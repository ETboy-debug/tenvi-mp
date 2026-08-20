// MPClient.cpp -  socket
// [v37] Mouse click + Tab toggle field. No Ctrl hotkeys, no Y-guessing.
//       : ,  DirectInput
//       : , (/)
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <windows.h>
#include "MPClient.h"
#include "../Share/Simple/Simple.h"

#pragma comment(lib, "ws2_32.lib")

#define MP_INI_NAME L"RunEmuTenvi"

// ---- ( v28) ----
static SOCKET g_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_cs;
static bool g_csReady = false;
static std::vector<std::vector<BYTE>> g_inQueue;
// [v50] Parallel queue holding the dispatch context of each entry in
// g_inQueue. 1 = CWvsContext (local player), 0 = CField (remote/objects).
static std::vector<BYTE> g_inCtxQueue;
static std::vector<std::vector<BYTE>> g_ctrlQueue;
static void MP_PushPacket(const BYTE *p, DWORD n, bool ctx);
static std::string g_ip = "127.0.0.1";
static int g_port = 8787;
static volatile LONG g_connected = 0;
static volatile LONG g_authed = 0;

// ----  ctrl  ----
bool MP_WaitCtrlResult(BYTE expectCmd, int timeoutMs, BYTE &outByte) {
	DWORD start = GetTickCount();
	while (true) {
		EnterCriticalSection(&g_cs);
		for (size_t i = 0; i < g_ctrlQueue.size(); ) {
			std::vector<BYTE> &pkt = g_ctrlQueue[i];
			if (pkt.size() >= 2 && pkt[1] == expectCmd) {
				outByte = (pkt.size() >= 3) ? pkt[2] : 1;
				g_ctrlQueue.erase(g_ctrlQueue.begin() + i);
				LeaveCriticalSection(&g_cs);
				return true;
			}
			++i;
		}
		LeaveCriticalSection(&g_cs);
		if (timeoutMs == 0) return false;
		if (GetTickCount() - start > (DWORD)timeoutMs) return false;
		Sleep(30);
	}
}

// [v82] Deployment sentinel string (grep-friendly binary marker).
static const char *MP_VERSION_TAG = "MP_CLIENT_V83_MOVE_MEMWRITE";

// [v61-diag] Per-process diagnostic log path. See MPClient.h for why a shared
// filename made the v60 client logs unreadable.
const char *MP_DiagPath() {
	static char s_path[64] = { 0 };
	if (s_path[0] == 0) {
		sprintf_s(s_path, sizeof(s_path), "D:/mp_diag_%lu.log",
			(unsigned long)GetCurrentProcessId());
	}
	return s_path;
}

// ----  ----
bool MP_IsConnected() { return g_connected != 0; }

static void MP_SendRaw(BYTE type, const BYTE *p, DWORD n) {
	if (g_sock == INVALID_SOCKET || !g_connected) return;
	DWORD len = n + 1;
	std::vector<BYTE> frame;
	frame.push_back((BYTE)(len & 0xFF));
	frame.push_back((BYTE)((len >> 8) & 0xFF));
	frame.push_back((BYTE)((len >> 16) & 0xFF));
	frame.push_back((BYTE)((len >> 24) & 0xFF));
	frame.push_back(type);
	if (n && p) frame.insert(frame.end(), p, p + n);
	int sent = send(g_sock, (const char *)&frame[0], (int)frame.size(), 0);
	if (sent == SOCKET_ERROR) {
		DEBUG(L"MP send failed");
		InterlockedExchange(&g_connected, 0);
	}
}

void MP_SendGame(const BYTE *p, DWORD n) {
	if (!p || !n) return;
	MP_SendRaw(MP_TYPE_GAME, p, n);
}

void MP_SendCtrl(BYTE cmd) { MP_SendRaw(MP_TYPE_CTRL, &cmd, 1); }

void MP_SendLogin(const std::string &acc, const std::string &pw) {
	std::vector<BYTE> buf;
	buf.push_back(MP_CTRL_LOGIN);
	for (char c : acc) buf.push_back((BYTE)c);
	buf.push_back(0);
	for (char c : pw) buf.push_back((BYTE)c);
	if (buf.size() > 1) MP_SendRaw(MP_TYPE_CTRL, &buf[0], (DWORD)buf.size());
}

bool MP_IsAuthed() { return g_authed == 1; }
void MP_SetAuthed(bool v) { InterlockedExchange(&g_authed, v ? 1 : -1); }

bool MP_PopPacket(std::vector<BYTE> &out) {
	bool ctx = true;
	return MP_PopPacketEx(out, ctx);
}

// [v50] Pop a packet together with the dispatch context the server tagged it
// with. Both queues are always pushed/popped in lockstep under g_cs.
bool MP_PopPacketEx(std::vector<BYTE> &out, bool &ctx) {
	if (!g_csReady) return false;
	bool got = false;
	EnterCriticalSection(&g_cs);
	if (!g_inQueue.empty()) {
		out = g_inQueue.front();
		g_inQueue.erase(g_inQueue.begin());
		if (!g_inCtxQueue.empty()) {
			ctx = (g_inCtxQueue.front() != 0);
			g_inCtxQueue.erase(g_inCtxQueue.begin());
		} else {
			ctx = true; // defensive: should never happen
		}
		got = true;
	}
	LeaveCriticalSection(&g_cs);
	return got;
}

static void MP_PushPacket(const BYTE *p, DWORD n, bool ctx) {
	if (!g_csReady || !n) return;
	std::vector<BYTE> pkt(p, p + n);
	EnterCriticalSection(&g_cs);
	g_inQueue.push_back(pkt);
	g_inCtxQueue.push_back(ctx ? 1 : 0);
	LeaveCriticalSection(&g_cs);
}

// ============================================================
// [v36] GetAsyncKeyState 
// ============================================================
// : GetAsyncKeyState  Windows
//        DirectInput  API 
//       
//
//       :
//         WH_GETMESSAGE      -> DirectInput  WM_KEYDOWN, 
//         WH_KEYBOARD_LL     -> , 
//         WH_KEYBOARD_LL+-> ( DI )
//         GetAsyncKeyState   -> ,  

static volatile LONG g_captureRunning = 0;   // 
static volatile LONG g_captureEnabled = 0;   // (MP_StartCapture/Stop)
static DWORD   g_gamePid = 0;               // ID()
static std::string g_capAccount;            // 
static std::string g_capPassword;           // 
static int      g_capField = 0;             // : 0= 1=
static CRITICAL_SECTION g_capCs;            // 
static bool     g_capCsReady = false;

// [v38] One-way field switch: first Tab/click -> lock to password. 2nd Tab -> unlock back.
static LONG     g_lastClickY = -1;          // (reserved)
static const int CLICK_Y_THRESHOLD = 30;    // (reserved)
static bool     g_capLocked = false;        // true = field locked after 1st switch

// (: )
static BYTE g_prevKeys[256];

// [v54 MARK] version-explicit-field-switch (deployment sentinel string)
// v52/v53 time-pause heuristic misfired on normal typing pauses inside the
// account name. Now: TAB / mouse click switch to password field one-way,
// but only after the account buffer is non-empty.

// [v54]  TAB / (), 

// 
static bool IsPrintableChar(WCHAR ch) {
	// ASCII  + 
	if (ch >= 0x20 && ch < 0x7F) return true;  // ~DEL
	return false;
}

// ( Shift/CapsLock )
static int VkToChar(UINT vk, WCHAR &outCh) {
	BYTE keyState[256] = {};
	//  GetKeyboardState  Shift/CapsLock/Ctrl 
	GetKeyboardState(keyState);

	// ToUnicode 
	WCHAR buf[4] = {};
	UINT scanCode = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
	int ret = ToUnicode(vk, scanCode, keyState, buf, 4, 0);
	if (ret == 1 && IsPrintableChar(buf[0])) {
		outCh = buf[0];
		return 1;
	}
	return 0;
}

//  UTF-8 
static void AppendChar(std::string &target, WCHAR ch) {
	char mb[8] = {};
	int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, sizeof(mb), NULL, NULL);
	if (n > 0) target.append(mb, n);
}

// : ~100Hz  GetAsyncKeyState
static DWORD WINAPI CaptureThread(LPVOID) {
	// 
	memset(g_prevKeys, 0, sizeof(g_prevKeys));
	int diagCounter = 0;

	while (InterlockedCompareExchange(&g_captureRunning, 0, 0)) {
		// (ID, HWND)
		if (g_captureEnabled && g_gamePid != 0) {
			HWND fg = GetForegroundWindow();
			if (fg) {
				DWORD fgPid = 0;
				GetWindowThreadProcessId(fg, &fgPid);
				if (fgPid == g_gamePid) {
					// [DIAG] 5
					diagCounter++;
					if (diagCounter % 500 == 0) {  // 500 * 10ms = 5s
						FILE *df = NULL; fopen_s(&df, MP_DiagPath(), "a");
						if (df) {
							fprintf(df, "[MP-CAP] active: acc=%d chars pw=%d chars field=%d\n",
								(int)g_capAccount.size(), (int)g_capPassword.size(), g_capField);
							fflush(df); fclose(df);
						}
					}

				// (0x01)
				for (UINT vk = 0x01; vk <= 0xFE; vk++) {
					SHORT state = GetAsyncKeyState(vk);
					bool nowDown = (state & 0x8000) != 0;
					bool wasDown = (g_prevKeys[vk] & 0x80) != 0;

					// ""()
					if (nowDown && !wasDown) {
						// [v54] : TAB / (),
						// ****  /
						// TAB()
						// , /TAB = ", "
						// v52/v53 (>300ms),
						// 
						if (vk == VK_TAB) {
							EnterCriticalSection(&g_capCs);
							if (!g_capLocked && g_capField == 0 && !g_capAccount.empty()) {
								g_capField = 1; g_capLocked = true;
							}
							LeaveCriticalSection(&g_capCs);
							{ FILE *df = NULL; fopen_s(&df, MP_DiagPath(), "a");
							  if (df) { fprintf(df, "[MP-CAP] Tab -> field=%d locked=%d\n", g_capField, (int)g_capLocked); fflush(df); fclose(df); } }
						}
					// [v54] Mouse click: same one-way switch, only if account has content.
					else if (vk == VK_LBUTTON) {
							EnterCriticalSection(&g_capCs);
							if (!g_capLocked && g_capField == 0 && !g_capAccount.empty()) {
								g_capField = 1; g_capLocked = true;
							}
							LeaveCriticalSection(&g_capCs);
							{ FILE *df = NULL; fopen_s(&df, MP_DiagPath(), "a");
							  if (df) { fprintf(df, "[MP-CAP] mouse click -> field=%d locked=%d\n", g_capField, (int)g_capLocked); fflush(df); fclose(df); } }
					}
						// --- Backspace:  ---
						else if (vk == VK_BACK) {
							EnterCriticalSection(&g_capCs);
							std::string &target = (g_capField == 0) ? g_capAccount : g_capPassword;
							if (!target.empty()) {
								size_t len = target.size();
								while (len > 0 && (target[len - 1] & 0xC0) == 0x80) len--;
								if (len > 0) len--;
								target.resize(len);
							}
							LeaveCriticalSection(&g_capCs);
						}
						// --- Delete:  ---
						else if (vk == VK_DELETE) {
							EnterCriticalSection(&g_capCs);
							std::string &target = (g_capField == 0) ? g_capAccount : g_capPassword;
							target.clear();
							LeaveCriticalSection(&g_capCs);
						}
						// --- Enter/Escape: () ---
						else if (vk == VK_RETURN || vk == VK_ESCAPE) {
							// ignore
						}
						// --- :  ---
						else {
							WCHAR ch = 0;
							if (VkToChar(vk, ch)) {
								EnterCriticalSection(&g_capCs);
								std::string &target = (g_capField == 0) ? g_capAccount : g_capPassword;
								AppendChar(target, ch);
								if (target.size() > 64) target.resize(64);
								LeaveCriticalSection(&g_capCs);
								// [DIAG] 
								{ FILE *df = NULL; fopen_s(&df, MP_DiagPath(), "a");
								  if (df) { fprintf(df, "[MP-CAP] key vk=%02X ch='%c' field=%d acc='%s'\n", vk, (char)ch, g_capField, g_capAccount.c_str()); fflush(df); fclose(df); } }
							}
						}
					} // end if newly pressed

					// 
					g_prevKeys[vk] = nowDown ? 0x80 : 0;
				} // end for each vk
			} // end if same process
		} // end if foreground valid
	} // end if capture enabled

		Sleep(10); // ~100Hz poll rate
	} // end while running

	DEBUG(L"[MP-CAP] capture thread exited");
	return 0;
}

// ----  API ( AutoResponse.cpp ) ----

/// 
void MP_StartCapture() {
	if (!g_capCsReady) {
		InitializeCriticalSection(&g_capCs);
		g_capCsReady = true;
	}
	g_gamePid = GetCurrentProcessId();
	g_capAccount.clear();
	g_capPassword.clear();
	g_capField = 0;
	g_capLocked = false;
	g_lastClickY = -1;  // (reserved)
	memset(g_prevKeys, 0, sizeof(g_prevKeys));

	if (InterlockedCompareExchange(&g_captureRunning, 0, 0) == 0) {
		InterlockedExchange(&g_captureEnabled, 1);
		InterlockedExchange(&g_captureRunning, 1);
		HANDLE hThread = CreateThread(NULL, 0, CaptureThread, NULL, 0, NULL);
		if (hThread) CloseHandle(hThread);
		DEBUG(L"[MP-CAP] started, pid=%u", g_gamePid);
	} else {
		InterlockedExchange(&g_captureEnabled, 1);
		DEBUG(L"[MP-CAP] re-enabled");
	}
}

/// 
void MP_StopCapture() {
	InterlockedExchange(&g_captureEnabled, 0);
	DEBUG(L"[MP-CAP] stopped");
}

///  true 
/// [out] acc = (UTF-8), pw = (UTF-8)
bool MP_GetNativeCred(std::string &acc, std::string &pw) {
	if (!g_capCsReady) return false;
	EnterCriticalSection(&g_capCs);
	acc = g_capAccount;
	pw = g_capPassword;
	bool hasAcc = !g_capAccount.empty();
	LeaveCriticalSection(&g_capCs);
	return hasAcc;
}

/// ()
void MP_ClearCred() {
	if (!g_capCsReady) return;
	EnterCriticalSection(&g_capCs);
	g_capAccount.clear();
	g_capPassword.clear();
	g_capField = 0;
	g_capLocked = false;
	LeaveCriticalSection(&g_capCs);
}

/// ()
void MP_ResetLoginState() {
	MP_ClearCred();
	InterlockedExchange(&g_authed, 0);
}

// [v33] : ( MP_Thread )
static DWORD WINAPI MP_RecvThread(LPVOID);

// ============================================================
// [v33] : ("")
// ============================================================
bool MP_Reconnect() {
	// , 
	if (g_sock != INVALID_SOCKET && g_connected) {
		DEBUG(L"[MP] Reconnect: already connected");
		return true;
	}

	//  socket
	if (g_sock != INVALID_SOCKET) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}
	InterlockedExchange(&g_connected, 0);

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

	char portStr[16] = {};
	sprintf_s(portStr, sizeof(portStr), "%d", g_port);

	// 30(1, 30)
	for (int attempt = 0; attempt < 30; attempt++) {
		addrinfo hints = {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		addrinfo *res = NULL;
		if (getaddrinfo(g_ip.c_str(), portStr, &hints, &res) != 0 || !res) {
			Sleep(1000); continue;
		}

		bool ok = false;
		for (addrinfo *ai = res; ai; ai = ai->ai_next) {
			g_sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (g_sock == INVALID_SOCKET) continue;
			if (connect(g_sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) { ok = true; break; }
			closesocket(g_sock);
			g_sock = INVALID_SOCKET;
		}
		freeaddrinfo(res);
		if (ok) break;
		Sleep(1000);
	}

	if (g_sock == INVALID_SOCKET) {
		DEBUG(L"[MP] Reconnect: failed after 30 attempts");
		return false;
	}

	int flag = 1;
	setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
	InterlockedExchange(&g_connected, 1);
	DEBUG(L"[MP] Reconnect: connected to %s:%d", g_ip.c_str(), g_port);

	//  HELLO 
	MP_SendCtrl(MP_CTRL_HELLO);

	// ( MP_Thread , )
	//  MP_Thread 
	// : 
	HANDLE hThread = CreateThread(NULL, 0, MP_RecvThread, NULL, 0, NULL);
	if (hThread) CloseHandle(hThread);

	return true;
}

// ============================================================
// : ( MP_Thread  MP_Reconnect )
// ============================================================
static DWORD WINAPI MP_RecvThread(LPVOID) {
	DEBUG(L"[MP] RecvThread started");
	// 
	std::vector<BYTE> buf;
	char tmp[16384];
	while (true) {
		int n = recv(g_sock, tmp, sizeof(tmp), 0);
		if (n <= 0) break;
		buf.insert(buf.end(), tmp, tmp + n);
		while (buf.size() >= 4) {
			DWORD len = (DWORD)buf[0] | ((DWORD)buf[1] << 8) | ((DWORD)buf[2] << 16) | ((DWORD)buf[3] << 24);
			if (len == 0 || len > 65536) { buf.clear(); break; }
			if (buf.size() < 4 + len) break;
			BYTE type = buf[4];
			if (type == MP_TYPE_GAME && len > 1) {
				MP_PushPacket(&buf[5], len - 1, true);   // -> CWvsContext
			} else if (type == MP_TYPE_GAME_FIELD && len > 1) {
				MP_PushPacket(&buf[5], len - 1, false);  // [v50] -> CField
			} else if (type == MP_TYPE_CTRL && len >= 2) {
				std::vector<BYTE> cpkt(buf.begin() + 4, buf.begin() + 4 + len);
				EnterCriticalSection(&g_cs);
				g_ctrlQueue.push_back(cpkt);
				LeaveCriticalSection(&g_cs);
			}
			buf.erase(buf.begin(), buf.begin() + 4 + len);
		}
	}

	InterlockedExchange(&g_connected, 0);
	closesocket(g_sock);
	g_sock = INVALID_SOCKET;
	DEBUG(L"[MP] disconnected");
	return 0;
}

// ============================================================
// : DLL(120)
// ============================================================
static DWORD WINAPI MP_Thread(LPVOID) {
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		DEBUG(L"MP WSAStartup failed");
		return 0;
	}

	char portStr[16] = {};
	sprintf_s(portStr, sizeof(portStr), "%d", g_port);

	for (int attempt = 0; attempt < 120; attempt++) {
		addrinfo hints = {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		addrinfo *res = NULL;
		if (getaddrinfo(g_ip.c_str(), portStr, &hints, &res) != 0 || !res) {
			Sleep(1000); continue;
		}

		bool ok = false;
		for (addrinfo *ai = res; ai; ai = ai->ai_next) {
			g_sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (g_sock == INVALID_SOCKET) continue;
			if (connect(g_sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) { ok = true; break; }
			closesocket(g_sock);
			g_sock = INVALID_SOCKET;
		}
		freeaddrinfo(res);
		if (ok) break;
		Sleep(1000);
	}

	if (g_sock == INVALID_SOCKET) {
		DEBUG(L"MP connect failed");
		return 0;
	}

	int flag = 1;
	setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));

	InterlockedExchange(&g_connected, 1);
	DEBUG(L"MP connected");
	MP_SendCtrl(MP_CTRL_HELLO);

	// 
	HANDLE hRecv = CreateThread(NULL, 0, MP_RecvThread, NULL, 0, NULL);
	if (hRecv) CloseHandle(hRecv);

	return 0;
}

// ----  ----
bool MP_Start(HINSTANCE hinstDLL) {
	if (!g_csReady) {
		InitializeCriticalSection(&g_cs);
		g_csReady = true;
	}

	//  ini ( ini)
	Config conf(MP_INI_NAME L".ini", hinstDLL);
	std::wstring wIP, wPort;
	if (conf.Read(MP_INI_NAME, L"ServerIP", wIP) && wIP.length()) {
		char abuf[64] = {};
		WideCharToMultiByte(CP_ACP, 0, wIP.c_str(), -1, abuf, sizeof(abuf) - 1, NULL, NULL);
		g_ip = abuf;
	}
	if (conf.Read(MP_INI_NAME, L"ServerPort", wPort) && wPort.length()) {
		int p = _wtoi(wPort.c_str());
		if (p > 0 && p < 65536) g_port = p;
	}
	DEBUG(L"[MP] server=%s:%d (v33 reconnect-on-login)", g_ip.c_str(), g_port);

	HANDLE hThread = CreateThread(NULL, 0, MP_Thread, NULL, 0, NULL);
	if (hThread) { CloseHandle(hThread); return true; }
	return false;
}

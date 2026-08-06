// MPClient.cpp - 客户端侧明文桥接 socket
// [v40] Mouse click: 1st click -> account, 2nd click -> password, 3rd+ ignored.
//       Tab toggles both ways.
//       键盘: 内核级查询, 绕过 DirectInput
//       鼠标: 按点击次数切字段, 不再依赖 Y 坐标差
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

// ---- 网络基础设施(同 v28) ----
static SOCKET g_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_cs;
static bool g_csReady = false;
static std::vector<std::vector<BYTE>> g_inQueue;
static std::vector<std::vector<BYTE>> g_ctrlQueue;
static void MP_PushPacket(const BYTE *p, DWORD n);
static std::string g_ip = "127.0.0.1";
static int g_port = 8787;
static volatile LONG g_connected = 0;
static volatile LONG g_authed = 0;

// ---- 同步等待服务端 ctrl 结果 ----
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

// ---- 网络发送 ----
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
	if (!g_csReady) return false;
	bool got = false;
	EnterCriticalSection(&g_cs);
	if (!g_inQueue.empty()) { out = g_inQueue.front(); g_inQueue.erase(g_inQueue.begin()); got = true; }
	LeaveCriticalSection(&g_cs);
	return got;
}

static void MP_PushPacket(const BYTE *p, DWORD n) {
	if (!g_csReady || !n) return;
	std::vector<BYTE> pkt(p, p + n);
	EnterCriticalSection(&g_cs);
	g_inQueue.push_back(pkt);
	LeaveCriticalSection(&g_cs);
}

// ============================================================
// [v36] GetAsyncKeyState 轮询键盘捕获
// ============================================================
// 原理: GetAsyncKeyState 直接查内核键盘状态表，不经过 Windows
//       消息循环。即使游戏用 DirectInput 独占键盘，这个 API 仍然
//       能正确返回按键状态（因为它读的是驱动层状态）。
//
//       对比之前失败的方案:
//         WH_GETMESSAGE      -> DirectInput 不走 WM_KEYDOWN, 抓不到
//         WH_KEYBOARD_LL     -> 需要消息泵驱动回调, 游戏主线程无泵
//         WH_KEYBOARD_LL+独立线程-> 回调仍不触发(原因可能是 DI 低级过滤)
//         GetAsyncKeyState   -> 内核级查询, 不依赖任何消息机制 ✅

static volatile LONG g_captureRunning = 0;   // 捕获线程运行标志
static volatile LONG g_captureEnabled = 0;   // 是否启用捕获(MP_StartCapture/Stop)
static DWORD   g_gamePid = 0;               // 游戏进程ID(用于前台窗口归属检查)
static std::string g_capAccount;            // 捕获的账号
static std::string g_capPassword;           // 捕获的密码
static int      g_capField = 0;             // 当前输入字段: 0=账号 1=密码
static CRITICAL_SECTION g_capCs;            // 保护捕获缓冲区
static bool     g_capCsReady = false;

// [v40] Track click count to switch fields. 1st click = account, 2nd click = password,
//        3rd+ ignored (login button). Tab still toggles both ways.
static int      g_clickCount = 0;           // 当前捕获周期内鼠标左键点击次数

// 上一次的按键状态(用于检测边沿: 新按下)
static BYTE g_prevKeys[256];

// 可打印字符范围判断
static bool IsPrintableChar(WCHAR ch) {
	// ASCII 可打印字符 + 常见拉丁扩展
	if (ch >= 0x20 && ch < 0x7F) return true;  // 空格~DEL
	return false;
}

// 将虚拟键码转换为宽字符(处理 Shift/CapsLock 状态)
static int VkToChar(UINT vk, WCHAR &outCh) {
	BYTE keyState[256] = {};
	// 从 GetKeyboardState 获取当前 Shift/CapsLock/Ctrl 等状态
	GetKeyboardState(keyState);

	// ToUnicode 需要的参数
	WCHAR buf[4] = {};
	UINT scanCode = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
	int ret = ToUnicode(vk, scanCode, keyState, buf, 4, 0);
	if (ret == 1 && IsPrintableChar(buf[0])) {
		outCh = buf[0];
		return 1;
	}
	return 0;
}

// 将宽字符转为 UTF-8 字符串追加到目标缓冲区
static void AppendChar(std::string &target, WCHAR ch) {
	char mb[8] = {};
	int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, sizeof(mb), NULL, NULL);
	if (n > 0) target.append(mb, n);
}

// 捕获线程主函数: ~100Hz 轮询 GetAsyncKeyState
static DWORD WINAPI CaptureThread(LPVOID) {
	// 初始化上一次状态为全释放
	memset(g_prevKeys, 0, sizeof(g_prevKeys));
	int diagCounter = 0;

	while (InterlockedCompareExchange(&g_captureRunning, 0, 0)) {
		// 只在游戏窗口前台时捕获(通过进程ID判断, 不依赖固定HWND)
		if (g_captureEnabled && g_gamePid != 0) {
			HWND fg = GetForegroundWindow();
			if (fg) {
				DWORD fgPid = 0;
				GetWindowThreadProcessId(fg, &fgPid);
				if (fgPid == g_gamePid) {
					// [DIAG] 每5秒记录一次捕获活跃状态
					diagCounter++;
					if (diagCounter % 500 == 0) {  // 500 * 10ms = 5s
						FILE *df = NULL; fopen_s(&df, "D:/mp_diag.log", "a");
						if (df) {
							fprintf(df, "[MP-CAP] active: acc=%d chars pw=%d chars field=%d\n",
								(int)g_capAccount.size(), (int)g_capPassword.size(), g_capField);
							fflush(df); fclose(df);
						}
					}

				// 扫描所有虚拟键(从0x01开始以包含鼠标按键)
				for (UINT vk = 0x01; vk <= 0xFE; vk++) {
					SHORT state = GetAsyncKeyState(vk);
					bool nowDown = (state & 0x8000) != 0;
					bool wasDown = (g_prevKeys[vk] & 0x80) != 0;

					// 只处理"新按下"的边沿(避免重复触发)
					if (nowDown && !wasDown) {
					// --- [v39] Tab: simple toggle between account and password ---
					if (vk == VK_TAB) {
						EnterCriticalSection(&g_capCs);
						g_capField = (g_capField == 0) ? 1 : 0;
						int newField = g_capField;
						LeaveCriticalSection(&g_capCs);
						{ FILE *df = NULL; fopen_s(&df, "D:/mp_diag.log", "a");
						  if (df) { fprintf(df, "[MP-CAP] Tab -> field=%d\n", newField); fflush(df); fclose(df); } }
					}
					// [v40] Mouse click: 1st click -> account, 2nd click -> password, 3rd+ ignored.
					else if (vk == VK_LBUTTON) {
						EnterCriticalSection(&g_capCs);
						g_clickCount++;
						int newField = g_capField;
						if (g_clickCount == 1) {
							g_capField = 0;
							newField = 0;
						} else if (g_clickCount == 2) {
							g_capField = 1;
							newField = 1;
						}
						LeaveCriticalSection(&g_capCs);
						{ FILE *df = NULL; fopen_s(&df, "D:/mp_diag.log", "a");
						  if (df) { fprintf(df, "[MP-CAP] mouse click count=%d -> field=%d\n", g_clickCount, newField); fflush(df); fclose(df); } }
					}
						// --- Backspace: 删除末尾字符 ---
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
						// --- Delete: 清空当前字段 ---
						else if (vk == VK_DELETE) {
							EnterCriticalSection(&g_capCs);
							std::string &target = (g_capField == 0) ? g_capAccount : g_capPassword;
							target.clear();
							LeaveCriticalSection(&g_capCs);
						}
						// --- Enter/Escape: 不处理(留给游戏) ---
						else if (vk == VK_RETURN || vk == VK_ESCAPE) {
							// ignore
						}
						// --- 其他键: 尝试转换为可打印字符 ---
						else {
							WCHAR ch = 0;
							if (VkToChar(vk, ch)) {
								EnterCriticalSection(&g_capCs);
								std::string &target = (g_capField == 0) ? g_capAccount : g_capPassword;
								AppendChar(target, ch);
								if (target.size() > 64) target.resize(64);
								LeaveCriticalSection(&g_capCs);
								// [DIAG] 记录捕获到的字符
								{ FILE *df = NULL; fopen_s(&df, "D:/mp_diag.log", "a");
								  if (df) { fprintf(df, "[MP-CAP] key vk=%02X ch='%c' field=%d acc='%s'\n", vk, (char)ch, g_capField, g_capAccount.c_str()); fflush(df); fclose(df); } }
							}
						}
					} // end if newly pressed

					// 更新上一次状态
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

// ---- 捕获 API (供 AutoResponse.cpp 调用) ----

/// 启动键盘捕获。应在游戏窗口创建后调用。
void MP_StartCapture() {
	if (!g_capCsReady) {
		InitializeCriticalSection(&g_capCs);
		g_capCsReady = true;
	}
	g_gamePid = GetCurrentProcessId();
	g_capAccount.clear();
	g_capPassword.clear();
	g_capField = 0;
	g_clickCount = 0;
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

/// 停止键盘捕获
void MP_StopCapture() {
	InterlockedExchange(&g_captureEnabled, 0);
	DEBUG(L"[MP-CAP] stopped");
}

/// 取出捕获到的凭据。返回 true 表示有内容。
/// [out] acc = 账号(UTF-8), pw = 密码(UTF-8)
bool MP_GetNativeCred(std::string &acc, std::string &pw) {
	if (!g_capCsReady) return false;
	EnterCriticalSection(&g_capCs);
	acc = g_capAccount;
	pw = g_capPassword;
	bool hasAcc = !g_capAccount.empty();
	LeaveCriticalSection(&g_capCs);
	return hasAcc;
}

/// 清空捕获的凭据(认证成功后调用)
void MP_ClearCred() {
	if (!g_capCsReady) return;
	EnterCriticalSection(&g_capCs);
	g_capAccount.clear();
	g_capPassword.clear();
	g_capField = 0;
	g_clickCount = 0;
	LeaveCriticalSection(&g_capCs);
}

/// 重置登录状态(用于重试)
void MP_ResetLoginState() {
	MP_ClearCred();
	InterlockedExchange(&g_authed, 0);
}

// [v33] 前向声明: 收包线程(定义在 MP_Thread 之后)
static DWORD WINAPI MP_RecvThread(LPVOID);

// ============================================================
// [v33] 同步重连: 登录前按需连接(解决"开游戏时服务端没启动导致连接线程退出"的问题)
// ============================================================
bool MP_Reconnect() {
	// 如果已经连上了, 不需要重连
	if (g_sock != INVALID_SOCKET && g_connected) {
		DEBUG(L"[MP] Reconnect: already connected");
		return true;
	}

	// 清理旧 socket
	if (g_sock != INVALID_SOCKET) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}
	InterlockedExchange(&g_connected, 0);

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

	char portStr[16] = {};
	sprintf_s(portStr, sizeof(portStr), "%d", g_port);

	// 尝试30次(每次1秒, 共30秒足够)
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

	// 发送 HELLO 握手
	MP_SendCtrl(MP_CTRL_HELLO);

	// 启动收包线程(复用原来的 MP_Thread 收包逻辑, 但跳过连接部分)
	// 用一个标志告诉 MP_Thread 跳过连接直接进入收包循环
	// 简单方案: 直接创建新的收包线程
	HANDLE hThread = CreateThread(NULL, 0, MP_RecvThread, NULL, 0, NULL);
	if (hThread) CloseHandle(hThread);

	return true;
}

// ============================================================
// 收包线程: 只做收包转发(由 MP_Thread 或 MP_Reconnect 启动)
// ============================================================
static DWORD WINAPI MP_RecvThread(LPVOID) {
	DEBUG(L"[MP] RecvThread started");
	// 收包循环
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
				MP_PushPacket(&buf[5], len - 1);
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
// 连接线程: DLL启动时调用(后台重试120次)
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

	// 启动独立收包线程
	HANDLE hRecv = CreateThread(NULL, 0, MP_RecvThread, NULL, 0, NULL);
	if (hRecv) CloseHandle(hRecv);

	return 0;
}

// ---- 启动入口 ----
bool MP_Start(HINSTANCE hinstDLL) {
	if (!g_csReady) {
		InitializeCriticalSection(&g_cs);
		g_csReady = true;
	}

	// 从 ini 只读取服务器地址和端口(账号密码不再经过 ini)
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

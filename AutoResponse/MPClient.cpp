// MPClient.cpp - 客户端侧明文桥接 socket
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include "MPClient.h"
#include "../Share/Simple/Simple.h"

#pragma comment(lib, "ws2_32.lib")

#define MP_INI_NAME L"RunEmuTenvi"

static SOCKET g_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_cs;
static bool g_csReady = false;
static std::vector<std::vector<BYTE>> g_inQueue;
static std::string g_ip = "127.0.0.1";
static int g_port = 8787;
static std::string g_account = "Player"; // [MP] 账号名(utf8), 由游戏内弹窗提供, ini Account 兜底
static std::string g_password;            // [MP] 密码(utf8), 由弹窗提供
static volatile LONG g_authed = 0;        // [MP] 弹窗认证成功标志(门控原生登录)
static volatile LONG g_connected = 0;

bool MP_IsConnected() {
	return g_connected != 0;
}

// 组帧后发送
static void MP_SendRaw(BYTE type, const BYTE *p, DWORD n) {
	if (g_sock == INVALID_SOCKET || !g_connected) {
		return;
	}
	DWORD len = n + 1; // type 占 1 字节
	std::vector<BYTE> frame;
	frame.push_back((BYTE)(len & 0xFF));
	frame.push_back((BYTE)((len >> 8) & 0xFF));
	frame.push_back((BYTE)((len >> 16) & 0xFF));
	frame.push_back((BYTE)((len >> 24) & 0xFF));
	frame.push_back(type);
	if (n && p) {
		frame.insert(frame.end(), p, p + n);
	}
	int sent = send(g_sock, (const char *)&frame[0], (int)frame.size(), 0);
	if (sent == SOCKET_ERROR) {
		DEBUG(L"MP send failed");
		InterlockedExchange(&g_connected, 0);
	}
}

void MP_SendGame(const BYTE *p, DWORD n) {
	if (!p || !n) {
		return;
	}
	MP_SendRaw(MP_TYPE_GAME, p, n);
}

void MP_SendCtrl(BYTE cmd) {
	MP_SendRaw(MP_TYPE_CTRL, &cmd, 1);
}

// [MP] 账号\0密码 组帧发送(注册/登录共用)
static void MP_SendCred(BYTE cmd, const std::string &acc, const std::string &pw) {
	std::vector<BYTE> buf;
	buf.push_back(cmd);
	for (char c : acc) buf.push_back((BYTE)c);
	buf.push_back((BYTE)'\0');
	for (char c : pw) buf.push_back((BYTE)c);
	if (!buf.empty()) MP_SendRaw(MP_TYPE_CTRL, &buf[0], (DWORD)buf.size());
}

void MP_SendLogin(const std::string &acc, const std::string &pw) {
	MP_SendCred(MP_CTRL_LOGIN, acc, pw);
}

static void MP_SendRegister(const std::string &acc, const std::string &pw) {
	MP_SendCred(MP_CTRL_REGISTER, acc, pw);
}

bool MP_IsAuthed() {
	return g_authed != 0;
}

// ---- [MP] 游戏内登录/注册弹窗 ----
#define DLG_ACC    101
#define DLG_PW     102
#define DLG_LOGIN  103
#define DLG_REG    104
#define DLG_STATUS 105

struct AuthDlgCtx {
	bool done = false;
	bool authed = false;
};

static std::vector<BYTE> g_dlgBuf;

// 阻塞等待服务端 ctrl 结果; 期间收到的游戏包入队
static bool RecvCtrlResult(BYTE expectCmd, int timeoutMs, BYTE &outByte) {
	std::vector<BYTE> &buf = g_dlgBuf;
	DWORD start = GetTickCount();
	while (true) {
		fd_set fds; FD_ZERO(&fds); FD_SET(g_sock, &fds);
		timeval tv; tv.tv_sec = 0; tv.tv_usec = 150000;
		if (select(0, &fds, NULL, NULL, &tv) > 0) {
			char tmp[16384];
			int n = recv(g_sock, tmp, sizeof(tmp), 0);
			if (n <= 0) return false;
			buf.insert(buf.end(), tmp, tmp + n);
			while (buf.size() >= 4) {
				DWORD len = (DWORD)buf[0] | ((DWORD)buf[1] << 8) | ((DWORD)buf[2] << 16) | ((DWORD)buf[3] << 24);
				if (len == 0 || len > 65536) { buf.clear(); return false; }
				if (buf.size() < 4 + len) break;
				BYTE type = buf[4];
				if (type == MP_TYPE_CTRL && len >= 2) {
					BYTE cmd = buf[5];
					if (cmd == expectCmd) {
						outByte = (len >= 3) ? buf[6] : 1;
						buf.erase(buf.begin(), buf.begin() + 4 + len);
						return true;
					}
				} else if (type == MP_TYPE_GAME && len > 1) {
					MP_PushPacket(&buf[5], len - 1);
				}
				buf.erase(buf.begin(), buf.begin() + 4 + len);
			}
		}
		if (GetTickCount() - start > (DWORD)timeoutMs) return false;
	}
}

static LRESULT CALLBACK AuthDlgProc(HWND hDlg, UINT msg, WPARAM w, LPARAM l) {
	AuthDlgCtx *ctx = (AuthDlgCtx *)GetWindowLongPtr(hDlg, GWLP_USERDATA);
	switch (msg) {
	case WM_COMMAND: {
		int id = (int)LOWORD(w);
		if (id == IDCANCEL) { ctx->done = true; DestroyWindow(hDlg); return 0; }
		if (id == DLG_LOGIN || id == DLG_REG) {
			char acc[128] = {}, pw[128] = {};
			GetDlgItemTextA(hDlg, DLG_ACC, acc, sizeof(acc));
			GetDlgItemTextA(hDlg, DLG_PW, pw, sizeof(pw));
			bool reg = (id == DLG_REG);
			BYTE res = 0;
			if (reg) {
				MP_SendRegister(acc, pw);
				if (!RecvCtrlResult(MP_CTRL_REGISTER_RESULT, 6000, res)) { SetDlgItemTextA(hDlg, DLG_STATUS, "Register failed (net)"); return 0; }
			} else {
				MP_SendLogin(acc, pw);
				if (!RecvCtrlResult(MP_CTRL_LOGIN_RESULT, 6000, res)) { SetDlgItemTextA(hDlg, DLG_STATUS, "Login failed (net)"); return 0; }
			}
			if (res == 1) {
				g_account = acc; g_password = pw;
				if (!reg) {
					InterlockedExchange(&g_authed, 1);
					ctx->authed = true; ctx->done = true; DestroyWindow(hDlg);
				} else {
					SetDlgItemTextA(hDlg, DLG_STATUS, "Registered OK, click Login");
				}
			} else {
				SetDlgItemTextA(hDlg, DLG_STATUS, reg ? "Account exists" : "Wrong password");
			}
			return 0;
		}
		break;
	}
	case WM_CLOSE: ctx->done = true; DestroyWindow(hDlg); return 0;
	}
	return DefWindowProcA(hDlg, msg, w, l);
}

// 弹出登录/注册窗口, 阻塞到用户完成认证或取消
static void ShowAuthDialog() {
	AuthDlgCtx ctx;
	HINSTANCE hInst = GetModuleHandle(NULL);
	WNDCLASSEXA wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = AuthDlgProc;
	wc.hInstance = hInst;
	wc.lpszClassName = "TenviAuthCls";
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	RegisterClassExA(&wc);
	HWND hwnd = CreateWindowExA(0, "TenviAuthCls", "Tenvi - Login",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 360, 230,
		NULL, NULL, hInst, NULL);
	if (!hwnd) return;
	SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)&ctx);
	CreateWindowExA(0, "STATIC", "Account", WS_CHILD | WS_VISIBLE, 20, 14, 80, 18, hwnd, NULL, hInst, NULL);
	CreateWindowExA(0, "STATIC", "Password", WS_CHILD | WS_VISIBLE, 20, 54, 80, 18, hwnd, NULL, hInst, NULL);
	CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 20, 32, 300, 22, hwnd, (HMENU)DLG_ACC, hInst, NULL);
	CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD, 20, 72, 300, 22, hwnd, (HMENU)DLG_PW, hInst, NULL);
	CreateWindowExA(0, "BUTTON", "Login", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 30, 110, 130, 30, hwnd, (HMENU)DLG_LOGIN, hInst, NULL);
	CreateWindowExA(0, "BUTTON", "Register", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 190, 110, 130, 30, hwnd, (HMENU)DLG_REG, hInst, NULL);
	CreateWindowExA(0, "STATIC", "Enter account and password", WS_CHILD | WS_VISIBLE, 20, 150, 300, 40, hwnd, (HMENU)DLG_STATUS, hInst, NULL);
	if (!g_account.empty() && g_account != "Player") SetDlgItemTextA(hwnd, DLG_ACC, g_account.c_str());
	MSG m;
	while (!ctx.done) {
		if (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m); DispatchMessageA(&m);
			if (m.message == WM_QUIT) { ctx.done = true; break; }
		} else Sleep(10);
	}
	DestroyWindow(hwnd);
}

bool MP_PopPacket(std::vector<BYTE> &out) {
	if (!g_csReady) {
		return false;
	}
	bool got = false;
	EnterCriticalSection(&g_cs);
	if (!g_inQueue.empty()) {
		out = g_inQueue.front();
		g_inQueue.erase(g_inQueue.begin());
		got = true;
	}
	LeaveCriticalSection(&g_cs);
	return got;
}

static void MP_PushPacket(const BYTE *p, DWORD n) {
	if (!g_csReady || !n) {
		return;
	}
	std::vector<BYTE> pkt(p, p + n);
	EnterCriticalSection(&g_cs);
	g_inQueue.push_back(pkt);
	LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI MP_Thread(LPVOID) {
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		DEBUG(L"MP WSAStartup failed");
		return 0;
	}

	// 服务端可能比客户端启动晚, 这里重试等待。
	// 用 getaddrinfo 而不是 inet_addr, 这样服主填域名(DDNS)也能连。
	char portStr[16] = {};
	sprintf_s(portStr, sizeof(portStr), "%d", g_port);

	for (int attempt = 0; attempt < 120; attempt++) {
		addrinfo hints = {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		addrinfo *res = NULL;
		if (getaddrinfo(g_ip.c_str(), portStr, &hints, &res) != 0 || !res) {
			// 域名还没解析出来(断网/DNS慢), 等一秒再试
			Sleep(1000);
			continue;
		}

		bool ok = false;
		for (addrinfo *ai = res; ai; ai = ai->ai_next) {
			g_sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (g_sock == INVALID_SOCKET) {
				continue;
			}
			if (connect(g_sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
				ok = true;
				break;
			}
			closesocket(g_sock);
			g_sock = INVALID_SOCKET;
		}
		freeaddrinfo(res);

		if (ok) {
			break;
		}
		Sleep(1000);
	}

	if (g_sock == INVALID_SOCKET) {
		DEBUG(L"MP connect failed");
		return 0;
	}

	// 关掉 Nagle, 避免小包被合并延迟
	int flag = 1;
	setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));

	InterlockedExchange(&g_connected, 1);
	DEBUG(L"MP connected");
	MP_SendCtrl(MP_CTRL_HELLO);
	// [MP] 游戏内登录/注册弹窗: 收集账号密码并认证; 成功后服务端已建/载角色
	ShowAuthDialog();
	if (!MP_IsAuthed()) {
		DEBUG(L"MP auth cancelled/failed");
		InterlockedExchange(&g_connected, 0);
		closesocket(g_sock); g_sock = INVALID_SOCKET;
		return 0;
	}
	// 驱动原生流程(原生 LoginButton_Hook 已门控, 这里主动发 WORLDLIST)
	MP_SendCtrl(MP_CTRL_WORLDLIST);

	std::vector<BYTE> buf;
	char tmp[16384];
	while (true) {
		int n = recv(g_sock, tmp, sizeof(tmp), 0);
		if (n <= 0) {
			break;
		}
		buf.insert(buf.end(), tmp, tmp + n);
		while (buf.size() >= 4) {
			DWORD len = (DWORD)buf[0] | ((DWORD)buf[1] << 8) | ((DWORD)buf[2] << 16) | ((DWORD)buf[3] << 24);
			if (len == 0 || len > 65536) {
				DEBUG(L"MP bad frame length");
				buf.clear();
				break;
			}
			if (buf.size() < 4 + len) {
				break;
			}
			BYTE type = buf[4];
			if (type == MP_TYPE_GAME && len > 1) {
				MP_PushPacket(&buf[5], len - 1);
			}
			buf.erase(buf.begin(), buf.begin() + 4 + len);
		}
	}

	InterlockedExchange(&g_connected, 0);
	closesocket(g_sock);
	g_sock = INVALID_SOCKET;
	DEBUG(L"MP disconnected");
	return 0;
}

bool MP_Start(HINSTANCE hinstDLL) {
	if (!g_csReady) {
		InitializeCriticalSection(&g_cs);
		g_csReady = true;
	}

	// 从 RunEmuTenvi.ini 读服务器地址, 缺省 127.0.0.1:8787
	Config conf(MP_INI_NAME L".ini", hinstDLL);
	std::wstring wIP, wPort;
	if (conf.Read(MP_INI_NAME, L"ServerIP", wIP) && wIP.length()) {
		char abuf[64] = {};
		WideCharToMultiByte(CP_ACP, 0, wIP.c_str(), -1, abuf, sizeof(abuf) - 1, NULL, NULL);
		g_ip = abuf;
	}
	if (conf.Read(MP_INI_NAME, L"ServerPort", wPort) && wPort.length()) {
		int p = _wtoi(wPort.c_str());
		if (p > 0 && p < 65536) {
			g_port = p;
		}
	}
	// [MP] 读账号名(Account 字段, utf8 存盘)
	std::wstring wAcc;
	if (conf.Read(MP_INI_NAME, L"Account", wAcc) && wAcc.length()) {
		char abuf[128] = {};
		WideCharToMultiByte(CP_UTF8, 0, wAcc.c_str(), -1, abuf, sizeof(abuf) - 1, NULL, NULL);
		g_account = abuf;
	}
	if (g_account.empty()) g_account = "Player";

	HANDLE hThread = CreateThread(NULL, 0, MP_Thread, NULL, 0, NULL);
	if (hThread) {
		CloseHandle(hThread);
		return true;
	}
	return false;
}

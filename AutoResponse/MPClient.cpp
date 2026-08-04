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
static std::vector<std::vector<BYTE>> g_ctrlQueue; // ctrl 包队列(MP_Thread 填, 等待方轮询)
static void MP_PushPacket(const BYTE *p, DWORD n); // forward decl
static std::string g_ip = "127.0.0.1";
static int g_port = 8787;
static volatile LONG g_connected = 0;
static volatile LONG g_authed = 0;        // 0=未认证 1=成功 -1=失败

// ---- [MP] 原生登录界面凭据捕获（Windows 消息钩子）----
// 冲锋岛登录输入框是自定义渲染(DirectX), EnumChildWindows 读不到标准 EDIT 控件。
// 方案: 安装线程局部 WH_GETMESSAGE 钩子, 拦截 WM_CHAR/WM_KEYDOWN/WM_LBUTTONDOWN
//       来捕获用户在原生界面上输入的账号密码。

enum NatInputField {
	NAT_FIELD_ACCOUNT = 0,
	NAT_FIELD_PASSWORD,
};

static NatInputField g_natField = NAT_FIELD_ACCOUNT;
static std::string g_natAccount;
static std::string g_natPassword;
static HHOOK g_hMsgHook = NULL;
static HWND   g_hGameWnd = NULL;

static LRESULT CALLBACK NatMsgHookProc(int code, WPARAM wParam, LPARAM lParam) {
	if (code < 0) return CallNextHookEx(NULL, code, wParam, lParam);
	MSG *msg = (MSG*)lParam;
	if (!msg) return CallNextHookEx(NULL, code, wParam, lParam);

	switch (msg->message) {
	case WM_CHAR: {
		char c = (char)(msg->wParam & 0xFF);
		if (c >= 32 && c != 127) {
			if (g_natField == NAT_FIELD_ACCOUNT)
				g_natAccount += c;
			else
				g_natPassword += c;
		}
		break;
	}
	case WM_KEYDOWN: {
		VK vkey = (VK)(msg->wParam & 0xFF);
		if (vkey == VK_TAB) {
			g_natField = (g_natField == NAT_FIELD_ACCOUNT) ? NAT_FIELD_PASSWORD : NAT_FIELD_ACCOUNT;
		} else if (vkey == VK_BACK) {
			auto &buf = (g_natField == NAT_FIELD_ACCOUNT) ? g_natAccount : g_natPassword;
			if (!buf.empty()) buf.pop_back();
		}
		break;
	}
	case WM_LBUTTONDOWN: {
		POINT pt = { LOWORD(msg->lParam), HIWORD(msg->lParam) };
		if (g_hGameWnd) {
			RECT r; GetClientRect(g_hGameWnd, &r);
			if (pt.y > r.bottom / 2)
				g_natField = NAT_FIELD_PASSWORD;
			else
				g_natField = NAT_FIELD_ACCOUNT;
		}
		break;
	}
	}
	return CallNextHookEx(NULL, code, wParam, lParam);
}

static void NatInstallHook() {
	if (g_hMsgHook) return;
	g_hGameWnd = GetForegroundWindow();
	if (!g_hGameWnd) g_hGameWnd = GetActiveWindow();
	g_hMsgHook = SetWindowsHookEx(WH_GETMESSAGE, NatMsgHookProc, NULL, GetCurrentThread());
	if (g_hMsgHook)
		DEBUG(L"[MP] Native input hook installed");
	else
		DEBUG(L"[MP] WARNING: failed to install input hook");
}

static void NatUninstallHook() {
	if (g_hMsgHook) {
		UnhookWindowsHookEx(g_hMsgHook);
		g_hMsgHook = NULL;
		DEBUG(L"[MP] Native input hook uninstalled");
	}
}

bool MP_GetNativeCred(std::string &outAcc, std::string &outPw) {
	outAcc = g_natAccount;
	outPw = g_natPassword;
	return !g_natAccount.empty();
}

static void NatClearCred() {
	g_natAccount.clear();
	g_natPassword.clear();
	g_natField = NAT_FIELD_ACCOUNT;
}

// ---- [MP] 同步等待服务端 ctrl 结果(从 g_ctrlQueue 轮询) ----
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
		if (GetTickCount() - start > (DWORD)timeoutMs) return false;
		Sleep(30);
	}
}

// ---- 基础设施 ----

bool MP_IsConnected() {
	return g_connected != 0;
}

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

void MP_SendCtrl(BYTE cmd) {
	MP_SendRaw(MP_TYPE_CTRL, &cmd, 1);
}

// [MP] 发送带密码的登录请求(账号\0密码)
void MP_SendLogin(const std::string &acc, const std::string &pw) {
	std::vector<BYTE> buf;
	buf.push_back(MP_CTRL_LOGIN);
	for (char c : acc) buf.push_back((BYTE)c);
	buf.push_back(0);
	for (char c : pw) buf.push_back((BYTE)c);
	if (buf.size() > 1) MP_SendRaw(MP_TYPE_CTRL, &buf[0], (DWORD)buf.size());
}

bool MP_IsAuthed() {
	return g_authed == 1;
}

void MP_SetAuthed(bool v) {
	InterlockedExchange(&g_authed, v ? 1 : -1);
}

bool MP_PopPacket(std::vector<BYTE> &out) {
	if (!g_csReady) return false;
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
	if (!g_csReady || !n) return;
	std::vector<BYTE> pkt(p, p + n);
	EnterCriticalSection(&g_cs);
	g_inQueue.push_back(pkt);
	LeaveCriticalSection(&g_cs);
}

// ---- 收包线程: 连接服务端 + 收包转发 + 安装输入钩子 ----
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

	// [MP] 安装消息钩子: 捕获用户在原生登录界面的键盘输入
	NatInstallHook();

	// 收包循环: 游戏包入队 g_inQueue, ctrl 包入队 g_ctrlQueue
	// 认证由 LoginButton_Hook 驱动(用户点登录时才发), 这里只做转发.
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
	NatUninstallHook();
	DEBUG(L"MP disconnected");
	return 0;
}

bool MP_Start(HINSTANCE hinstDLL) {
	if (!g_csReady) {
		InitializeCriticalSection(&g_cs);
		g_csReady = true;
	}

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
	// 不再从 ini 读 Account/Password — 改为原生界面输入(消息钩子捕获)

	HANDLE hThread = CreateThread(NULL, 0, MP_Thread, NULL, 0, NULL);
	if (hThread) { CloseHandle(hThread); return true; }
	return false;
}

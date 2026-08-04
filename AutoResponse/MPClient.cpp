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

// ---- [MP] 自定义登录浮层（深色风格）----
// 冲锋岛原生登录框使用 DirectInput/Raw Input, 不走标准 Win32 消息循环。
// 已验证无效的方案: EnumChildWindows(非标准控件)、WH_GETMESSAGE(消息不经过队列)。
// 正解: 在游戏窗口上创建一个自定义绘制的深色浮层窗口, 让用户输入账号密码。
// 视觉上看起来像"游戏内的登录界面", 实际是独立 Win32 窗口。

static HWND   g_hOverlay = NULL;
static HWND   g_hEdtAcc = NULL;
static HWND   g_hEdtPw  = NULL;
static std::string g_ovAccount;
static std::string g_ovPassword;
static volatile LONG g_ovResult = 0;     // 0=显示中 1=用户确认 -1=取消/关闭

// 浮层配色(深色主题, 与启动器 GUI 风格统一)
#define OVR_BG       RGB(30,31,43)       // 主背景
#define OVR_CARD     RGB(40,42,58)       // 卡片背景
#define OVR_INPUT    RGB(26,27,38)       // 输入框背景
#define OVR_TEXT     RGB(232,232,240)    // 主文字
#define OVR_SUB      RGB(154,154,176)    // 副文字
#define OVR_ACCENT   RGB(91,140,255)     // 强调色
#define OVR_OK_BG    RGB(62,207,142)     // 确认按钮背景
#define OVR_OK_TEXT  RGB(15,36,25)       // 确认按钮文字
#define OVR_BORDER   RGB(60,62,80)       // 边框

static HBRUSH g_hBrushBg    = NULL;
static HBRUSH g_hBrushInput = NULL;
static HBRUSH g_hBrushCard  = NULL;
static HFONT  g_hFontTitle  = NULL;
static HFONT  g_hFontNormal = NULL;

static void InitOverlayResources() {
	if (!g_hBrushBg)    g_hBrushBg    = CreateSolidBrush(OVR_BG);
	if (!g_hBrushInput) g_hBrushInput = CreateSolidBrush(OVR_INPUT);
	if (!g_hBrushCard)  g_hBrushCard  = CreateSolidBrush(OVR_CARD);
	if (!g_hFontTitle)  g_hFontTitle  = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
	if (!g_hFontNormal) g_hFontNormal = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
}

static void FreeOverlayResources() {
	if (g_hBrushBg)    { DeleteObject(g_hBrushBg);    g_hBrushBg    = NULL; }
	if (g_hBrushInput) { DeleteObject(g_hBrushInput); g_hBrushInput = NULL; }
	if (g_hBrushCard)  { DeleteObject(g_hBrushCard);  g_hBrushCard  = NULL; }
	if (g_hFontTitle)  { DeleteObject(g_hFontTitle);  g_hFontTitle  = NULL; }
	if (g_hFontNormal) { DeleteObject(g_hFontNormal); g_hFontNormal = NULL; }
}

// 绘制浮层背景(标题栏 + 提示文字)
static void PaintOverlayBg(HWND hwnd) {
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);
	RECT rc;
	GetClientRect(hwnd, &rc);

	// 主背景
	FillRect(hdc, &rc, g_hBrushBg);

	// 标题
	SetBkMode(hdc, TRANSPARENT);
	HFONT oldFont = (HFONT)SelectObject(hdc, g_hFontTitle);
	SetTextColor(hdc, OVR_TEXT);
	TextOutW(hdc, 24, 20, L"Tenvi MP", 9);

	// 副标题
	SelectObject(hdc, g_hFontNormal);
	SetTextColor(hdc, OVR_SUB);
	const char *hint = "New accounts auto-register. Wrong password rejected.";
	TextOutA(hdc, 24, 52, hint, (int)strlen(hint));

	// 底部提示
	SetTextColor(hdc, OVR_SUB);
	const char *footer = "Press Enter or click Login to continue.";
	int footerLen = (int)strlen(footer);
	SIZE sz = {};
	GetTextExtentPoint32A(hdc, footer, footerLen, &sz);
	TextOutA(hdc, 24, rc.bottom - 36, footer, footerLen);

	SelectObject(hdc, oldFont);
	EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_PAINT:
		PaintOverlayBg(hwnd);
		return 0;

	case WM_COMMAND:
		if (LOWORD(wParam) == 100 || LOWORD(wParam) == IDOK) {
			// [Login] button pressed or Enter in an edit
			char acc[256] = {}, pw[256] = {};
			GetWindowTextA(g_hEdtAcc, acc, sizeof(acc));
			GetWindowTextA(g_hEdtPw, pw, sizeof(pw));
			g_ovAccount = acc;
			g_ovPassword = pw;
			InterlockedExchange(&g_ovResult, 1);
			DestroyWindow(hwnd);
			g_hOverlay = NULL;
			return 0;
		}
		break;

	case WM_CTLCOLORSTATIC: {
		HDC hdcS = (HDC)wParam;
		SetBkMode(hdcS, TRANSPARENT);
		SetTextColor(hdcS, OVR_TEXT);
		return (LRESULT)g_hBrushBg;
	}
	case WM_CTLCOLOREDIT: {
		HDC hdcE = (HDC)wParam;
		SetBkColor(hdcE, OVR_INPUT);
		SetTextColor(hdcE, OVR_TEXT);
		return (LRESULT)g_hBrushInput;
	}

	case WM_DESTROY:
		if (g_ovResult == 0) InterlockedExchange(&g_ovResult, -1); // 用户关了窗但没点确认
		return 0;

	case WM_CLOSE:
		InterlockedExchange(&g_ovResult, -1);
		DestroyWindow(hwnd);
		g_hOverlay = NULL;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 显示模态登录浮层。返回 true 表示用户点了确认(false=取消/关闭)。
// 阻塞调用方线程直到用户操作完毕。
static bool ShowLoginOverlay() {
	InitOverlayResources();

	// 注册窗口类(仅一次)
	static bool classRegistered = false;
	if (!classRegistered) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc   = OverlayWndProc;
		wc.hInstance     = GetModuleHandleW(NULL);
		wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
		wc.hbrBackground = g_hBrushBg;
		wc.lpszClassName = L"TenviMPOverlay";
		RegisterClassW(&wc);
		classRegistered = true;
	}

	// 计算位置: 屏幕居中(或游戏窗口居中)
	int x = 0, y = 0;
	HWND hOwner = GetForegroundWindow();
	if (!hOwner) hOwner = GetDesktopWindow();
	RECT or_;
	GetWindowRect(hOwner, &or_);
	x = or_.left + ((or_.right - or_.left) - 360) / 2;
	y = or_.top + ((or_.bottom - or_.top) - 260) / 2;
	if (x < 0) x = 0;
	if (y < 0) y = 0;

	// 创建浮层窗口(无边框、无标题栏、不在任务栏显示 —— 看起来像游戏内面板)
	g_hOverlay = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		L"TenviMPOverlay", L"",
		WS_POPUP | WS_VISIBLE,
		x, y, 360, 260,
		hOwner, NULL, GetModuleHandleW(NULL), NULL);

	if (!g_hOverlay) return false;

	// 创建子控件
	// --- Account label ---
	CreateWindowExW(0, L"STATIC", L"Account",
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		24, 90, 320, 22, g_hOverlay, (HMENU)200, NULL, NULL);

	// --- Account edit ---
	g_hEdtAcc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
		24, 116, 312, 28, g_hOverlay, (HMENU)101, NULL, NULL);
	SendMessage(g_hEdtAcc, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

	// --- Password label ---
	CreateWindowExW(0, L"STATIC", L"Password",
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		24, 152, 320, 22, g_hOverlay, (HMENU)201, NULL, NULL);

	// --- Password edit (masked) ---
	g_hEdtPw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
		24, 178, 312, 28, g_hOverlay, (HMENU)102, NULL, NULL);
	SendMessage(g_hEdtPw, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

	// --- Login button ---
	HWND hBtn = CreateWindowExW(0, L"BUTTON", L"Login >",
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
		24, 216, 312, 32, g_hOverlay, (HMENU)100, NULL, NULL);
	SendMessage(hBtn, WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);

	// 聚焦到账号输入框
	SetFocus(g_hEdtAcc);

	// [关键] 禁用游戏窗口, 防止用户点底部的原生登录按钮
	EnableWindow(hOwner, FALSE);

	// 模态消息循环
	InterlockedExchange(&g_ovResult, 0);
	ShowWindow(g_hOverlay, SW_SHOW);
	SetForegroundWindow(g_hOverlay);

	MSG msg;
	while (g_ovResult == 0 && GetMessageW(&msg, NULL, 0, 0)) {
		if (!IsDialogMessageW(g_hOverlay, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	// [关键] 恢复游戏窗口(无论成功/取消/关闭都要恢复)
	EnableWindow(hOwner, TRUE);
	SetForegroundWindow(hOwner); // 把焦点还给游戏

	bool ok = (g_ovResult == 1);
	FreeOverlayResources();
	return ok;
}

bool MP_GetNativeCred(std::string &outAcc, std::string &outPw) {
	outAcc = g_ovAccount;
	outPw  = g_ovPassword;
	return !g_ovAccount.empty();
}

void NatClearCred() {
	g_ovAccount.clear();
	g_ovPassword.clear();
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

// ---- 收包线程: 连接服务端 + 收包转发 + 弹出登录浮层 ----
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

	// [MP] 弹出深色登录浮层: 用户在此输入账号密码。
	// 浮层是模态的, 阻塞本线程直到用户确认或取消。
	DEBUG(L"MP showing login overlay...");
	bool ok = ShowLoginOverlay();
	if (!ok) {
		DEBUG(L"MP login overlay cancelled by user");
		// 用户取消了, 保持连接不断(可以重新弹? 目前直接放弃)
	} else {
		DEBUG(L"MP overlay got: acc=%hs pw=%d chars", g_ovAccount.c_str(), (int)g_ovPassword.length());
		// 发送登录请求(自动注册+校验合一)
		MP_SendLogin(g_ovAccount, g_ovPassword);

		// 同步等待结果
		BYTE res = 0;
		if (MP_WaitCtrlResult(MP_CTRL_LOGIN_RESULT, 8000, res)) {
			if (res == 1) {
				DEBUG(L"MP login SUCCESS");
				MP_SetAuthed(true);
				NatClearCred(); // 清内存中的明文密码
				MP_SendCtrl(MP_CTRL_WORLDLIST);
			} else {
				DEBUG(L"MP login FAILED (wrong password?)");
				// 失败: 可以在这里再弹一次浮层, 目前先标记失败
				MessageBoxA(NULL,
					"Login failed: wrong password or server error.\n"
					"Please restart the game and try again.",
					"Tenvi MP", MB_OK | MB_ICONWARNING);
				MP_SetAuthed(false);
			}
		} else {
			DEBUG(L"MP login TIMEOUT");
			MessageBoxA(NULL,
				"Login timeout: server not responding.\n"
				"Check that StandaloneServer.exe is running.",
				"Tenvi MP", MB_OK | MB_ICONERROR);
			MP_SetAuthed(false);
		}
	}

	// 收包循环: 游戏包入队 g_inQueue, ctrl 包入队 g_ctrlQueue
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
	// 不从 ini 读 Account/Password — 改为连接后弹出登录浮层

	HANDLE hThread = CreateThread(NULL, 0, MP_Thread, NULL, 0, NULL);
	if (hThread) { CloseHandle(hThread); return true; }
	return false;
}

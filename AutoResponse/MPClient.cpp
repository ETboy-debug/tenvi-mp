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

// ---- [MP] 原生登录界面键盘捕获系统(v27) ----
// 原理: 冲锋岛登录框的输入绕开了 Windows 控件(自定义渲染), 无法直接读控件值。
//       但 WH_KEYBOARD_LL 低级键盘钩子运行在 OS 最底层, 无论游戏用 DirectInput
//       还是 RawInput 都能捕获按键 —— 前提是钩子必须挂在【独立消息泵线程】上。
//       (之前 v25 失败就是这个原因: 钩子挂在了游戏主线程, 主线程不泵消息 ->
//        回调永不触发。这是教科书级的正确写法, 与游戏不可hook无关。)
//       本版用专用线程 SetWindowsHookEx + GetMessage 循环, 回调在该线程触发。
//
// 流程:
//   1. MP_Start 启动 KBCaptureThread(安装钩子 + 消息循环)
//   2. 连上服务端后 MP_EnableCapture() -> 静默记录按键(仅当游戏窗口前台)
//   3. 用户在原生登录界面正常打字(游戏照常显示字符, 我们后台镜像)
//      - 账号字段: 直接记录; 输完按 TAB 切到密码字段
//      - VK_BACK 删除上一字符
//   4. 点"登录" -> LoginButton_Hook 取 MP_GetNativeCred -> 发服务端认证
//   5. 认证成功后 MP_DisableCapture() 停止记录

static HINSTANCE g_hInst = NULL;
static HHOOK g_hKBHook = NULL;
static volatile LONG g_captureEnabled = 0;   // 1=登录界面期间记录按键
static int g_field = 0;                        // 0=账号 1=密码
static std::string g_acc;
static std::string g_pw;
static CRITICAL_SECTION g_capCs;
static bool g_capCsReady = false;

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION && wParam == WM_KEYDOWN) {
		if (InterlockedCompareExchange(&g_captureEnabled, 0, 0)) {
			// 仅当游戏窗口处于前台(确保按键是打给游戏的)
			DWORD fgPid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
			if (fgPid == GetCurrentProcessId()) {
				KBDLLHOOKSTRUCT *kh = (KBDLLHOOKSTRUCT *)lParam;
				DWORD vk = kh->vkCode;

				if (vk == VK_TAB) {
					g_field = 1; // 切到密码字段
					return CallNextHookEx(g_hKBHook, nCode, wParam, lParam);
				}
				if (vk == VK_BACK) {
					if (g_field == 0 && !g_acc.empty()) g_acc.pop_back();
					else if (g_field == 1 && !g_pw.empty()) g_pw.pop_back();
					return CallNextHookEx(g_hKBHook, nCode, wParam, lParam);
				}
				if (vk == VK_RETURN || vk == VK_ESCAPE) {
					return CallNextHookEx(g_hKBHook, nCode, wParam, lParam);
				}
				// 用全局异步按键状态翻译字符(跨线程可靠, 含 Shift/Caps 状态)
				BYTE kbState[256] = {0};
				for (int i = 0; i < 256; i++)
					kbState[i] = (GetAsyncKeyState(i) & 0x8000) ? 0xFF : 0;
				WORD wch = 0;
				if (ToAscii(vk, kh->scanCode, kbState, &wch, 0) == 1) {
					char c = (char)(wch & 0xFF);
					if (c >= 0x20 && c < 0x7F) { // 可打印 ASCII
						if (g_field == 0 && g_acc.size() < 64) g_acc += c;
						else if (g_field == 1 && g_pw.size() < 64) g_pw += c;
					}
				}
			}
		}
	}
	return CallNextHookEx(g_hKBHook, nCode, wParam, lParam);
}

static DWORD WINAPI KBCaptureThread(LPVOID) {
	g_hKBHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hInst, 0);
	if (!g_hKBHook) {
		DEBUG(L"[MP] KB hook install failed, err=%lu", GetLastError());
		return 0;
	}
	DEBUG(L"[MP] KB hook installed on dedicated thread");
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	if (g_hKBHook) { UnhookWindowsHookEx(g_hKBHook); g_hKBHook = NULL; }
	return 0;
}

// 启动键盘捕获线程(在 MP_Start 调用)
static void MP_StartKBCapture() {
	if (!g_capCsReady) { InitializeCriticalSection(&g_capCs); g_capCsReady = true; }
	HANDLE h = CreateThread(NULL, 0, KBCaptureThread, NULL, 0, NULL);
	if (h) CloseHandle(h);
}

// 连上服务端后调用: 清空并开启记录
void MP_EnableCapture() {
	EnterCriticalSection(&g_capCs);
	g_acc.clear(); g_pw.clear(); g_field = 0;
	LeaveCriticalSection(&g_capCs);
	InterlockedExchange(&g_captureEnabled, 1);
	DEBUG(L"[MP] capture ENABLED");
}

// 认证完成后调用: 停止记录
void MP_DisableCapture() {
	InterlockedExchange(&g_captureEnabled, 0);
	DEBUG(L"[MP] capture DISABLED");
}

// LoginButton_Hook 调用: 取捕获的账号密码
bool MP_GetNativeCred(std::string &outAcc, std::string &outPw) {
	EnterCriticalSection(&g_capCs);
	outAcc = g_acc; outPw = g_pw;
	LeaveCriticalSection(&g_capCs);
	return !outAcc.empty();
}

// 认证成功后清空(安全)
void MP_ClearCred() {
	EnterCriticalSection(&g_capCs);
	g_acc.clear(); g_pw.clear(); g_field = 0;
	LeaveCriticalSection(&g_capCs);
}

void MP_ResetLoginState() {
	MP_DisableCapture();
	MP_ClearCred();
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
		if (timeoutMs == 0) return false;        // 非阻塞模式, 立即返回
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

// ---- 收包线程: 连接服务端 + 收包转发(纯网络, 不涉及UI/钩子) ----
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
	MP_EnableCapture(); // [v27] 登录界面期间开始静默记录键盘

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
	if (!g_capCsReady) {
		InitializeCriticalSection(&g_capCs);
		g_capCsReady = true;
	}
	g_hInst = hinstDLL;

	// [v27] 启动键盘捕获线程(独立消息泵, 低级钩子回调在其上触发)
	MP_StartKBCapture();

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
	// 不从 ini 读 Account/Password — 改为从登录包中拦截提取

	HANDLE hThread = CreateThread(NULL, 0, MP_Thread, NULL, 0, NULL);
	if (hThread) { CloseHandle(hThread); return true; }
	return false;
}

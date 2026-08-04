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

// ---- [MP] 低级键盘钩子(WH_KEYBOARD_LL) 捕获原生登录界面输入 ----
// 冲锋岛登录框是自定义 DirectX 渲染, 输入走 DirectInput/Raw Input。
// 已验证无效: EnumChildWindows(非标准控件)、WH_GETMESSAGE(消息不经过队列)。
// 正解: WH_KEYBOARD_LL 在操作系统内核层拦截按键, 比 DirectInput 更底层,
//       无论游戏用什么方式读取键盘都能捕获。用户在原生界面正常打字即可。

enum NatField { NAT_ACC = 0, NAT_PW = 1 };

static NatField g_natField = NAT_ACC;          // 当前输入字段(账号/密码)
static std::string g_natAccount;               // 捕获的账号
static std::string g_natPassword;              // 捕获的密码
static HHOOK g_hKbHook = NULL;                 // 低级键盘钩子句柄
static HWND  g_hGameWnd = NULL;                // 游戏窗口句柄
static volatile LONG g_hookActive = 0;         // 钩子是否激活(连接后激活, 认证后停用)
static CRITICAL_SECTION g_credCs;              // 保护凭据的 CS
static bool g_credCsReady = false;

// 钩子回调(在系统线程运行, 必须快)
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode >= 0 && wParam == WM_KEYDOWN && InterlockedCompareExchange(&g_hookActive, 0, 0)) {
		KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;

		// 只在游戏窗口聚焦时捕获
		HWND fg = GetForegroundWindow();
		if (fg != g_hGameWnd) return CallNextHookEx(NULL, nCode, wParam, lParam);

		DWORD vk = kb->vkCode;
		EnterCriticalSection(&g_credCs);

		if (vk == VK_TAB) {
			// TAB 切换账号/密码字段
			g_natField = (g_natField == NAT_ACC) ? NAT_PW : NAT_ACC;
		} else if (vk == VK_BACK) {
			// 退格删除
			std::string &s = (g_natField == NAT_ACC) ? g_natAccount : g_natPassword;
			if (!s.empty()) s.pop_back();
		} else if (vk == VK_RETURN) {
			// 回车不做特殊处理(让游戏自己处理"登录"按钮点击)
		} else if (vk >= 0x20 && vk <= 0x7E) {
			// 可打印 ASCII 字符
			BYTE ks[256] = {};
			GetKeyboardState(ks);
			WCHAR uch[4] = {};
			int ret = ToUnicode(vk, kb->scanCode, ks, uch, 4, 0);
			if (ret > 0) {
				char mb[8] = {};
				int mblen = WideCharToMultiByte(CP_ACP, 0, uch, ret, mb, sizeof(mb), NULL, NULL);
				if (mblen > 0) {
					std::string &s = (g_natField == NAT_ACC) ? g_natAccount : g_natPassword;
					s.append(mb, mblen);
				}
			}
		}
		// 其他键(Shift/Ctrl/Alt/箭头等)忽略, 不阻止传递给游戏

		LeaveCriticalSection(&g_credCs);
	}

	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// 安装低级键盘钩子
static void InstallKBHook() {
	if (!g_credCsReady) {
		InitializeCriticalSection(&g_credCs);
		g_credCsReady = true;
	}
	g_hGameWnd = GetForegroundWindow(); // 游戏窗口(连接时应该已经显示)
	g_natAccount.clear();
	g_natPassword.clear();
	g_natField = NAT_ACC;
	InterlockedExchange(&g_hookActive, 1);
	g_hKbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
		GetModuleHandleW(NULL), 0); // 全局钩子(threadId=0)
	DEBUG(L"[MP] KB hook installed, hwnd=%p", g_hGameWnd);
}

// 卸载低级键盘钩子并清空凭据
void UninstallKBHook() {
	InterlockedExchange(&g_hookActive, 0);
	if (g_hKbHook) {
		UnhookWindowsHookEx(g_hKbHook);
		g_hKbHook = NULL;
	}
	DEBUG(L"[MP] KB hook uninstalled");
}

// 获取捕获到的凭据(供 AutoResponse.cpp 的 LoginButton_Hook 调用)
bool MP_GetNativeCred(std::string &outAcc, std::string &outPw) {
	EnterCriticalSection(&g_credCs);
	outAcc = g_natAccount;
	outPw  = g_natPassword;
	bool ok = !g_natAccount.empty();
	LeaveCriticalSection(&g_credCs);
	return ok;
}

// 清空凭据(认证成功后调用)
void NatClearCred() {
	EnterCriticalSection(&g_credCs);
	g_natAccount.clear();
	g_natPassword.clear();
	g_natField = NAT_ACC;
	LeaveCriticalSection(&g_credCs);
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

// ---- 收包线程: 连接服务端 + 安装键盘钩子 + 收包转发 + 等待认证 ----
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

	// [MP] 安装低级键盘钩子: 用户在游戏原生登录界面打字, 我们静默捕获。
	// 不弹任何窗口, 不阻挡任何游戏操作。
	InstallKBHook();

	// 注意: 认证不在这里做! 认证推迟到用户点"登录"按钮时,
	// 由 AutoResponse.cpp 的 LoginButton_Hook 触发(MP_GetNativeCred 取输入 -> 发登录请求)。
	// 这里只负责: 连接 + 装钩子 + 收包循环。

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
	UninstallKBHook();
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
	// 不从 ini 读 Account/Password — 改为键盘钩子从原生界面捕获

	HANDLE hThread = CreateThread(NULL, 0, MP_Thread, NULL, 0, NULL);
	if (hThread) { CloseHandle(hThread); return true; }
	return false;
}

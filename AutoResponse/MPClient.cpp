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
static std::vector<std::vector<BYTE>> g_ctrlQueue; // [MP] 服务端 ctrl 包队列(MP_Thread 填充, MP_WaitCtrlResult 轮询)
static void MP_PushPacket(const BYTE *p, DWORD n); // [MP] forward decl, defined later in this file
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

void MP_SetAuthed(bool v) {
	InterlockedExchange(&g_authed, v ? 1 : 0);
}

// ---- [MP] 从冲锋岛原生登录界面读取账号密码 ----
static struct {
	char acc[128];
	char pw[128];
	int nEdit;
} s_nativeCred;

static BOOL CALLBACK EnumNativeEdits(HWND hwnd, LPARAM) {
	char cls[64];
	GetClassNameA(hwnd, cls, sizeof(cls));
	if (_stricmp(cls, "EDIT") == 0) {
		LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
		bool isPwd = (style & ES_PASSWORD) != 0;
		if (isPwd)
			GetWindowTextA(hwnd, s_nativeCred.pw, sizeof(s_nativeCred.pw));
		else
			GetWindowTextA(hwnd, s_nativeCred.acc, sizeof(s_nativeCred.acc));
		s_nativeCred.nEdit++;
	}
	return TRUE;
}

/// 从游戏原生登录界面的 EDIT 控件读取账号和密码。
/// 返回 true 表示至少读到了一个非空字段。
bool MP_ReadNativeCred(std::string &outAcc, std::string &outPw) {
	memset(&s_nativeCred, 0, sizeof(s_nativeCred));
	HWND hMain = GetForegroundWindow();
	if (!hMain) return false;
	EnumChildWindows(hMain, EnumNativeEdits, 0);
	if (s_nativeCred.nEdit < 2) {
		// 向上搜索父窗口（有些游戏的编辑框在子面板里）
		HWND hParent = GetParent(hMain);
		while (hParent && s_nativeCred.nEdit < 2) {
			EnumChildWindows(hParent, EnumNativeEdits, 0);
			hParent = GetParent(hParent);
		}
	}
	outAcc = s_nativeCred.acc;
	outPw = s_nativeCred.pw;
	return s_nativeCred.acc[0] != '\0' || s_nativeCred.pw[0] != '\0';
}

// 同步等待服务端 ctrl 结果; MP_Thread 是唯一收包线程, ctrl 包已入 g_ctrlQueue, 这里轮询该队列
bool MP_WaitCtrlResult(BYTE expectCmd, int timeoutMs, BYTE &outByte) {
	DWORD start = GetTickCount();
	while (true) {
		EnterCriticalSection(&g_cs);
		for (size_t i = 0; i < g_ctrlQueue.size(); ) {
			std::vector<BYTE> &pkt = g_ctrlQueue[i];
			// pkt[0]=type, pkt[1]=cmd, pkt[2]=可选数据
			if (pkt.size() >= 2 && pkt[1] == expectCmd) {
				outByte = (pkt.size() >= 3) ? pkt[2] : 1;
				g_ctrlQueue.erase(g_ctrlQueue.begin() + i);
				LeaveCriticalSection(&g_cs);
				return true;
			}
			++i; // 非期望的 ctrl 包: 保留, 不误删其他等待者
		}
		LeaveCriticalSection(&g_cs);
		if (GetTickCount() - start > (DWORD)timeoutMs) return false;
		Sleep(30);
	}
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
	// [MP] 不再弹 Win32 对话框。认证由原生登录界面驱动:
	//   用户在游戏原生登录界面输入账号密码 -> 点"登录"
	//   -> LoginButton_Hook 读取原生控件文本 -> 发服务端 -> 等结果 -> 成功才放行
	// 这里只做收包转发，不阻塞。

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

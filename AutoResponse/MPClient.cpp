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

// ---- [MP] 登录包拦截认证系统(v24) ----
// 原理: 冲锋岛登录框的输入方式完全绕开 Windows 键盘架构(DirectInput 独占),
//       已验证三种键盘拦截全部失败(EnumChildWindows / WM_GETMESSAGE / WH_KEYBOARD_LL)。
//       正解: 不拦键盘, 而是拦截游戏自己发出的登录包。
//       用户在原生界面正常输入 -> 点"登录" -> 游戏调 EnterSendPacket 发登录包
//       -> 我们在 EnterSendPacket_Hook 里从明文包中提取账号密码 -> 发服务端认证
//
// 流程:
//   1. LoginButton_Hook 设置 g_loginPending=1 + 调用原始按钮处理(触发发包)
//   2. EnterSendPacket_Hook 检测到 g_loginPending + 登录包格式 -> 提取凭据
//      -> 发 MP_CTRL_LOGIN 到服务端 -> 丢弃原始包(不发往真实服务器)
//      -> 设置 g_authInProgress=1
//   3. ProcessPacketCaller_Hook(每帧轮询)检测到 g_authInProgress
//      -> 轮询 g_ctrlQueue 等 MP_CTRL_LOGIN_RESULT
//      -> 成功: MP_SetAuthed(true) + MP_SendCtrl(MP_CTRL_WORLDLIST)
//      -> 失败: 弹错误提示

volatile LONG g_loginPending = 0;     // LoginButton_Hook 已触发, 等待截获登录包
volatile LONG g_authInProgress = 0;    // 登录包已截获, 正在等待服务端认证结果
static std::string g_extractedAcc;            // 从登录包中提取的账号
static std::string g_extractedPw;             // 从登录包中提取的密码
static CRITICAL_SECTION g_authCs;
static bool g_authCsReady = false;

// 从原始包数据中尝试提取两个连续的空终止字符串(账号+密码)
// 大多数老网游登录包格式: [opcode N字节] [account\0] [password\0] [可选额外字段]
// 返回 true 如果成功提取到两个非空字符串
static bool TryExtractLoginCreds(const BYTE *data, DWORD len,
                                  std::string &outAcc, std::string &outPw) {
	if (!data || len < 5) return false; // 至少: 1字节opcode + "a\0b\0"

	// 跳过 opcode(通常1-2字节, 少数情况3字节)
	// 策略: 从第2字节开始扫描, 找到两个连续的非空空终止字符串
	for (DWORD start = 1; start <= 3 && start < len; start++) {
		DWORD i = start;
		// 第一个字符串: 账号
		if (i >= len || data[i] == 0) continue; // 空字符串, 跳过
		DWORD accStart = i;
		while (i < len && data[i] != 0) i++;
		if (i >= len) break; // 没找到结束符
		DWORD accLen = i - accStart;
		if (accLen == 0 || accLen > 50) continue; // 账号太长或为空, 跳过这个 offset
		i++; // 跳过 '\0'

		// 第二个字符串: 密码
		if (i >= len || data[i] == 0) continue;
		DWORD pwStart = i;
		while (i < len && data[i] != 0) i++;
		if (i >= len) break; // 包结束了但没找到密码结束符(可能密码在最后且无终止符?)
		DWORD pwLen = i - pwStart;
		if (pwLen == 0 || pwLen > 50) continue;

		// 验证: 账号和密码都应该是可打印 ASCII
		bool valid = true;
		for (DWORD j = accStart; j < accStart + accLen; j++) {
			if (data[j] < 0x20 || data[j] > 0x7E) { valid = false; break; }
		}
		if (!valid) continue;
		for (DWORD j = pwStart; j < pwStart + pwLen; j++) {
			if (data[j] < 0x20 || data[j] > 0x7E) { valid = false; break; }
		}
		if (!valid) continue;

		// 找到了!
		outAcc.assign((const char *)(data + accStart), accLen);
		outPw.assign((const char *)(data + pwStart), pwLen);
		return true;
	}
	return false;
}

// [供 EnterSendPacket_Hook 调用] 尝试截获登录包并提取凭据
// 返回: 0=不是登录包/无需处理  1=已截获并发起认证  -1=截获但凭据无效
int MP_InterceptLoginPacket(const BYTE *data, DWORD len) {
	if (!InterlockedCompareExchange(&g_loginPending, 0, 0)) return 0; // 没有待处理的登录

	std::string acc, pw;
	if (!TryExtractLoginCreds(data, len, acc, pw)) {
		// 包格式不匹配 — 可能不是登录包, 或者格式与预期不同
		// 记录诊断信息
		DEBUG(L"[MP] Login pending but packet format mismatch, op=%02X len=%lu",
		      len > 0 ? data[0] : 0, (unsigned long)len);
		{ FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
		  if (f) { fprintf(f, "[LOGIN-INTERCEPT] op=%02X len=%lu hex=",
		                   len > 0 ? data[0] : 0, (unsigned long)len);
		    for (DWORD k = 0; k < len && k < 64; k++) fprintf(f, "%02X", data[k]);
		    fprintf(f, "\n"); fflush(f); fclose(f); } }
		return 0; // 不是登录包, 让它正常走
	}

	// 凭据提取成功!
	EnterCriticalSection(&g_authCs);
	g_extractedAcc = acc;
	g_extractedPw = pw;
	LeaveCriticalSection(&g_authCs);

	DEBUG(L"[MP] Intercepted login: acc=%hs pw=%d chars", acc.c_str(), pw.length());
	{ FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
	  if (f) { fprintf(f, "[LOGIN-INTERCEPT OK] acc=%s pw_len=%d\n", acc.c_str(), (int)pw.length());
	    fflush(f); fclose(f); } }

	// 发送到服务端认证
	MP_SendLogin(acc, pw);

	// 标记: 登录包已截获, 等待认证结果(由 ProcessPacketCaller_Hook 轮询)
	InterlockedExchange(&g_loginPending, 0);     // 清除: 只截获第一个匹配包
	InterlockedExchange(&g_authInProgress, 1);   // 标记: 认证进行中

	return 1; // 告诉调用方: 这是登录包, 已处理, 丢弃它
}

// [供 ProcessPacketCaller_Hook 每帧调用] 检查认证结果
// 返回: 0=无需处理/还在等  1=认证成功  -1=认证失败  -2=超时
static DWORD g_authStartTime = 0;

int MP_PollAuthResult() {
	if (!InterlockedCompareExchange(&g_authInProgress, 0, 0)) return 0;

	// 初始化超时计时
	if (g_authStartTime == 0) g_authStartTime = GetTickCount();

	// 检查超时(15秒)
	if (GetTickCount() - g_authStartTime > 15000) {
		InterlockedExchange(&g_authInProgress, 0);
		g_authStartTime = 0;
		return -2; // 超时
	}

	BYTE res = 0;
	if (!MP_WaitCtrlResult(MP_CTRL_LOGIN_RESULT, 0, res)) {
		return 0; // 还没结果, 下一帧再查(MP_WaitCtrlResult timeout=0 立即返回)
	}

	// 收到结果!
	InterlockedExchange(&g_authInProgress, 0);
	g_authStartTime = 0;

	if (res == 1) {
		MP_SetAuthed(true);
		DEBUG(L"[MP] Auth SUCCESS");
		return 1; // 成功
	} else {
		DEBUG(L"[MP] Auth FAILED res=%d", res);
		return -1; // 失败
	}
}

// 获取最近一次认证失败的凭据(用于错误提示)
void MP_GetLastCred(std::string &outAcc) {
	EnterCriticalSection(&g_authCs);
	outAcc = g_extractedAcc;
	LeaveCriticalSection(&g_authCs);
}

void MP_ResetLoginState() {
	InterlockedExchange(&g_loginPending, 0);
	InterlockedExchange(&g_authInProgress, 0);
	g_authStartTime = 0;
	EnterCriticalSection(&g_authCs);
	g_extractedAcc.clear();
	g_extractedPw.clear();
	LeaveCriticalSection(&g_authCs);
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

	// [v24] 不安装任何键盘钩子/浮层/弹窗。
	// 认证由 EnterSendPacket_Hook 拦截登录包驱动, 完全无感。

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
	if (!g_authCsReady) {
		InitializeCriticalSection(&g_authCs);
		g_authCsReady = true;
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
	// 不从 ini 读 Account/Password — 改为从登录包中拦截提取

	HANDLE hThread = CreateThread(NULL, 0, MP_Thread, NULL, 0, NULL);
	if (hThread) { CloseHandle(hThread); return true; }
	return false;
}

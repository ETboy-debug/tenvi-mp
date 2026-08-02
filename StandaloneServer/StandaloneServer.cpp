// StandaloneServer.cpp - 冲锋岛(Tenvi) 独立服务端
//
// 架构说明（明文桥接）:
//   客户端原生网络栈带加解密, 而模拟器的 hook 点在明文层。
//   所以不去兼容客户端原生线格式, 而是由注入 DLL(AutoResponse) 另开一条 socket,
//   两端只跑纯明文 Tenvi 包, 帧格式由我们自己定义。
//
//   帧 = [4 字节小端 payload 长度][payload]
//   payload[0] = type: 0 = 游戏明文包, 1 = 控制命令
//
// 多玩家:
//   每个连接一个线程。会话状态 TA 声明为 thread_local(见 FakeServer.h),
//   所以各玩家的角色数据天然隔离, 不会串号。
//   注意: 本版本玩家之间"看不到彼此"(无状态广播), 那是下一阶段的事。
//
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <vector>
#include <string>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include "ClientPacket.h"
#include "ServerPacket.h"
#include "FakeServer.h"
#include "TenviData.h"
#include "../EmuMainTenvi/ConfigTenvi.h"

#pragma comment(lib, "ws2_32.lib")

using namespace std;

#define MP_TYPE_GAME 0
#define MP_TYPE_CTRL 1

#define MP_CTRL_HELLO     1
#define MP_CTRL_WORLDLIST 2
#define MP_CTRL_CHARLIST  3

#define MP_MAX_PLAYERS 32

// FakeServer.cpp 里定义的版本包（头文件未声明，这里补上）
void VersionPacket();

// ---- 区域配置：原本由注入 DLL 读 ini 决定，独立服务端固定国服 CN v126 ----
static Region g_region = TENVI_CN;
static wstring g_regionStr = L"CN";
static wstring g_xmlPath = L"tv_xml";

Region GetRegion() { return g_region; }
wstring GetRegionStr() { return g_regionStr; }
wstring GetXMLPath() { return g_xmlPath; }

// ---- 全局(进程级) ----
static int g_port = 8787;
static int g_dump = 8;                 // 每个会话前 N 个收发包打印十六进制
static volatile LONG g_online = 0;     // 当前在线人数
static volatile LONG g_sidSeq = 0;     // 会话编号发号器
static std::mutex g_logMutex;          // 多线程日志不交错

// ---- 会话级(线程局部): 每个玩家一份 ----
static thread_local SOCKET t_client = INVALID_SOCKET;
static thread_local int t_sid = 0;
static thread_local int t_sendCount = 0;
static thread_local int t_recvCount = 0;

// 带会话号的日志, 整行加锁输出
static void Log(const char *fmt, ...) {
	char line[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(line, sizeof(line) - 1, fmt, ap);
	line[sizeof(line) - 1] = 0;
	va_end(ap);

	std::lock_guard<std::mutex> lock(g_logMutex);
	if (t_sid > 0) {
		printf("[TenviServer] [#%d] %s\n", t_sid, line);
	}
	else {
		printf("[TenviServer] %s\n", line);
	}
}

static void LogW(const char *prefix, const wstring &s) {
	char b[1024] = { 0 };
	WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, b, sizeof(b) - 1, NULL, NULL);
	Log("%s%s", prefix, b);
}

static void HexDump(const char *tag, const BYTE *p, size_t n) {
	char line[256];
	int off = _snprintf(line, sizeof(line) - 1, "%s (%u bytes):", tag, (unsigned)n);
	size_t show = n > 32 ? 32 : n;
	for (size_t i = 0; i < show && off < (int)sizeof(line) - 8; i++) {
		off += _snprintf(line + off, sizeof(line) - off - 1, " %02X", p[i]);
	}
	if (n > show) {
		_snprintf(line + off, sizeof(line) - off - 1, " ...");
	}
	Log("%s", line);
}

// 替代 AutoResponse 的注入式发送：编码后经本线程的 socket 发回对应客户端
void SendPacket(ServerPacket &sp) {
	if (t_client == INVALID_SOCKET) {
		return;
	}
	vector<BYTE> data = sp.get();
	DWORD len = (DWORD)data.size() + 1; // + type
	vector<BYTE> frame;
	frame.push_back((BYTE)(len & 0xFF));
	frame.push_back((BYTE)((len >> 8) & 0xFF));
	frame.push_back((BYTE)((len >> 16) & 0xFF));
	frame.push_back((BYTE)((len >> 24) & 0xFF));
	frame.push_back(MP_TYPE_GAME);
	frame.insert(frame.end(), data.begin(), data.end());

	if (t_sendCount < g_dump && !data.empty()) {
		HexDump("SEND", &data[0], data.size());
		t_sendCount++;
	}

	int r = send(t_client, (const char *)&frame[0], (int)frame.size(), 0);
	if (r == SOCKET_ERROR) {
		Log("send failed, err=%d", WSAGetLastError());
	}
}

// 兼容 FakeServer.cpp 中调用的另外两个发送符号
void SendPacket2(ServerPacket &sp) { SendPacket(sp); }
void DelaySendPacket(ServerPacket &sp) { SendPacket(sp); }

static void HandleCtrl(BYTE cmd) {
	switch (cmd) {
	case MP_CTRL_HELLO:
		Log("client says HELLO -> sending version packet");
		VersionPacket();
		break;
	case MP_CTRL_WORLDLIST:
		Log("ctrl: world list");
		WorldListPacket();
		break;
	case MP_CTRL_CHARLIST:
		Log("ctrl: character list");
		CharacterSelectPacket();
		CharacterListPacket_Test();
		break;
	default:
		Log("unknown ctrl cmd = %u", (unsigned)cmd);
		break;
	}
}

// 处理一条完整 payload
static void HandlePayload(const BYTE *p, DWORD n) {
	if (n < 1) {
		return;
	}
	BYTE type = p[0];
	if (type == MP_TYPE_CTRL) {
		if (n >= 2) {
			HandleCtrl(p[1]);
		}
		return;
	}
	if (type != MP_TYPE_GAME || n < 2) {
		return;
	}
	if (t_recvCount < g_dump) {
		HexDump("RECV", p + 1, n - 1);
		t_recvCount++;
	}
	ClientPacket cp((BYTE *)(p + 1), n - 1);
	FakeServer(cp); // 分发处理，内部通过 SendPacket 回包
}

static void ServeClient() {
	vector<BYTE> buf;
	char tmp[16384];
	while (true) {
		int n = recv(t_client, tmp, sizeof(tmp), 0);
		if (n <= 0) {
			break;
		}
		buf.insert(buf.end(), tmp, tmp + n);
		while (buf.size() >= 4) {
			DWORD len = (DWORD)buf[0] | ((DWORD)buf[1] << 8) | ((DWORD)buf[2] << 16) | ((DWORD)buf[3] << 24);
			if (len == 0 || len > 65536) {
				Log("!! bad frame length = %u, dropping buffer", len);
				HexDump("BADBUF", &buf[0], buf.size() > 32 ? 32 : buf.size());
				buf.clear();
				break;
			}
			if (buf.size() < 4 + len) {
				break;
			}
			HandlePayload(&buf[4], len);
			buf.erase(buf.begin(), buf.begin() + 4 + len);
		}
	}
}

// 每个玩家一个线程。TA 是 thread_local, 所以会话状态天然隔离。
static DWORD WINAPI ClientThread(LPVOID param) {
	t_client = (SOCKET)(UINT_PTR)param;
	t_sid = (int)InterlockedIncrement(&g_sidSeq);
	t_sendCount = 0;
	t_recvCount = 0;

	LONG online = InterlockedIncrement(&g_online);
	Log("player joined (online=%d)", online);

	ServeClient();

	closesocket(t_client);
	t_client = INVALID_SOCKET;
	online = InterlockedDecrement(&g_online);
	Log("player left (online=%d)", online);
	return 0;
}

int main(int argc, char **argv) {
	// 日志直出，别让 stdout 缓冲把日志卡在管道里（启动器要实时读）
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	// 参数：第一个非选项参数 = xml 数据目录；-p 端口；-d 十六进制转储条数
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
			g_port = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			g_dump = atoi(argv[++i]);
		}
		else if (argv[i][0] != '-') {
			wchar_t wbuf[512] = { 0 };
			MultiByteToWideChar(CP_ACP, 0, argv[i], -1, wbuf, 511);
			g_xmlPath = wbuf;
		}
	}

	Log("=== Tenvi standalone server (multi-player) ===");
	LogW("xml path = ", g_xmlPath);
	LogW("region   = ", g_regionStr);
	Log("port     = %d", g_port);
	Log("max players = %d", MP_MAX_PLAYERS);

	// 加载地图/NPC 数据（FakeServer 换图、刷怪要用）
	tenvi_data.set_xml_path(g_xmlPath);
	Log("xml data loaded.");

	// 初始化国服 v126 的 opcode 编解码表
	SetClientPacketHeader_CN_v126();
	SetServerPacketHeader_CN_v126();

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		Log("WSAStartup failed");
		return 1;
	}

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
	if (listenSock == INVALID_SOCKET) {
		Log("socket failed");
		return 1;
	}

	BOOL reuse = TRUE;
	setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡, 外网玩家才连得进来
	addr.sin_port = htons((u_short)g_port);
	if (bind(listenSock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		Log("bind failed, err=%d (port %d already in use?)", WSAGetLastError(), g_port);
		return 1;
	}
	listen(listenSock, SOMAXCONN);
	Log("listening on 0.0.0.0:%d, waiting for players...", g_port);

	while (true) {
		sockaddr_in ca = {};
		int calen = sizeof(ca);
		SOCKET s = accept(listenSock, (sockaddr *)&ca, &calen);
		if (s == INVALID_SOCKET) {
			Log("accept failed, err=%d", WSAGetLastError());
			break;
		}

		if (g_online >= MP_MAX_PLAYERS) {
			Log("server full (%d), rejecting %s", MP_MAX_PLAYERS, inet_ntoa(ca.sin_addr));
			closesocket(s);
			continue;
		}

		int flag = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
		Log("connection from %s", inet_ntoa(ca.sin_addr));

		HANDLE h = CreateThread(NULL, 0, ClientThread, (LPVOID)(UINT_PTR)s, 0, NULL);
		if (h) {
			CloseHandle(h);
		}
		else {
			Log("CreateThread failed, dropping connection");
			closesocket(s);
		}
	}

	closesocket(listenSock);
	WSACleanup();
	Log("stopped.");
	return 0;
}

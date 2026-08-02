// StandaloneServer.cpp - 冲锋岛(Tenvi) 联机改造里程碑1：独立服务端
//
// 架构说明（明文桥接）:
//   客户端原生网络栈带加解密, 而模拟器的 hook 点在明文层。
//   所以不去兼容客户端原生线格式, 而是由注入 DLL(AutoResponse) 另开一条 socket,
//   两端只跑纯明文 Tenvi 包, 帧格式由我们自己定义。
//
//   帧 = [4 字节小端 payload 长度][payload]
//   payload[0] = type: 0 = 游戏明文包, 1 = 控制命令
//
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>
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

// FakeServer.cpp 里定义的版本包（头文件未声明，这里补上）
void VersionPacket();

// ---- 区域配置：原本由注入 DLL 读 ini 决定，独立服务端里程碑1 固定国服 CN v126 ----
static Region g_region = TENVI_CN;
static wstring g_regionStr = L"CN";
static wstring g_xmlPath = L"tv_xml";

Region GetRegion() { return g_region; }
wstring GetRegionStr() { return g_regionStr; }
wstring GetXMLPath() { return g_xmlPath; }

// 单客户端连接（里程碑1仅验证单客户端能连独立进程，多人同步是里程碑2）
static SOCKET g_client = INVALID_SOCKET;
static int g_port = 8787;
static int g_dump = 8;      // 前 N 个收发包打印十六进制, 便于诊断
static int g_sendCount = 0;
static int g_recvCount = 0;

static void LogW(const char *prefix, const wstring &s) {
	char b[1024] = { 0 };
	WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, b, sizeof(b) - 1, NULL, NULL);
	printf("%s%s\n", prefix, b);
}

static void HexDump(const char *tag, const BYTE *p, size_t n) {
	size_t show = n > 32 ? 32 : n;
	printf("[TenviServer] %s (%u bytes):", tag, (unsigned)n);
	for (size_t i = 0; i < show; i++) {
		printf(" %02X", p[i]);
	}
	if (n > show) {
		printf(" ...");
	}
	printf("\n");
}

// 替代 AutoResponse 的注入式发送：编码后经 socket 发回客户端
void SendPacket(ServerPacket &sp) {
	if (g_client == INVALID_SOCKET) {
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

	if (g_sendCount < g_dump && !data.empty()) {
		HexDump("SEND", &data[0], data.size());
		g_sendCount++;
	}

	int r = send(g_client, (const char *)&frame[0], (int)frame.size(), 0);
	if (r == SOCKET_ERROR) {
		printf("[TenviServer] send failed, err=%d\n", WSAGetLastError());
	}
}

// 兼容 FakeServer.cpp 中调用的另外两个发送符号
void SendPacket2(ServerPacket &sp) { SendPacket(sp); }
void DelaySendPacket(ServerPacket &sp) { SendPacket(sp); }

static void HandleCtrl(BYTE cmd) {
	switch (cmd) {
	case MP_CTRL_HELLO:
		printf("[TenviServer] client says HELLO -> sending version packet\n");
		VersionPacket();
		break;
	case MP_CTRL_WORLDLIST:
		printf("[TenviServer] ctrl: world list\n");
		WorldListPacket();
		break;
	case MP_CTRL_CHARLIST:
		printf("[TenviServer] ctrl: character list\n");
		CharacterSelectPacket();
		CharacterListPacket_Test();
		break;
	default:
		printf("[TenviServer] unknown ctrl cmd = %u\n", (unsigned)cmd);
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
	if (g_recvCount < g_dump) {
		HexDump("RECV", p + 1, n - 1);
		g_recvCount++;
	}
	ClientPacket cp((BYTE *)(p + 1), n - 1);
	FakeServer(cp); // 分发处理，内部通过 SendPacket 回包
}

static void ServeClient() {
	vector<BYTE> buf;
	char tmp[16384];
	while (true) {
		int n = recv(g_client, tmp, sizeof(tmp), 0);
		if (n <= 0) {
			printf("[TenviServer] client disconnected (n=%d)\n", n);
			break;
		}
		buf.insert(buf.end(), tmp, tmp + n);
		while (buf.size() >= 4) {
			DWORD len = (DWORD)buf[0] | ((DWORD)buf[1] << 8) | ((DWORD)buf[2] << 16) | ((DWORD)buf[3] << 24);
			if (len == 0 || len > 65536) {
				printf("[TenviServer] !! bad frame length = %u, dropping buffer\n", len);
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

	printf("[TenviServer] === Tenvi standalone server (milestone 1, plaintext bridge) ===\n");
	LogW("[TenviServer] xml path = ", g_xmlPath);
	LogW("[TenviServer] region   = ", g_regionStr);
	printf("[TenviServer] port     = %d\n", g_port);

	// 加载地图/NPC 数据（FakeServer 换图、刷怪要用）
	tenvi_data.set_xml_path(g_xmlPath);
	printf("[TenviServer] xml data loaded.\n");

	// 初始化国服 v126 的 opcode 编解码表
	SetClientPacketHeader_CN_v126();
	SetServerPacketHeader_CN_v126();

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		printf("[TenviServer] WSAStartup failed\n");
		return 1;
	}

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
	if (listenSock == INVALID_SOCKET) {
		printf("[TenviServer] socket failed\n");
		return 1;
	}

	BOOL reuse = TRUE;
	setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("0.0.0.0"); // 跨机联机也用这个
	addr.sin_port = htons((u_short)g_port);
	if (bind(listenSock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		printf("[TenviServer] bind failed, err=%d (port %d already in use?)\n", WSAGetLastError(), g_port);
		return 1;
	}
	listen(listenSock, 4);
	printf("[TenviServer] listening on 0.0.0.0:%d, waiting for client...\n", g_port);

	// 循环接客：客户端退出后不用重启服务端，直接等下一次连接
	while (true) {
		sockaddr_in ca = {};
		int calen = sizeof(ca);
		g_client = accept(listenSock, (sockaddr *)&ca, &calen);
		if (g_client == INVALID_SOCKET) {
			printf("[TenviServer] accept failed, err=%d\n", WSAGetLastError());
			break;
		}

		int flag = 1;
		setsockopt(g_client, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));

		printf("[TenviServer] client connected from %s\n", inet_ntoa(ca.sin_addr));
		g_sendCount = 0;
		g_recvCount = 0;

		ServeClient();

		closesocket(g_client);
		g_client = INVALID_SOCKET;
		printf("[TenviServer] waiting for next client... (角色状态未重置，异常时请重启服务端)\n");
	}

	closesocket(listenSock);
	WSACleanup();
	printf("[TenviServer] stopped.\n");
	return 0;
}

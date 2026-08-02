// StandaloneServer.cpp - 冲锋岛(Tenvi) 联机改造里程碑1：独立服务端
// 把进程内 FakeServer 逻辑搬到独立进程，监听 127.0.0.1:8787，通过真实 socket 与客户端通讯
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>
#include "ClientPacket.h"
#include "ServerPacket.h"
#include "FakeServer.h"
#include "TenviData.h"
#include "../EmuMainTenvi/ConfigTenvi.h"

#pragma comment(lib, "ws2_32.lib")

using namespace std;

// FakeServer.cpp 里定义的版本包（头文件未声明，这里补上）
void VersionPacket();

// ---- 区域配置：原本由注入 DLL 读 ini 决定，独立服务端里程碑1 固定国服 CN v126 ----
static Region g_region = TENVI_CN;
static wstring g_regionStr = L"CN";
static wstring g_xmlPath = L"tv_xml";

Region GetRegion() { return g_region; }
wstring GetRegionStr() { return g_regionStr; }
wstring GetXMLPath() { return g_xmlPath; }

// 单客户端连接（里程碑1仅验证单客户端能连独立进程，不做多客户端同步）
static SOCKET g_client = INVALID_SOCKET;

// 替代 AutoResponse 的注入式发送：把 ServerPacket 编码后加 2 字节长度头经 socket 发回客户端
void SendPacket(ServerPacket &sp) {
    if (g_client == INVALID_SOCKET) return;
    vector<BYTE> data = sp.get();
    WORD len = (WORD)data.size();
    vector<BYTE> frame;
    frame.push_back((BYTE)(len & 0xFF));
    frame.push_back((BYTE)((len >> 8) & 0xFF));
    frame.insert(frame.end(), data.begin(), data.end());
    int r = send(g_client, (const char*)&frame[0], (int)frame.size(), 0);
    if (r == SOCKET_ERROR) {
        printf("[TenviServer] send failed, err=%d\n", WSAGetLastError());
    }
}

// 兼容 FakeServer.cpp 中调用的另外两个发送符号
void SendPacket2(ServerPacket &sp) { SendPacket(sp); }
void DelaySendPacket(ServerPacket &sp) { SendPacket(sp); }

// 从接收缓冲中提取一个完整包：假定 2 字节小端长度头，len = 明文内容长度（不含头）
static bool ExtractPacket(vector<BYTE> &buf, vector<BYTE> &outPkt) {
    if (buf.size() < 2) return false;
    WORD len = (WORD)(buf[0] | ((WORD)buf[1] << 8));
    if (buf.size() < (size_t)2 + len) return false;
    outPkt.assign(buf.begin() + 2, buf.begin() + 2 + len);
    buf.erase(buf.begin(), buf.begin() + 2 + len);
    return true;
}

int main(int argc, char **argv) {
    // 可选：命令行第一个参数覆盖 xml 数据目录
    if (argc > 1) {
        wchar_t wbuf[512] = { 0 };
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wbuf, 511);
        g_xmlPath = wbuf;
    }
    // 加载地图/NPC 数据（FakeServer 换图、刷怪要用）
    tenvi_data.set_xml_path(g_xmlPath);
    wprintf(L"[TenviServer] xml path = %s, region = %s\n", g_xmlPath.c_str(), g_regionStr.c_str());

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
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("0.0.0.0"); // 监听所有网卡（跨机部署用 0.0.0.0，本机用 127.0.0.1 也行）
    addr.sin_port = htons(8787);
    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[TenviServer] bind failed, err=%d\n", WSAGetLastError());
        return 1;
    }
    listen(listenSock, 1);
    printf("[TenviServer] listening on 0.0.0.0:8787, waiting for client...\n");

    g_client = accept(listenSock, NULL, NULL);
    if (g_client == INVALID_SOCKET) {
        printf("[TenviServer] accept failed\n");
        return 1;
    }
    printf("[TenviServer] client connected.\n");

    // 主动推送登录前准备包（模拟服务器连上即推世界列表）
    VersionPacket();
    WorldListPacket();

    vector<BYTE> buf;
    char tmp[8192];
    while (true) {
        int n = recv(g_client, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            printf("[TenviServer] client disconnected (n=%d)\n", n);
            break;
        }
        buf.insert(buf.end(), tmp, tmp + n);
        vector<BYTE> pkt;
        while (ExtractPacket(buf, pkt)) {
            ClientPacket cp(&pkt[0], (DWORD)pkt.size());
            FakeServer(cp); // 分发处理，内部通过 SendPacket 回包
        }
    }

    closesocket(g_client);
    closesocket(listenSock);
    WSACleanup();
    printf("[TenviServer] stopped.\n");
    return 0;
}

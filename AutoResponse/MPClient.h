#ifndef __MPCLIENT_H__
#define __MPCLIENT_H__

#include <Windows.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// 明文桥接协议 (Milestone 1)
//
// 客户端原生网络栈带加解密, 而模拟器的 hook 点在"明文层":
//   出站 EnterSendPacket -> 拿到的是加密前的明文
//   入站 OnPacket        -> 拿到的是解密后的明文
// 所以不走客户端自己的 socket, 而是在 DLL 内另开一条 socket 直连独立服务端,
// 两端跑纯明文 Tenvi 包。线格式由我们自己定义, 不需要猜客户端的分包方式。
//
// 帧格式: [4 字节小端 payload 长度][payload]
// payload[0] = type, 其后为数据
// ---------------------------------------------------------------------------

#define MP_TYPE_GAME 0 // 游戏明文包
#define MP_TYPE_CTRL 1 // 控制命令

// 控制命令: 客户端某些按钮不会真的发包(原模拟器直接本地伪造),
// 桥接后改为通知服务端, 由服务端回对应的包
#define MP_CTRL_HELLO     1
#define MP_CTRL_WORLDLIST 2
#define MP_CTRL_CHARLIST  3

bool MP_Start(HINSTANCE hinstDLL);
void MP_SendGame(const BYTE *p, DWORD n);
void MP_SendCtrl(BYTE cmd);
bool MP_PopPacket(std::vector<BYTE> &out);
bool MP_IsConnected();

#endif

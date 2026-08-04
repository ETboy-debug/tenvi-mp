#ifndef __MPCLIENT_H__
#define __MPCLIENT_H__

#include <Windows.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// 明文桥接协议 (Milestone 1)
//
// 帧格式: [4 字节小端 payload 长度][payload]
// payload[0] = type, 其后为数据
// ---------------------------------------------------------------------------

#define MP_TYPE_GAME 0 // 游戏明文包
#define MP_TYPE_CTRL 1 // 控制命令

#define MP_CTRL_HELLO     1
#define MP_CTRL_WORLDLIST 2
#define MP_CTRL_CHARLIST  3
#define MP_CTRL_LOGIN     4   // 登录: payload[2..] = utf8 "账号\0密码"
#define MP_CTRL_REGISTER  5   // 注册: payload[2..] = utf8 "账号\0密码"
#define MP_CTRL_LOGIN_RESULT 6    // 服务端回: payload[2] = 1 成功 / 0 失败
#define MP_CTRL_REGISTER_RESULT 7 // 服务端回: payload[2] = 1 成功 / 0 已存在

bool MP_Start(HINSTANCE hinstDLL);
void MP_SendGame(const BYTE *p, DWORD n);
void MP_SendCtrl(BYTE cmd);
bool MP_PopPacket(std::vector<BYTE> &out);
bool MP_IsConnected();
bool MP_IsAuthed();
void MP_SetAuthed(bool v);

// [MP] 原生登录界面凭据捕获(消息钩子)
bool MP_GetNativeCred(std::string &outAcc, std::string &outPw);
void NatClearCred();  // 清空捕获的凭据

// [MP] 发送带密码的登录请求 + 同步等待结果
void MP_SendLogin(const std::string &acc, const std::string &pw);
bool MP_WaitCtrlResult(BYTE expectCmd, int timeoutMs, BYTE &outByte);

#endif

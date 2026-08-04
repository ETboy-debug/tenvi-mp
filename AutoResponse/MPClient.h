// MPClient.h - 客户端侧明文桥接接口
#pragma once
#include <string>
#include <vector>

// ---- 常量 ----
#define MP_TYPE_GAME  0   // 游戏数据包(透传)
#define MP_TYPE_CTRL  1   // 控制命令包

// 控制命令码(服务端<->客户端)
#define MP_CTRL_HELLO           1   // 客户端->服务端: 握手
#define MP_CTRL_LOGIN           4   // 客户端->服务端: 登录请求(账号\0密码)
#define MP_CTRL_LOGIN_RESULT    5   // 服务端->客户端: 登录结果(1=成功 其他=失败)
#define MP_CTRL_WORLDLIST       10  // 客户端->服务端: 请求世界列表
#define MP_CTRL_CHARLIST        11  // 客户端->服务端: 请求角色列表

// ---- 导出函数(DLL内部使用, AutoResponse.cpp 调用) ----

// 启动桥接线程(从 DLLMain 调用)
bool MP_Start(HINSTANCE hinstDLL);

// 连接状态查询
bool MP_IsConnected();
bool MP_IsAuthed();        // 是否已通过认证
void MP_SetAuthed(bool v); // 设置认证状态

// 包操作
bool MP_PopPacket(std::vector<BYTE> &out);     // 取一个游戏包(供 MP_Pump 用)
void MP_SendGame(const BYTE *p, DWORD n);       // 发送游戏包到服务端
void MP_SendCtrl(BYTE cmd);                    // 发送控制命令
void MP_SendLogin(const std::string &acc, const std::string &pw); // 发带密码的登录请求

// 同步等待 ctrl 结果(阻塞轮询, 用于 LoginButton_Hook 等需要同步等待的场景)
bool MP_WaitCtrlResult(BYTE expectCmd, int timeoutMs, BYTE &outByte);

// ---- [v24] 登录包拦截认证系统 ----

// 尝试从发包中截获登录凭据(EnterSendPacket_Hook 调用)
// 返回: 0=不是登录包  1=已截获并发起认证(调用方应丢弃此包)  -1=凭据无效
int MP_InterceptLoginPacket(const BYTE *data, DWORD len);

// 每帧轮询认证结果(ProcessPacketCaller_Hook 调用)
// 返回: 0=无需处理  1=认证成功(已自动 SetAuthed+发WORLDLIST)  -1=失败  -2=超时
int MP_PollAuthResult();

// 获取最近一次认证的账号(用于错误提示)
void MP_GetLastCred(std::string &outAcc);

// 重置所有登录状态(认证失败/超时后调用)
void MP_ResetLoginState();

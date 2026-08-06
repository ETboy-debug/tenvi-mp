// MPClient.h - 客户端侧明文桥接 socket 接口
// [v29] GetAsyncKeyState 轮询键盘捕获方案
#ifndef MP_CLIENT_H
#define MP_CLIENT_H

#include <windows.h>
#include <string>
#include <vector>

// ---- 协议常量 ----
#define MP_TYPE_GAME  0   // 游戏明文包 -> CWvsContext (local player)
#define MP_TYPE_CTRL  1   // 控制命令
// [v50] Game packet that MUST be dispatched through CField instead of
// CWvsContext: remote players, monsters, map objects. Before v50 this
// distinction existed only inside the server process (SendPacket vs
// SendPacket2) and was lost on the wire, forcing the DLL to guess which
// 0x11 spawn packet was the local player. The guess broke whenever a
// remote spawn arrived before the local one.
#define MP_TYPE_GAME_FIELD 2

// 控制命令类型(必须与服务端 StandaloneServer.cpp 完全一致!)
#define MP_CTRL_HELLO           1  // 客户端握手
#define MP_CTRL_WORLDLIST       2  // 请求世界列表
#define MP_CTRL_CHARLIST        3  // 请求角色列表
#define MP_CTRL_LOGIN           4  // 登录请求(账号\0密码, 自动注册+验密)
#define MP_CTRL_REGISTER        5  // 注册请求
#define MP_CTRL_LOGIN_RESULT    6  // 登录结果(1=成功)
#define MP_CTRL_REGISTER_RESULT 7  // 注册结果(1=成功)

// ---- 网络基础 API ----

/// 启动网络线程(连接服务端 + 收包)。从 ini 读 ServerIP/ServerPort。
bool MP_Start(HINSTANCE hinstDLL);

/// 是否已连接
bool MP_IsConnected();

/// 发送游戏明文包到服务端
void MP_SendGame(const BYTE *p, DWORD n);

/// 发送控制命令
void MP_SendCtrl(BYTE cmd);

/// 发送带密码的登录请求(账号\0密码)
void MP_SendLogin(const std::string &acc, const std::string &pw);

/// 是否已认证通过
bool MP_IsAuthed();

/// 设置认证状态
void MP_SetAuthed(bool v);

/// 取出一个收到的游戏包(从队列)。返回 false 表示队列为空。
bool MP_PopPacket(std::vector<BYTE> &out);

/// [v50] Same as MP_PopPacket but also reports the dispatch context the
/// server tagged the packet with. ctx=true -> CWvsContext, false -> CField.
bool MP_PopPacketEx(std::vector<BYTE> &out, bool &ctx);

/// 阻塞等待指定类型的 ctrl 包结果。timeoutMs=0 为非阻塞。
bool MP_WaitCtrlResult(BYTE expectCmd, int timeoutMs, BYTE &outByte);

// ---- [v30] GetAsyncKeyState 键盘捕获 API ----

/// 启动键盘捕获线程(使用进程ID判断前台窗口, 无需传HWND)
void MP_StartCapture();

/// 停止键盘捕获
void MP_StopCapture();

/// 取出捕获到的凭据。返回 true 表示有账号内容。
bool MP_GetNativeCred(std::string &acc, std::string &pw);

/// 清空凭据缓冲区
void MP_ClearCred();

/// 重置登录状态(清凭据+重置认证标志)
void MP_ResetLoginState();

/// [v33] 同步重连服务端(阻塞式, 用于登录前按需连接)。返回是否成功。
bool MP_Reconnect();

#endif // MP_CLIENT_H

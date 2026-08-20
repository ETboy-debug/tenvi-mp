// MPClient.h -  socket 
// [v29] GetAsyncKeyState 
#ifndef MP_CLIENT_H
#define MP_CLIENT_H

#include <windows.h>
#include <string>
#include <vector>

// ----  ----
#define MP_TYPE_GAME  0   //  -> CWvsContext (local player)
#define MP_TYPE_CTRL  1   // 
// [v50] Game packet that MUST be dispatched through CField instead of
// CWvsContext: remote players, monsters, map objects. Before v50 this
// distinction existed only inside the server process (SendPacket vs
// SendPacket2) and was lost on the wire, forcing the DLL to guess which
// 0x11 spawn packet was the local player. The guess broke whenever a
// remote spawn arrived before the local one.
#define MP_TYPE_GAME_FIELD 2

// ( StandaloneServer.cpp !)
#define MP_CTRL_HELLO           1  // 
#define MP_CTRL_WORLDLIST       2  // 
#define MP_CTRL_CHARLIST        3  // 
#define MP_CTRL_LOGIN           4  // (\0, +)
#define MP_CTRL_REGISTER        5  // 
#define MP_CTRL_LOGIN_RESULT    6  // (1=)
#define MP_CTRL_REGISTER_RESULT 7  // (1=)

// ----  API ----

/// ( + ) ini  ServerIP/ServerPort
bool MP_Start(HINSTANCE hinstDLL);

/// 
bool MP_IsConnected();

/// 
void MP_SendGame(const BYTE *p, DWORD n);

/// 
void MP_SendCtrl(BYTE cmd);

/// (\0)
void MP_SendLogin(const std::string &acc, const std::string &pw);

/// 
bool MP_IsAuthed();

/// 
void MP_SetAuthed(bool v);

/// () false 
bool MP_PopPacket(std::vector<BYTE> &out);

/// [v50] Same as MP_PopPacket but also reports the dispatch context the
/// server tagged the packet with. ctx=true -> CWvsContext, false -> CField.
bool MP_PopPacketEx(std::vector<BYTE> &out, bool &ctx);

///  ctrl timeoutMs=0 
bool MP_WaitCtrlResult(BYTE expectCmd, int timeoutMs, BYTE &outByte);

// ---- [v30] GetAsyncKeyState  API ----

/// (ID, HWND)
void MP_StartCapture();

/// 
void MP_StopCapture();

///  true 
bool MP_GetNativeCred(std::string &acc, std::string &pw);

/// 
void MP_ClearCred();

/// (+)
void MP_ResetLoginState();

/// [v33] (, )
bool MP_Reconnect();

/// [v61-diag] Per-process diagnostic log path, e.g. "D:/mp_diag_1234.log".
/// Both clients used to share one fixed "D:/mp_diag.log" and DllMain opens it
/// with mode "w" on attach, so launching the SECOND client wiped every line the
/// FIRST client had written - including its self-spawn 0x11. That is why the
/// v60 log looked as if no 0x11 ever arrived. Never diagnose through a shared
/// file that a second process truncates.
const char *MP_DiagPath();

#endif // MP_CLIENT_H

#ifndef __FAKESERVER_H__
#define __FAKESERVER_H__

#include"ClientPacket.h"
#include"ServerPacket.h"
#include"TenviData.h"
#include"TemporaryData.h"

// FakeServer.cpp 使用的全局账号数据（定义在 TemporaryData.cpp）
// [MP] 每个连接线程一份独立会话状态, 这样多个玩家同时连服务端不会串号。
// 客户端 DLL 侧只有主线程访问, 行为不变。
extern thread_local TenviAccount TA;

#define MAPID_ITEM_SHOP 65535
#define MAPID_PARK 65534
#define MAPID_EVENT 62501

bool FakeServer(ClientPacket &cp);

// [MP] 跨连接/会话管理(实现分处 FakeServer.cpp / StandaloneServer.cpp)
extern thread_local int t_sid;
// [v50] context selects the receiving client's dispatch layer:
// true -> CWvsContext, false -> CField (remote players / objects).
void MP_BroadcastToSid(int sid, ServerPacket &sp, bool context = true);
void MP_RemovePlayer(int sid);
// [v61] Dispatch layer used for REMOTE-player packets (0x3D/0x11/0x12/0x0C).
// Runtime-configurable via "mp_ctx.cfg" next to StandaloneServer.exe so the
// layer can be flipped with a server restart instead of a 10-minute cloud
// build. History: v54 tested ctx=1 but was confounded by the window-reopen
// bug that wiped the stable client's field (only found in v60); v60 removed
// the reopen but used ctx=0. "no reopen + ctx=1" was never tested, and ctx=1
// is the only layer the stock single-player code ever used for 0x11.
bool MP_RemoteCtx();
// [v61] Whether to also push 0x3D (account data) for remote players. Defaults
// to OFF: 0x3D is the receiver's OWN account packet and would overwrite their
// identity once it is routed through CWvsContext. See mp_ctx.cfg.
bool MP_RemoteSend3D();
// [v55] 把客户端发出的游戏包(移动 0x0C 等)原样转发给同图其他玩家
void MP_ForwardToSameMap(const BYTE *pkt, DWORD len);

// test
void WorldListPacket();
void CharacterSelectPacket();
void CharacterListPacket();
void CharacterListPacket_Test();
void NPCTalkPacket(DWORD npc_id, std::wstring text);

#endif
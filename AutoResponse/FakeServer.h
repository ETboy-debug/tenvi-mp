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
// [v62] Whether to also push 0x3D (account data) for remote players. Defaults
// to ON now: 0x11 is dropped at 0x0048DCEF unless the character object already
// exists, and 0x3D (handler 0x00498E4F) is what creates it. See mp_ctx.cfg.
bool MP_RemoteSend3D();
// [v62] After pushing a remote 0x3D+0x11 pair, replay the RECEIVER's own 0x3D.
// The remote 0x3D overwrites the receiver's account context - v61b field report:
// the first player rendered the newcomer but could no longer move. Default ON.
bool MP_Restore3D();
// [v62] Send the player's own 0x11 BEFORE the cross-visibility loop. The client
// latches its identity onto the first 0x11 it receives; v61b showed the joining
// player locking onto the OTHER player's oid and driving a ghost. Default ON.
bool MP_SelfSpawnFirst();
// [v64] Movement relay strategy. Disassembly of CN v126 proves the raw 0x0C
// relay can never work: CField::OnPacket2 (0x004CBE34) only accepts opcodes
// 0x1E/0x42/0x5C/0xA3/0xA7/0xA8 and >0xA8 - 0x0C falls through to the reject
// branch at 0x004CBE8A. 0x0C is a CLIENT->SERVER opcode (CP_PLAYER_MOVEMENT)
// with no object id in it; the SP table has no "remote player moved" opcode at
// all because the stock emulator was single-player. So instead of relaying
// 0x0C we parse the destination coordinates out of its tail and re-send 0x11
// (CHARACTER_SPAWN) with the new position - the one packet already proven to
// place a remote character at an exact coordinate. Default ON.
bool MP_MoveAsSpawn();
// [v102] Movement relay fallback (pure-network, publishable). When moveAsSpawn
// is OFF, the SP table has no "remote player moved" opcode, so we cannot move
// an already-spawned remote avatar with a single packet. This mode destroys and
// rebuilds the remote character each step: 0x12 (remove old oid) + 0x3D (rebuild
// object) + 0x11 (spawn at new position). Stepped/flashing but guaranteed to
// move without any client memory writes. Default ON (the only working option).
bool MP_RebuildMove();
// [v104] B-route smooth movement. When ON, MP_ForwardToSameMap synthesizes the
// real SP remote-player-move packet [0x0C][oid LE4][movement path] (the client's
// own CWvsContext handler 0x4937A0 already animates it natively) instead of
// rebuilding. movement path is forwarded verbatim from the client's CP 0x0C body.
// Default OFF so the v102 rebuild stays the safe baseline until verified.
bool MP_SmoothMove();
// [v55] 把客户端发出的游戏包(移动 0x0C 等)原样转发给同图其他玩家
void MP_ForwardToSameMap(const BYTE *pkt, DWORD len);

// test
void WorldListPacket();
void CharacterSelectPacket();
void CharacterListPacket();
void CharacterListPacket_Test();
void NPCTalkPacket(DWORD npc_id, std::wstring text);

#endif
#include"FakeServer.h"
#include"AutoResponse.h"
#include"TemporaryData.h"
#include <mutex>
#include <map>
#include <cmath>
#include <vector>
#define NOMINMAX
#include <windows.h>

#ifdef MP_SERVER
#include "../StandaloneServer/db.h"
#include <cstdio>
// [DIAG] 崩溃定位标记: 直接写 stdout(服务端无缓冲, 落到 server_live.log)
#define MP_MARK(msg) do { printf("[TenviServer] MARK %s\n", msg); } while(0)

// [v84] Deployment sentinel string (grep-friendly binary marker).
static const char *MP_SERVER_VERSION_TAG = "MP_SERVER_V89_OID_REWRITE";
#else
#define MP_MARK(msg) do { } while(0)
#endif

thread_local TenviAccount TA; // [MP] 见 FakeServer.h: 按连接线程隔离会话

#ifdef MP_SERVER
// [MP] 在线玩家快照表(服务端跨线程共享): 用于把同图真人互刷给对方
struct RemotePlayer {
	int sid;
	std::wstring account;
	DWORD char_id;
	TenviCharacter chr;
	WORD map;
	float x, y;
	// [v65] last coordinate we actually broadcast as a movement update,
	// so we do not remove+respawn the remote character on every tiny step.
	float last_move_x, last_move_y;
	bool has_last_move;
};
static std::mutex g_playersMtx;
static std::map<int, RemotePlayer> g_players;
#endif
// ========== TENVI Packet Response ==========
#define TENVI_VERSION 0x1023

// 0x01
void VersionPacket() {
	ServerPacket sp(SP_VERSION);
	sp.Encode4(TENVI_VERSION); // 00491388, version
	sp.Encode4(1); // 00491391
	sp.Encode4(1); // 0049139A
	sp.Encode4(1); // 004913A4
	SendPacket(sp);
}

// 0x02
void CrashPacket() {
	ServerPacket sp(SP_CRASH);
	sp.Encode4(TENVI_VERSION); // version
	SendPacket(sp);
}

// 0x03
void LoginFailedPacket() {
	ServerPacket sp(SP_LOGIN_FAILED);
	SendPacket(sp);
}

// 0x04
void CharacterSelectPacket() {
	ServerPacket sp(SP_CHARACTER_SELECT);
	sp.Encode1(0); // 00492CE6, 0 = OK, 1 = login error
	sp.Encode4(-1); // 00492D5B saved to pointer
	sp.Encode1(0); // 00492D65 saved to pointer

	if (GetRegion() == TENVI_KR) {
		sp.Encode1(0);
		sp.EncodeWStr1(L"Tenvi KR v200");
	}

	SendPacket(sp);
}

// 0x05
int character_slot = 6;
void CharacterListPacket() {
	ServerPacket sp(SP_CHARACTER_LIST);
	sp.Encode1(0); // Number of Characters
	sp.Encode1(character_slot); // Number of Character Slot
	DelaySendPacket(sp);
}

void CharacterListPacket_Test() {
	ServerPacket sp(SP_CHARACTER_LIST);

#ifdef MP_SERVER
	// [MP] 每次列角色都从 DB 重载, 让 GM 改的等级/金币下一轮就生效
	TA.ReloadFromDB();
#endif

	sp.Encode1((BYTE)TA.GetCharacters().size()); // characters
	for (auto &chr : TA.GetCharacters()) {
		sp.Encode4(chr.id); // ID
		sp.Encode1(chr.job_mask);
		sp.Encode1((BYTE)chr.level); // level
		sp.EncodeWStr1(chr.name); // name
		sp.EncodeWStr1(L"");
		sp.Encode2(chr.job);
		sp.Encode2(chr.skin);
		sp.Encode2(chr.hair);
		sp.Encode2(chr.face);
		sp.Encode2(chr.cloth);
		sp.Encode2(chr.gcolor);
		// character equip, max 15
		for (int i = 0; i < 15; i++) {
			sp.Encode2(0);
		}
		// guardian equip, max 15
		for (auto gequip : chr.gequipped) {
			sp.Encode2(gequip);
		}
		sp.Encode2(chr.map); // mapid
	}

	if (GetRegion() == TENVI_KR) {
		sp.Encode1(0);
	}

	sp.Encode1(TA.slot); // character slots
	SendPacket(sp);
}

// 0x07
void DeleteCharacter() {
	ServerPacket sp(SP_DELETE_CHARACTER_MSG);
	sp.Encode1(0); // 00491C61, error code 1,2,4
	SendPacket(sp);
}

// 0x08
void WorldSelectPacket() {
	ServerPacket sp(SP_WORLD_SELECT);
	SendPacket(sp);
}

// 0x09
int worlds = 1;
int channels = 5;
void WorldListPacket() {
	ServerPacket sp(SP_WORLD_LIST);
	sp.EncodeWStr2(L""); // 004938BC, Message

	if (GetRegion() == TENVI_JP || GetRegion() == TENVI_CN) {
		sp.Encode1(0); // 004938C8, NetCafe
	}

	sp.Encode1(worlds); // 00493934, Number of Worlds
	for(int world = 0; world < worlds; world++) {
		sp.Encode1(2); // 00493979, World Mark
		sp.EncodeWStr1(L"Spica"); // 0049398A World Name
		sp.Encode8(0); // 004939AD

		sp.Encode1(1); // 004939BA
		{
			sp.EncodeWStr1(L"127.0.0.1"); // 004939D2, IP
			sp.Encode2(8787); // 004939F5, Port
		}

		sp.Encode1(channels); // 00493A0D, Number of channels
		for (int channel = 0; channel < channels; channel++) {
			sp.Encode1(channel + 1); // 00493A1E
			sp.Encode1(50); // 00493A28, Population (CH1 and CH2 are edited by client side lol)
		}
	}

	sp.Encode1(1); // 00493A6A
	sp.Encode1(0); // 00493A7B

	if (GetRegion() == TENVI_JP || GetRegion() == TENVI_CN) {
		sp.Encode1(0); // 00493A88
	}

	sp.Encode1(0); // 00493A92

	SendPacket(sp);
}

// 0x0A not used in JP
void CharacterSelectInvitedPacket() {
	ServerPacket sp(SP_CS_INVITED);
	sp.Encode1(0); // 00491CEF, 0-2
	sp.Encode4(0); // 00491E20, unique code
	SendPacket(sp);
}

// 0x0B not used in JP
void CharacterSelectUnknownMsgPacket() {
	ServerPacket sp(SP_CS_KOREAN_MSG);
	sp.Encode1(1); // 00491AF9, show dialog
	sp.Encode1(0); // 00491B1E, not used
	SendPacket(sp);
}

// 0x0C
void GetGameServerPacket() {
	ServerPacket sp(SP_GET_GAME_SERVER);
	sp.Encode1(0); // error code
	sp.Encode4(0x0100007F); // IP
	sp.Encode2(8787); // port
	sp.Encode4(0);
	sp.Encode4(0);
	SendPacket(sp);
}

// 0x0D
void GetLoginServerPacket() {
	ServerPacket sp(SP_GET_LOGIN_SERVER);
	sp.Encode1(0); // error code
	sp.Encode4(0);
	sp.Encode4(0);
	sp.Encode4(0x0100007F); // IP
	sp.Encode2(8787); // port
	SendPacket(sp);
}

// 0x0E
void ConnectedPacket() {
	ServerPacket sp(SP_CONNECTED);
	sp.Encode1(0); // error code
	SendPacket(sp);
}

// 0x0F
void MapResetPacket() {
	ServerPacket sp(SP_MAP_RESET);
	sp.Encode1(0); // error code
	sp.Encode4(0);
	sp.Encode4(0);
	sp.Encode4(0);
	sp.Encode2(0);
	sp.Encode2(0);
	SendPacket(sp);
}

// 0x10
// [v54c] target_sid >= 0: re-broadcast this map-change to a SPECIFIC client
// (re-opens its "accepting new players" window). The CN client only creates
// remote character objects while its own ChangeMap flow is running; once it
// finishes, later 0x11 spawns are silently dropped (0x48DCEF/0x48DD03).
void ChangeMapPacket(WORD mapid, float x = 0, float y = 0, int target_sid = -1) {
	ServerPacket sp(SP_MAP_CHANGE);
	sp.Encode1(0); // error code = 37
	sp.Encode2(mapid); // mapid
	sp.Encode1(0); // 1 = empty map?
	sp.Encode4(0);
	sp.EncodeFloat(x); // float value
	sp.EncodeFloat(y); // float value
	sp.Encode1(0);
	sp.Encode1(0);
	sp.Encode1(0); // disable item shop and park
	sp.Encode1(0);
	sp.Encode4(0);
	if (target_sid < 0) SendPacket(sp);
	else MP_BroadcastToSid(target_sid, sp, true);
}

// 0x11
void CharacterSpawnPacket(TenviCharacter &chr, float x = 0, float y = 0, BYTE dir = 0, int target_sid = -1, bool context = true) {
	ServerPacket sp(SP_CHARACTER_SPAWN);
	sp.Encode4(chr.id); // 0048DB9B id, where checks id?
	sp.EncodeFloat(x); // 0048DBA5, coordinate x
	sp.EncodeFloat(y); // 0048DBAF, corrdinate y
	sp.Encode1(dir); // 0048DBB9, direction 0 = left, 1 = right
	sp.Encode1(1); // 0048DBC6, guardian, 0 = guardian off, 1 = guardian on
	sp.Encode1(1); // 0048DBD3, death, 0 = death, 1 = alive
	sp.Encode1(0); // 0048DBE0, battle, 0 = change channel OK, 1 = change channel NG
	sp.Encode4(4444); // 0048DBFB, ???
	sp.Encode1(chr.job_mask); // 0048DC08
	sp.Encode1((BYTE)chr.level); // 0048DC2B

	if (GetRegion() == TENVI_HK || GetRegion() == TENVI_KRX) {
		sp.Encode1(1);
	}

	sp.EncodeWStr1(chr.name); // name
	sp.EncodeWStr1(L""); // guardian name
	sp.Encode1(0); // 0048DC8F
	sp.Encode1(1); // 0048DC9C
	sp.Encode2(chr.job); // 0048DCA9
	sp.Encode2(chr.skin); // 0048DCB7
	sp.Encode2(chr.hair); // 0048DCC5
	sp.Encode2(chr.face); // 0048DCD3
	sp.Encode2(chr.cloth); // 0048DCE1
	sp.Encode2(chr.gcolor); // 0048DCEF
	sp.Encode1(0); // 0048DCFD

	// character equip
	for (auto equip : chr.equipped) {
		sp.Encode8(0);
		sp.Encode2(equip);
	}

	// guardian equip
	for (auto gequip : chr.gequipped) {
		sp.Encode8(0);
		sp.Encode2(gequip);
	}

	sp.Encode2(0); // 0048DDC3
	sp.Encode2(0); // 0048DDD1
	sp.Encode2(0); // 0048DDE1
	sp.Encode2(0); // 0048DDF1
	sp.Encode2(0); // 0048DE01

	if (GetRegion() == TENVI_JP || GetRegion() == TENVI_CN) {
		sp.Encode1(0); // 0048DE11
		sp.Encode1(0); // 0048DE21
		sp.Encode1(0); // 0048DE35
	}

	sp.EncodeWStr1(L""); // guild
	sp.Encode1(0); // 0057B508
	sp.Encode1(0); // 0057B515
	sp.Encode1(0); // 0057B522
	sp.Encode1(0); // 0057B52F
	sp.Encode1(0); // 0057B53C
	sp.Encode1(0); // 0057B549
	sp.Encode1(0); // 0048DE7F
	sp.Encode1(0); // 0048DE8C
	sp.Encode1(0); // 0048DE9F
	sp.Encode1(0); // 0048DEAC
	sp.Encode1(0); // 0048DEB9
	sp.Encode1(0); // 0048E4F3
	sp.Encode4(0); // 0048E513
	sp.Encode1(0); // 0048E51D
	sp.Encode1(0); // 0048E5F9
	sp.Encode1(0); // 0048E60D
	sp.Encode1(0); // 0048E621
	sp.Encode4(0); // 0048E6A1
	sp.Encode2(0); // 0048E6AB
	sp.Encode4(0); // 0048E6C7
	sp.Encode4(0); // 0048E6D1
	sp.Encode4(0); // 0048E6DB
	sp.Encode4(0); // 0048E6E5
	sp.Encode4(0); // 0048E6EF
	sp.Encode1(0); // 0048E6F9
	sp.Encode1(0); // 0048E706
	sp.Encode4(0); // 0048E713
	sp.Encode4(0); // 0048E71D
	sp.Encode4(0); // 0048E727
	sp.Encode4(0); // 0048E731
	sp.EncodeWStr1(L""); // 0048E73F
	sp.Encode2(0); // 0048E74A
	sp.Encode2(0); // 0048E757
	sp.EncodeWStr1(L""); // 0048E768
	sp.Encode1(0); // 0048E773
	sp.Encode1(0); // 0048E780

	if (GetRegion() == TENVI_HK || GetRegion() == TENVI_KR || GetRegion() == TENVI_KRX) {
		sp.Encode1(0);
		sp.Encode1(0);
	}

	if (target_sid < 0) {
		if (context) SendPacket(sp);
		else SendPacket2(sp);
	}
	// [v57] REVERT remote 0x11 to CField (ctx=0). The v54 premise "CN client
	// renders remote 0x11 via CWvsContext" is contradicted by the live failure:
	// remote players stay invisible at ctx=1. Canonical rule (0x11 = CField/
	// ctx=0) is correct; the v50 ctx=0 failure was the ID-collision bug (now
	// fixed - oids are distinct in mp_diag). Self spawn (target_sid<0) stays
	// ctx=1 (CWvsContext) and remains correct.
	// [v77] Respect the caller-supplied `context` for remote targets as well.
	// Initial cross-visibility spawns (default true) stay on CWvsContext where
	// the client renders them. Movement updates explicitly pass false and are
	// routed to CField so the receiving client can update the remote avatar
	// position on the field layer. Previously this parameter was ignored and
	// every remote 0x11 went through MP_RemoteCtx(), which defaulted to ctx=1.
	else MP_BroadcastToSid(target_sid, sp, context);
}

// 0x12
// [v57] 0x12 移除随 0x11 一起退回 CField(ctx=0): 移除层必须与渲染层一致,
// 否则下线后对方仍显示. 远程 0x11 已退回 CField, 移除也必须同步.
void RemoveObjectPacket(DWORD object_id, int target_sid = -1) {
	ServerPacket sp(SP_REMOVE_OBJECT);
	sp.Encode4(object_id); // not only for character
	if (target_sid < 0) SendPacket(sp);
	else MP_BroadcastToSid(target_sid, sp, MP_RemoteCtx());
}

// 0x14
// [v54c] target_sid >= 0: send to a SPECIFIC client (used when re-broadcasting
// the full scene to re-open a player's ChangeMap window).
void CreateObjectPacket(TenviRegen &regen, int target_sid = -1) {
	ServerPacket sp(SP_CREATE_OBJECT);
	sp.Encode4(regen.id);
	sp.Encode2(regen.object.id); // npc, mob id
	sp.Encode1(0);

	if (GetRegion() == TENVI_KRX) {
		sp.Encode1(0);
	}

	sp.Encode4(0);
	sp.Encode1(regen.flip); // face right
	sp.Encode1(0); // fade in
	sp.Encode4(0);
	sp.Encode1(1); //show, if coordinate is far from your character, the object will be invisible
	sp.Encode2(0);
	sp.EncodeFloat(regen.area.left);
	sp.EncodeFloat(regen.area.bottom);
	sp.Encode2(0);
	sp.EncodeFloat(regen.area.left);
	sp.EncodeFloat(regen.area.top);
	sp.EncodeFloat(regen.area.right);
	sp.EncodeFloat(regen.area.bottom);
	sp.Encode1(0);
	sp.Encode1(0);
	{
		/*
		sp.Encode4(1);
		sp.Encode2(0);
		sp.Encode1(1);
		sp.Encode4(1);
		sp.Encode4(1337); // character id
		sp.Encode1(1);
		*/
	}

	if (GetRegion() != TENVI_JP) {
		sp.Encode1(0);
	}
	if (target_sid < 0) SendPacket(sp);
	else MP_BroadcastToSid(target_sid, sp, true);
}

// 0x20
void ActivateObjectPacket(TenviRegen &regen, int target_sid = -1) {
	ServerPacket sp(SP_ACTIVATE_OBJECT);
	sp.Encode4(regen.id);
	sp.Encode1(3); // 1 = fade in, 2 = !, 3 = walk, 4 = dash
	if (target_sid < 0) SendPacket(sp);
	else MP_BroadcastToSid(target_sid, sp, true);
}

// 0x21
void HitPacket(DWORD hit_from, DWORD hit_to) {
	ServerPacket sp(SP_HIT);
	sp.Encode4(hit_from); // 004867C1
	sp.Encode4(hit_to); // 004867C8
	sp.Encode1(0); // 00470977, Knock back
	sp.Encode4(0); // 00470984
	sp.Encode2(0); // 0047098E
	sp.Encode1(1); // 0047099B, hit count
	sp.Encode4(1337); // 004709AC, damage
	sp.Encode1(0); // 004709C1
	sp.Encode1(0); // 004709CE
	sp.Encode1(0); // 004709DB
	sp.Encode2(0); // 004709E8
	sp.Encode1(0); // 004709F5
	SendPacket(sp);
}

// 0x23
void ShowObjectPacket(TenviRegen &regen, int target_sid = -1) {
	ServerPacket sp(SP_SHOW_OBJECT);
	sp.Encode4(regen.id);
	sp.Encode1(1);
	sp.Encode1(1);
	sp.Encode2(0);
	sp.EncodeFloat(regen.area.left);
	sp.EncodeFloat(regen.area.bottom);
	if (target_sid < 0) SendPacket(sp);
	else MP_BroadcastToSid(target_sid, sp, true);
}

// 0x3C
// [v81] Revert v80: IN_MAP_TELEPORT is for the LOCAL character only. Sending
// it with a remote character id crashes the receiving client (the handler at
// 0x00489222 resolves the id through the local player/object table). Keep the
// original zero-destination signature for local-only use.
void InMapTeleportPacket(TenviCharacter &chr) {
	ServerPacket sp(SP_IN_MAP_TELEPORT);
	sp.Encode4(chr.id); // 00489222, character id
	sp.Encode1(1); // 0048923E, 0 = fall down, 1 = teleport to platform
	sp.Encode4(0); // 0048924B, x?
	sp.Encode4(0); // 00489255, y?
	sp.Encode1(0); // 0048925F, 0 = face left, 1 = face right
	sp.Encode1(0); // 00489269
	sp.Encode1(0); // 00489276
	sp.Encode1(1); // 00489283, 0 = no guardian, 1 = with guardian
	SendPacket(sp);
}

// 0x3D
// [v54b] target_sid >= 0: send this account data to a SPECIFIC remote client
// (via MP_BroadcastToSid) instead of the current one. The CN client only
// renders a 0x11 spawn if it can find the character object; that object is
// created by 0x3D. Broadcasting only the spawn (v50-v54) left the remote
// character unknown -> silently dropped. Now we send 0x3D + 0x11 together.
void AccountDataPacket(TenviCharacter &chr, int target_sid = -1) {
	ServerPacket sp(SP_ACCOUNT_DATA);
	sp.Encode4(0); // 00498E4F, ???
	sp.Encode4(chr.id); // 00498E5C, character id
	sp.EncodeWStr1(chr.name); // 00498E79, character name
	sp.EncodeWStr1(L""); // 00498EA5, ???
	sp.Encode1(chr.job_mask); // 00498ECD

	if (GetRegion() == TENVI_HK || GetRegion() == TENVI_KRX) {
		sp.Encode1(10); // unk
	}

	sp.Encode1((BYTE)chr.level); // 00498EF0
	sp.Encode8(1234); // 00498F0C, EXP
	sp.Encode8(77770503); // 00498F28, Coin (Gold, Silver, Bronze)
	sp.Encode8(0); // 00498F44, ???
	sp.Encode1(0); // 00498F60
	sp.Encode1(0); // 00498F70

	if (GetRegion() == TENVI_JP || GetRegion() == TENVI_CN) {
		sp.Encode1(0); // 00498F80
	}

	{
		sp.EncodeWStr1(L"TENVI"); // 0057A877, Guild Name
		{
			sp.Encode1(0); // 0057B508
			sp.Encode1(0); // 0057B515
			sp.Encode1(0); // 0057B522
			sp.Encode1(0); // 0057B52F
			sp.Encode1(0); // 0057B53C
			sp.Encode1(0); // 0057B549
		}
	}
	sp.EncodeWStr1(L"chr unk1"); // 00498FA0

	if (GetRegion() == TENVI_JP) {
		sp.Encode1(0); // 00498FC8
		sp.Encode1(0); // 00498FD8
		sp.Encode1(0); // 00498FF0
	}

	// loop 4
	{
		sp.Encode1(5 * 8); // 00499024, Equip Slot
		sp.Encode1(5 * 8); // 00499024, Item Slot
		sp.Encode1(5 * 8); // 00499024, Cash Slot
		sp.Encode1(4 * 10); // 00499024, Card Slots

		if (GetRegion() == TENVI_HK || GetRegion() == TENVI_KR || GetRegion() == TENVI_KRX) {
			sp.Encode1(4); // unk
		}

	}
	sp.EncodeWStr1(L"TenviTest"); // 00499044, Profile Message
	sp.Encode1(0); // 0049906C, ???
	sp.Encode1(0); // 004990B3
	// loop
	{
		/*
		sp.Encode1(0);
		sp.Encode2(0);
		sp.Encode1(0);
		*/
	}
	sp.Encode1(0); // 00499101, ???
	sp.Encode4(0); // 00499117
	sp.Encode1(0); // 00499124
	sp.Encode1(0); // 00499134, Married?
	sp.Encode4(0); // 00499144, Partner Character ID?
	sp.EncodeWStr1(L""); // 00499155, Partner Name?
	sp.Encode1(0); // 0049917D, Item Shop New Icon
	sp.Encode1(0); // 0049918D, Item Shop Box Icon
	sp.Encode1(0); // 0049919D, ???

	if (GetRegion() == TENVI_CN) {
		sp.Encode1(0);
	}

	// [v57] 0x3D 远程分支退回 CField(ctx=0), 与 0x11 出生同层: 客户端在该层
	// 用 0x3D 建对象后, 同一层的 0x11 才能引用到 oid 并渲染.
	if (target_sid < 0) SendPacket(sp);
	else MP_BroadcastToSid(target_sid, sp, MP_RemoteCtx());
}

// 0x41
void PlayerHitPacket(TenviCharacter &chr) {
	ServerPacket sp(SP_PLAYER_HIT);
	sp.Encode4(1); // 0048693A
	sp.Encode4(chr.id); // 00486941
	sp.Encode4(0); // 0045D825, 0 or 4,8 (�_��)
	sp.Encode2(0); // 0045D82F
	sp.Encode1(1); // 0045D83C, hit count
	sp.Encode2(1337); // 0045D84D, damage
	sp.Encode2(0); // 0045D865
	sp.Encode1(0); // 0045D872
	sp.Encode1(0); // 0045D87F
	sp.Encode2(0); // 0045D88C
	sp.Encode1(0); // 0045D899
	SendPacket(sp);
}

// 0x42
void PlayerLevelUpPacket(TenviCharacter &chr) {
	ServerPacket sp(SP_PLAYER_LEVEL_UP);
	sp.Encode4(chr.id); // 00488FD1, id
	sp.Encode1(chr.level + 1); // 00488FDB, 0 = rank up, others are level
	SendPacket(sp);
}

// 0x45
// 0x45
void PlayerSPPacket(TenviCharacter &chr) {
	ServerPacket sp(SP_PLAYER_STAT_SP);
	sp.Encode2(chr.sp);
	SendPacket(sp);
}
// 0x46
void PlayerAPPacket(TenviCharacter &chr) {
	ServerPacket sp(SP_PLAYER_STAT_AP);
	sp.Encode2(chr.ap);
	SendPacket(sp);
}

// 0x47
void PlayerStatPacket(TenviCharacter &chr) {
	ServerPacket sp(SP_PLAYER_STAT_ALL);
	sp.Encode2(3000); // 004956F5, HP
	sp.Encode2(4000); // 00495713, MAXHP
	sp.Encode2(1000); // 0049572F, MP
	sp.Encode2(2000); // 0049574B, MAXMP
	sp.Encode2(chr.stat_str); // 00495767, �� (STR)
	sp.Encode2(chr.stat_dex); // 00495783, �q�� (DEX)
	sp.Encode2(chr.stat_hp); // 0049579F, �̗� (HP)
	sp.Encode2(chr.stat_int); // 004957BB, �m�\ (INT)
	sp.Encode2(chr.stat_mp); // 004957D7, �m�b (MP)
	sp.Encode2(988); // 004957F3, �����_���[�W Min
	sp.Encode2(1006); // 0049580F, �����_���[�W Max
	sp.Encode2(1000); // 0049582B, �����U����
	sp.Encode2(2718); // 00495847, ���@�U����
	sp.Encode2(1887); // 00495863, �h���
	sp.Encode2(9130); // 0049587F, ����������
	sp.Encode2(9763); // 004958A7, ���@������
	sp.Encode2(129); // 004958CF, ���
	sp.Encode2(189); // 004958F7, �����N���e�B�J��
	sp.Encode2(2279); // 0049591F, ���@�N���e�B�J��

	if (GetRegion() == TENVI_KRX) {
		sp.Encode2(0);
	}

	sp.Encode2(131); // 00495947, ��s�X�s�[�h
	sp.Encode2(100); // 0049596F, ���s�X�s�[�h
	sp.Encode2(22); // 00495997, ����R��
	sp.Encode2(23); // 004959B3, �X��R��

	if (GetRegion() != TENVI_KRX) {
		sp.Encode2(24); // 004959CF, ����R��
	}

	sp.Encode2(25); // 004959EB, ����R��
	sp.Encode2(26); // 00495A07, �Œ�R��

	if (GetRegion() != TENVI_KRX) {
		sp.Encode2(0); // 00495A23, �͍���
		sp.Encode2(0); // 00495A42, �q������
		sp.Encode2(0); // 00495A61
		sp.Encode2(0); // 00495A80
		sp.Encode2(0); // 00495A9F
	}
	else {
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.EncodeFloat(0.0);
		sp.Encode4(0);
	}


	SendPacket(sp);
}

// 0x4A
void GuardianSummonPacket(TenviCharacter &chr, bool bSummon) {
	ServerPacket sp(SP_GUARDIAN_SUMMON);
	sp.Encode4(chr.id);
	sp.Encode1(bSummon ? 0x01 : 0x00);
	SendPacket(sp);
}

// 0x4B
void EmotionPacket(TenviCharacter &chr, BYTE emotion) {
	ServerPacket sp(SP_EMOTION);
	sp.Encode4(chr.id); // 0048608E, character id
	sp.Encode1(emotion); // 00486099, emotion
	SendPacket(sp);
}

// 0x54
void WorldMapUpdatePacket(BYTE area_code) {
	ServerPacket sp(SP_WORLD_MAP_UPDATE);
	sp.Encode1(area_code); // 00496E95
	sp.Encode1(0); // 00496EB7
	sp.Encode1(1); // 00496ED7
	sp.Encode1(area_code); // 00496EE7
	DelaySendPacket(sp);
}

void WorldMapUpdatePacketTest(BYTE area_code) {
	ServerPacket sp(SP_WORLD_MAP_UPDATE);
	sp.Encode1(area_code);

	for (int i = 1; i < 256; i++) {
		sp.Encode1((BYTE)i);
	}
	sp.Encode1(0);

	std::vector<BYTE> activated_area;

	activated_area.push_back(8); // �V�����@�A�C�����h
	activated_area.push_back(2); // ���u���A�C�����h
	activated_area.push_back(5); // �^���[B1�A�C�����h
	activated_area.push_back(6); // �~�m�X�A�C�����h

	if (GetRegion() != TENVI_HK) {
		activated_area.push_back(1); // �r�L�E�B�j�[�A�C�����h
		activated_area.push_back(3); // �t�@���g���A�C�����h
		activated_area.push_back(4); // �v�`�|�`�p�[�N
	}

	if (GetRegion() == TENVI_HK || GetRegion() == TENVI_KR || GetRegion() == TENVI_KRX) {
		activated_area.push_back(7); // �W����
	}

	sp.Encode1(activated_area.size()); // Number of Islands
	for (auto &v : activated_area) {
		sp.Encode1(v);
	}

	DelaySendPacket(sp);
}


// 0x5B
void PlayerRevivePacket(TenviCharacter &chr) {
	ServerPacket sp(SP_PLAYER_REVIVE);
	sp.Encode4(chr.id);
	SendPacket(sp);
}


// 0x5C
void EnterItemShopErrorPacket() {
	ServerPacket sp(SP_ITEM_SHOP_ERROR);
	sp.Encode1(1); // 004C93B9, error code 1-7
	SendPacket(sp);
}

// 0x66
void UpdateSkillPacket(TenviCharacter &chr, WORD skill_id) {
	ServerPacket sp(SP_UPDATE_SKILL);
	sp.Encode4(chr.id); // 00485E65, character id
	sp.Encode2(skill_id); // 00485E6F, skill id
	sp.Encode1(1); // 00485E7A, 0 = failed, 1 = success
	DelaySendPacket(sp);
}

// 0x6D
void InitSkillPacket(TenviCharacter &chr) {
	ServerPacket sp(SP_PLAYER_SKILL_ALL);
	sp.Encode1((BYTE)chr.skill.size()); // 0049977E, number of skills

	for (auto v : chr.skill) {
		sp.Encode1(1); // 00499792, idk
		sp.Encode2(v.id); // 0049979F, skill id
		sp.Encode1(v.level); // 004997AA, skill point
	}

	SendPacket(sp);
}

// 0xE0
enum BoardAction {
	Board_Spawn = 0,
	Board_Remove = 1,
	Board_AddInfo = 2,
};
void BoardPacket(BoardAction action, std::wstring owner = L"", std::wstring msg = L"") {
	ServerPacket sp(SP_BOARD);

	sp.Encode1(action); // 0048F653, 0 = spawn object, 1 = remove object, 2 = insert info

	switch (action) {
	case Board_Spawn: {
		sp.Encode4(3131); // 0048AEF6 object id
		sp.Encode4(1337); // 0048AF00 ???
		sp.EncodeWStr1(owner); // 0048AF0E ???
		sp.EncodeWStr1(msg); // 0048AF1D message
		sp.Encode4(0); // 0048AF28
		sp.Encode4(0); // 0048AF32
		sp.Encode1(0); // 0048AF3C
		sp.Encode1(3); // 0048AF46 board type
		break;
	}
	case Board_Remove: {
		sp.Encode4(3131); // object id
		break;
	}
	case Board_AddInfo: {
		sp.Encode4(3131); // object id
		sp.EncodeWStr1(msg); // 0048AF1D message
		break;
	}
	default: {
		// error
		return;
	}
	}

	SendPacket(sp);
}

// [FIX] NPC dialogue response: when player clicks NPC, show basic text
void NPCTalkPacket(DWORD npc_id, std::wstring text) {
	ServerPacket sp(SP_NPC_TALK);
	sp.Encode4(npc_id);
	sp.Encode1(0); // msg_type 0 = normal talk
	sp.EncodeWStr2(text);
	SendPacket(sp);
	// Also show BoardPacket to visually confirm CP_NPC_TALK is being handled
	BoardPacket(Board_Spawn, L"NPC", L"Clicked! ID=" + std::to_wstring(npc_id));
}

// ========== Functions ==================

void SpawnObjects(TenviCharacter &chr, WORD map_id, int target_sid = -1) {
	MP_MARK("SpawnObjects before get_map");
	std::vector<TenviRegen> &regens = tenvi_data.get_map(map_id)->GetRegen();
	MP_MARK("SpawnObjects after get_map");
	for (auto &regen : regens) {
		CreateObjectPacket(regen, target_sid);
		ShowObjectPacket(regen, target_sid);
		ActivateObjectPacket(regen, target_sid);
	}
	MP_MARK("SpawnObjects loop done");
}

#ifdef MP_SERVER
// [MP] 玩家断开: 从在线表移除, 并通知同图其他人移除其对象
void MP_RemovePlayer(int sid) {
	RemotePlayer rp;
	{
		std::lock_guard<std::mutex> lk(g_playersMtx);
		auto it = g_players.find(sid);
		if (it == g_players.end()) return;
		rp = it->second;
		g_players.erase(it);
	}
	std::lock_guard<std::mutex> lk(g_playersMtx);
	for (auto &kv : g_players) {
		if (kv.second.map == rp.map)
			RemoveObjectPacket(rp.char_id, kv.first);
	}
}
#else
void MP_RemovePlayer(int) {}
#endif

#ifdef MP_SERVER
// [v64] Movement sync, rebuilt on disassembly evidence instead of guesswork.
//
// Why the v55..v63 "relay 0x0C verbatim" approach could never work:
//   * 0x0C is CP_PLAYER_MOVEMENT - a CLIENT->SERVER opcode. Its body is a list
//     of movement segments and carries NO object id, so the receiving client
//     has no way to know which character moved. (The old log line printed
//     "oid=%08X" off pkt+1 and produced a different value every packet - that
//     was movement data being misread as an id.)
//   * CField::OnPacket2 (0x004CBE34, CN v126) compares the opcode against
//     0x1E / 0x42 / 0x5C / 0xA3 / 0xA7 / 0xA8 and >0xA8 only; anything else
//     falls through to the reject branch at 0x004CBE8A. 0x0C is dropped.
//   * The SP (server->client) opcode table has no "remote player moved" entry
//     at all - the stock emulator was single-player and never needed one.
//
// So we synthesize movement out of the one packet already proven to place a
// remote character at an exact coordinate: 0x11 CHARACTER_SPAWN. We parse the
// destination out of the 0x0C tail ([float x][float y][1 byte terminator]),
// cache it, and re-send 0x11 to everyone else on the map. The result is a
// stepped ("teleporting") movement rather than a smooth walk, but the remote
// character finally tracks its owner. Set the 5th char of mp_ctx.cfg to '0'
// to fall back to the old verbatim relay for comparison.
// ---------------------------------------------------------------------------
// [v108] Structured CP 0x0C parser - replaces the fragile "last 9 bytes" guess.
//
// Layout verified against 19/19 complete raw dumps in _srv_v106.log (0 misses):
//
//   [0x0C] [segment A] [segment B] [tail:1]
//   segment = [flag:1]  and, when flag != 0, [count:1][element * count]
//   element = 18 bytes:
//       [0..1]   u16 tick     [2..3]   u16 b       [4..5]   i16 delta
//       [6]      u8  d        [7]      u8  stance  [8]      u8  layer  (0x15/0x16)
//       [9]      u8  z        [10..13] f32 x       [14..17] f32 y
//
// Why this matters: the old code read x/y from pkt+len-9 unconditionally. That
// offset only lands on the coordinates when segment B carries elements. When
// segment B is empty (flag=0, i.e. a single 0x00 byte) the read slides one byte
// into the middle of x and yields garbage - typically (0,0). Those were the
// "poison packets" that teleported peers to the map origin and produced the
// user-visible "runs a few steps then freezes". Parsing the real structure
// eliminates the entire failure class instead of filtering its symptom.
// ---------------------------------------------------------------------------
struct MPMovePoint { float x; float y; BYTE stance; };

static bool MP_ParseCP0C(const BYTE *pkt, DWORD len, std::vector<MPMovePoint> &pts) {
	pts.clear();
	if (len < 4 || pkt[0] != 0x0C) return false;
	DWORD p = 1;
	for (int seg = 0; seg < 2; seg++) {
		if (p >= len) return false;
		BYTE flag = pkt[p++];
		if (flag == 0) continue;               // empty segment: flag byte only
		if (p >= len) return false;
		BYTE count = pkt[p++];
		if (count == 0) continue;
		if (p + (DWORD)count * 18u > len) return false;
		for (BYTE i = 0; i < count; i++) {
			const BYTE *e = pkt + p + (DWORD)i * 18u;
			MPMovePoint mp;
			memcpy(&mp.x, e + 10, 4);
			memcpy(&mp.y, e + 14, 4);
			mp.stance = e[7];
			pts.push_back(mp);
		}
		p += (DWORD)count * 18u;
	}
	if (len - p != 1) return false;            // exactly one tail byte remains
	return !pts.empty();
}

void MP_ForwardToSameMap(const BYTE *pkt, DWORD len) {
	BYTE op = (len > 0) ? pkt[0] : 0;

	// [v108] Destination comes from the LAST element of the parsed path.
	bool has_pos = false;
	float nx = 0.0f, ny = 0.0f;
	BYTE mv_stance = 0;
	BYTE mv_dir = 0;        // facing: 0 = left, 1 = right (derived from dx)
	int mv_points = 0;
	bool mv_struct = false;
	// [v110] Hoisted to function scope: the downstream 0x0C builder below needs
	// the whole path, not just its last point.
	std::vector<MPMovePoint> mv_pts;
	if (op == 0x0C && len >= 10) {
		std::vector<MPMovePoint> &pts = mv_pts;
		if (MP_ParseCP0C(pkt, len, pts)) {
			mv_struct = true;
			mv_points = (int)pts.size();
			nx = pts.back().x;
			ny = pts.back().y;
			mv_stance = pts.back().stance;
			has_pos = true;
		} else {
			// Fallback to the legacy tail guess so an unknown packet variant
			// still yields movement rather than nothing at all.
			// [v121] Guard against short packets: a length under 9 makes the
			// pkt+len-9 / pkt+len-5 reads go out of bounds and raise an access
			// violation that drops the sender's connection (seen on mount/rider
			// packets). Only guess when there is room for the 8 coordinate bytes.
			if (len >= 9) {
				memcpy(&nx, pkt + len - 9, 4);
				memcpy(&ny, pkt + len - 5, 4);
				has_pos = true;
			}
		}
		// Tenvi world coordinates stay well inside this box; anything outside
		// means we mis-parsed a variant packet layout, so skip the rewrite.
		if (!(nx > -1.0e5f && nx < 1.0e5f && ny > -1.0e5f && ny < 1.0e5f)) {
			has_pos = false;
		}
	printf("[MP-PARSE v118] struct=%d pts=%d last=(%.1f,%.1f) stance=%d len=%d dir=%d\n",
		mv_struct ? 1 : 0, mv_points, nx, ny, (int)mv_stance, (int)len, (int)mv_dir);
		// [v103-diag] raw hex dump of the movement packet so we can reverse the
		// real x/y layout offline (the float-tail assumption breaks x -> -0.1).
		{
			char hb[256]; int hi = 0; hi += snprintf(hb + hi, sizeof(hb) - hi, "[MP-RAW v103] op=%02X len=%d:", (unsigned)op, (int)len);
			DWORD dump = len > 48 ? 48 : len;
			for (DWORD i = 0; i < dump; i++) hi += snprintf(hb + hi, sizeof(hb) - hi, " %02X", pkt[i]);
			printf("%s\n", hb);
		}
	}

	WORD my_map = 0;
	TenviCharacter me;
	{
		std::lock_guard<std::mutex> lk(g_playersMtx);
		auto it = g_players.find(t_sid);
		if (it == g_players.end()) {
			printf("[MP-FWD] sid=%d not in players table, skip\n", t_sid);
			return;
		}
		my_map = it->second.map;
		if (has_pos) {
			it->second.x = nx;
			it->second.y = ny;
		}
		me = it->second.chr;
	}

	struct TargetInfo { int sid; TenviCharacter chr; };
	std::vector<TargetInfo> targets;
	{
		std::lock_guard<std::mutex> lk(g_playersMtx);
		for (auto &kv : g_players) {
			if (kv.first == t_sid) continue;
			if (kv.second.map == my_map) targets.push_back({ kv.first, kv.second.chr });
		}
	}
	printf("[MP-FWD] v65 sid=%d map=%d targets=%zu op=%02X pos=%d x=%.1f y=%.1f\n",
		(int)t_sid, (int)my_map, targets.size(), (unsigned)op,
		has_pos ? 1 : 0, nx, ny);
	if (targets.empty()) return;

	// [v106] B-route SMOOTH movement (pure network, NO memory writes, native
	// client animation). Reverse-engineering of the client CWvsContext dispatch
	// table (0x49391C) shows:
	//   - opcode 0x0C (handler 0x48d4ea) = LOCAL player movement. It decodes the
	//     path from byte[1] and drives the LOCAL avatar via the field object; any
	//     4 bytes at [1..4] are path data, NOT an id (the client's own CP 0x0C is
	//     [0x0C][path], no oid). So forwarding 0x0C to a peer does NOT move the
	//     remote avatar -> that is why v104/v105 froze.
	//   - opcode 0x0D (handler 0x4881d1) = REMOTE player movement. It decodes a
	//     4-byte oid from byte[1..4] (0x42acdd lookup) then applies the movement
	//     path (0x45806d / 0x458388) to THAT avatar smoothly. This is the SP
	//     packet the client already knows how to animate for other players.
	// Correct server->peer packet:
	//     [0x0D][remote oid 4 LE][movement path from sender CP byte[1]]
	// [v107] PROVEN BY LIVE HEX (v106 log line 134):
	//   0C | 01 02 C6 11 | 79 04 D6 FF 00 | 09 16 00 | 5E 8A 8E C3 | 00 00 A6 43 ...
	//   ^op  ^sender oid   ^path nodes...              ^x float LE   ^y float LE
	// [MP-HDL] printed oid=11C60201 == bytes[1..4] read little-endian, so the
	// client's CP 0x0C DOES carry its own oid at [1..4] (the v106 comment claiming
	// "CP has no oid" was wrong). Appending from pkt+1 duplicates that oid after
	// the one we already wrote -> path shifted 4 bytes -> client misparses ->
	// avatar frozen (exactly the v104 failure). Forward from pkt+5: skip
	// 1 opcode byte + 4 sender-oid bytes, keep only the movement path.
	// Falls back to rebuild below when smoothMove is OFF (cfg 7th char).
	// [v110] ---------------------------------------------------------------
	// The v107 code above was wrong on BOTH counts and is replaced wholesale:
	//   1) It sent opcode 0x0D. Handler 0x4881D1 reads an oid then calls
	//      0x45806D(0) = SetMoving(FALSE) and nothing else - it can only STOP
	//      an avatar. No path is read, no coordinate is written. Peers could
	//      never move, no matter what payload followed.
	//   2) It appended `pkt+5`, assuming the client's CP 0x0C carried its own
	//      oid at [1..4]. It does not: the [MP-HDL] "oid" printed a different
	//      value every packet because those bytes are path data (flag, count,
	//      tick). So the relay also truncated 4 bytes of real path.
// [v119] Restore V110 golden movement relay. Confirmed via git-history recovery of
// the build the user verified working ("互相旺"): opcode 0x0C + explicit oid +
// absolute (x,y) int16 pairs (path mode 0), broadcast on CField (ctx=false).
// The disasm notes that misled v114-v118 were WRONG for this client: the 0x0C
// mover (handler 0x48D4EA) is reached on the in-game CField/type-2 dispatch path,
// and the path decoder (0x45CAEE) consumes [count:1][int16*count] as ABSOLUTE
// coords here (delta teleports, absolute walks) -- V110 proved it live.
	if (MP_SmoothMove() && op == 0x0C && has_pos && !mv_pts.empty()) {
		std::vector<short> elems;
		for (size_t i = 0; i < mv_pts.size(); i++) {
			elems.push_back((short)mv_pts[i].x);
			elems.push_back((short)mv_pts[i].y);
		}
		if (elems.size() > 255) elems.resize(255);
		if (elems.empty()) {
			printf("[MP-FWD] v119 sp0x0C skip: empty path\n");
			return;
		}
		for (auto &t : targets) {
			ServerPacket sp;
			sp.Encode1(0x0C);
			sp.Encode4(me.id);                 // remote oid, 4-byte LE
			sp.Encode1((BYTE)elems.size());    // count = number of int16 values
			for (size_t k = 0; k < elems.size(); k++)
				sp.Encode2((WORD)(unsigned short)elems[k]);
			sp.Encode1(mv_stance);             // stance
			MP_BroadcastToSid(t.sid, sp, false);  // [v121] CField (ctx=false): remote-move receiver is the CField 0x0C handler. V110-proven (birth=CWvsContext, move=CField).
			printf("[MP-FWD] v121 sp0x0C -> sid=%d oid=%08X count=%d stance=%d dst=(%.1f,%.1f)\n",
				t.sid, (unsigned)me.id, (int)elems.size(), (int)mv_stance, nx, ny);
		}
		MP_MARK("MP-FWD v119 sp-0x0C-path");
		return;
	}


	// [v102] Destroy-rebuild movement (pure network, NO memory writes).
	// CN v126 SP table has NO dedicated "remote player moved" opcode, and the
	// client ignores repeated 0x11 for an already-rendered avatar (freezes).
	// So the only network-only way to move a remote avatar is to tear it down
	// (0x12 remove) and recreate it (0x3D build object + 0x11 render) at the
	// new position. Triggered by MP_RebuildMove() (6th cfg bit). Movement
	// looks staircase/teleporty and may strobe, but it MOVES and is fully
	// publishable (identical for every player, no per-machine memory offsets).
	// Smoothing via a real remote-move recv opcode is deferred until IDA
	// yields the correct handler.
	if (MP_RebuildMove() && has_pos) {
		// [v107] Poison-packet guard. The v106 log shows 11 CP 0x0C packets parsed
		// as x=0.0 y=0.0 (not every 0x0C sub-packet carries coordinates in its last
		// 8 bytes), and 6 of them were rebuilt anyway -> the peer avatar was torn
		// down and recreated at the MAP ORIGIN, off-screen / under the floor. That
		// is the user-reported "runs a few steps then freezes / vanishes". The
		// MoveAsSpawn branch already had this filter; rebuild never did.
		// [v108] Kept as a second line of defence only. MP_ParseCP0C now derives
		// the coordinate from the real last path element, so genuine (0,0) reads
		// should no longer occur; this guard only catches unknown packet variants
		// that fell through to the legacy tail-guess path.
		if (fabsf(nx) < 0.001f && fabsf(ny) < 0.001f) {
			printf("[MP-FWD]   v108 skip poison (0,0) pos\n");
			return;
		}
		// [v108] 40px produced very long teleport hops ("jumping"). With the
		// coordinate now parsed correctly we can afford denser updates.
		const float MOVE_THRESHOLD = 20.0f; // larger = less strobe, more teleporty
		bool do_update = false;
		{
			std::lock_guard<std::mutex> lk(g_playersMtx);
			auto it = g_players.find(t_sid);
			if (it == g_players.end()) return;
			if (it->second.has_last_move) {
				float ddx = nx - it->second.last_move_x;
				float ddy = ny - it->second.last_move_y;
				mv_dir = (ddx < 0.0f) ? 0 : 1;   // moving left -> face left
				if (ddx * ddx + ddy * ddy >= MOVE_THRESHOLD * MOVE_THRESHOLD)
					do_update = true;
			} else {
				do_update = true; // first movement in this session
				mv_dir = 0;
			}
		}
		if (!do_update) {
			printf("[MP-FWD]   skip rebuild (under %.0fpx)\n", MOVE_THRESHOLD);
			return;
		}
		for (auto &t : targets) {
			// 1) remove existing remote avatar at old position
			RemoveObjectPacket(me.id, t.sid);
			// 2) rebuild object (0x3D) then render at new position (0x11)
			AccountDataPacket(me, t.sid);
			CharacterSpawnPacket(me, nx, ny, mv_dir, t.sid, MP_RemoteCtx());
			printf("[MP-FWD] v103 rebuild-move -> sid=%d oid=%08X to (%.1f,%.1f)\n",
				t.sid, (unsigned)me.id, nx, ny);
		}
		MP_MARK("MP-FWD v102 rebuild-move");
		{
			std::lock_guard<std::mutex> lk(g_playersMtx);
			auto it = g_players.find(t_sid);
			if (it != g_players.end()) {
				it->second.last_move_x = nx;
				it->second.last_move_y = ny;
				it->second.has_last_move = true;
			}
		}
		return;
	}

	if (MP_MoveAsSpawn() && has_pos) {
		// [v70] Filter out the (0,0) poison packets that v69 logged by the
		// hundreds. The 0x0C parser reads the last 8 bytes as floats, but not
		// every 0x0C sub-packet carries coordinates there - sometimes we get
		// zeros. Spawning a remote avatar at (0,0) moves it to the map origin
		// (often off-screen / under the floor), which looks like "he ran and
		// disappeared". Only accept non-zero coordinates for the respawn.
		if (fabsf(nx) < 0.001f && fabsf(ny) < 0.001f) {
			printf("[MP-FWD]   skip invalid (0,0) pos\n");
			return;
		}
		// [v69] Each 0x11 respawn triggers the client-side "entered map" UI
		// effect (map name flashing), because the client treats every
		// CharacterSpawn as a birth/arrival event. We cannot stop that from
		// the server, so the only lever is to respawn less often.
		// [v70] 100px is a compromise: less strobing than 30/50px, but not as
		// teleporty as 150px, and the (0,0) filter stops the disappear bug.
		// [v73] Lowered to 30px for smoother movement. Combined with the
		// no-remove optimization below, the teleport feel is greatly reduced.
		// [v76] Drop to 10px while client-side interpolation is enabled. The
		// DLL now lerp-smoothes each remote spawn, so we can send more frequent
		// position updates without the "teleport" look. This tightens position
		// sync and lets the lerp do the visual smoothing.
		// [v84] Threshold history: 100->30->10->60px across versions.
		// [v90] Movement is now a BARE 0x11 (see loop below). No object is
		// ever torn down on move, so there is no strobe. The previous 60px
		// threshold caused visible teleport jumps because the DLL coordinate
		// probe was not reliably locked (lerp could not smooth between
		// updates). Drop the threshold to 2px so the client refreshes the
		// remote position on essentially every frame - continuous, no jump.
		const float MOVE_THRESHOLD = 2.0f;
		bool do_update = false;
		{
			std::lock_guard<std::mutex> lk(g_playersMtx);
			auto it = g_players.find(t_sid);
			if (it == g_players.end()) return;
			if (it->second.has_last_move) {
				float ddx = nx - it->second.last_move_x;
				float ddy = ny - it->second.last_move_y;
				mv_dir = (ddx < 0.0f) ? 0 : 1;   // moving left -> face left
				if (ddx * ddx + ddy * ddy >= MOVE_THRESHOLD * MOVE_THRESHOLD)
					do_update = true;
			} else {
				do_update = true; // first movement in this session
				mv_dir = 0;
			}
		}
		if (!do_update) {
			printf("[MP-FWD]   skip (under %.0fpx threshold)\n", MOVE_THRESHOLD);
			return;
		}
	for (auto &t : targets) {
		// [v90] MOVEMENT = bare 0x11 update, NO despawn. The DLL's
		// skip-inject path swallows this 0x11 for an already-rendered remote
		// oid: GetCharacterByOID finds the live CCharacter (the object is never
		// deleted on move), so the packet is NOT injected into the client - the
		// DLL lerp-smooths the existing object in memory instead. This removes
		// the per-step 0x12-remove + 0x3D-account-data churn that caused the
		// strobing/teleport flicker. The full 0x12+0x3D+0x11 respawn stays in
		// the ChangeMap cross-visibility loop, used only when the object does
		// not yet exist.
		CharacterSpawnPacket(me, nx, ny, mv_dir, t.sid, MP_RemoteCtx());
		printf("[MP-FWD]   -> sid=%d move-update v90 ctx=%d oid=%08X to (%.1f,%.1f)\n",
			t.sid, MP_RemoteCtx() ? 1 : 0, (unsigned)me.id, nx, ny);
	}
	MP_MARK("MP-FWD move=bare-0x11-update v90");
		{
			std::lock_guard<std::mutex> lk(g_playersMtx);
			auto it = g_players.find(t_sid);
			if (it != g_players.end()) {
				it->second.last_move_x = nx;
				it->second.last_move_y = ny;
				it->second.has_last_move = true;
			}
		}
		return;
	}

}
#else
void MP_ForwardToSameMap(const BYTE *, DWORD) {}
#endif

#ifndef MP_SERVER
// [MP] dll 侧无其它连接, 跨连接广播为空操作(符号需存在供链接)
void MP_BroadcastToSid(int sid, ServerPacket &sp, bool context) {}
// [v61] dll 侧不做跨玩家分发, 返回值无意义, 仅为链接符号
bool MP_RemoteCtx() { return false; }
bool MP_RemoteSend3D() { return false; }
// [v62] 同上; selffirst 必须为 false, 否则 dll 单机路径会重复发自我出生包
bool MP_Restore3D() { return false; }
bool MP_SelfSpawnFirst() { return false; }
bool MP_MoveAsSpawn() { return false; }
// [v110] The dll side never relays peer movement; default encoding is fine.
// Present only so the linker resolves the symbol.
int MP_PathMode() { return 0; }
#endif

// go to map
void ChangeMap(TenviCharacter &chr, WORD map_id, float x, float y) {
	MP_MARK("ChangeMap entry");
	ChangeMapPacket(map_id, x, y);
	MP_MARK("ChangeMap after ChangeMapPacket");

	switch (map_id) {
	case MAPID_ITEM_SHOP:
	case MAPID_EVENT:
	case MAPID_PARK:
	{
		// do not change last map id
		chr.SetMapReturn(chr.map);
		break;
	}
	default:
	{
		chr.SetMapReturn(chr.map);
		chr.map = map_id;
		break;
	}
	}
	chr.x = x;
	chr.y = y;
#ifdef MP_SERVER
	// [MP] 换图后保存地图与坐标, 关游戏不丢进度
	if (!TA.GetAccount().empty()) {
		db().updateCharMap(TA.GetAccount(), chr.id, chr.map, chr.x, chr.y);
	}
#endif
	MP_MARK("ChangeMap before SpawnObjects");
	SpawnObjects(chr, map_id);
	MP_MARK("ChangeMap after SpawnObjects");

#ifdef MP_SERVER
	// [MP] 静态互见: 刷新自己进在线表, 并与同图其他人互刷
	// 注意: 必须在自我出生包之前把同图其他人的出生包发下去,
	// 否则客户端收到自我出生包后可能认为场地初始化完毕, 从而忽略后来的远程玩家。
	{
		std::lock_guard<std::mutex> lk(g_playersMtx);
		RemotePlayer &me = g_players[t_sid];
		me.sid = t_sid;
		me.account = TA.GetAccount();
		me.char_id = chr.id;
		me.chr = chr;
		me.map = chr.map;
		me.x = x; me.y = y;
		me.last_move_x = x; me.last_move_y = y;
		me.has_last_move = false;
	}
	MP_MARK("ChangeMap after players-table insert");
	// [v62] SELF SPAWN FIRST. The CN client latches "who am I" onto the first
	// 0x11 it receives. v61b field evidence: the joining client got the remote
	// spawn (oid=0x54C) before its own (oid=0x57C), locked onto the wrong
	// character and drove a ghost - could not move, could not see anyone. The
	// already-stable player, whose own 0x11 arrived at login long before, DID
	// render the newcomer. So the ordering is the whole difference.
	bool self_spawned = false;
	if (MP_SelfSpawnFirst()) {
		CharacterSpawnPacket(chr, x, y);
		self_spawned = true;
		MP_MARK("ChangeMap self spawn (v62 early, before xvis loop)");
	}
	// [v94] Collect dirB (后进者看先进者) targets inside the lock, then send
	// the deferred spawns OUTSIDE the lock so the Sleep no longer holds
	// g_playersMtx (which froze other players' movement sync while blocked).
	std::vector<RemotePlayer> dirB_targets;
	{
		std::lock_guard<std::mutex> lk(g_playersMtx);
		// [v61-diag] prove whether this loop actually matches anybody.
		printf("[MP-XVIS] enter t_sid=%d my_map=%d table_size=%d\n",
			t_sid, (int)chr.map, (int)g_players.size());
		fflush(stdout);
		int xvis_matched = 0;
		for (auto &kv : g_players) {
			int other_sid = kv.first;
			RemotePlayer &other = kv.second;
			printf("[MP-XVIS]   cand sid=%d map=%d self=%d\n",
				other_sid, (int)other.map, (other_sid == t_sid) ? 1 : 0);
			fflush(stdout);
			if (other_sid == t_sid) continue;
			if (other.map != chr.map) continue;
			xvis_matched++;
			// [v54b] 0x3D must reach the receiving client BEFORE the 0x11 spawn:
			// the CN client creates the character object from 0x3D and ignores
			// any spawn whose oid is not a known object. v50-v54 sent only the
			// spawn -> remote players were silently dropped.
			// [v54i] "自己看到别人"重发场景**移到 self spawn 之后**(原 v54h 放在
			// 这里时,与 t_client 自己的 ChangeMap 流程冲突→崩溃). 改为在 ChangeMap
			// 函数末尾 self spawn 之后发,此时 t_client 自己的 ChangeMap 已完成.
			// [v54c] "别人看到自己": 给 other_sid 重发场景
			// [v60] REMOVED: ChangeMapPacket/SpawnObjects/AccountData/Spawn for other_sid.
			// Re-opening the ChangeMap window (0x10) on an ALREADY-STABLE client wipes its
			// field state -- the first player lost even his OWN character the moment a second
			// player joined (v57r field report). v54p law: ChangeMap runs ONCE at login only.
			// Now we only push a plain CField dynamic spawn (0x3D + 0x11) to the existing
			// player; the client byte-patch at 0x0048DD03 already allows rendering non-local
			// characters, so no window re-open is required.
			// [v61] 0x3D is opt-in now (mp_ctx.cfg char 2). 0x11 alone carries
			// id/pos/job/level/name/appearance/equipment, and at ctx=1 a remote
			// 0x3D would overwrite the receiver's own character identity.
			// --- 方向A: 让已在场的 other_sid 看到我 ---
			// 0x3D 先建对象(否则 0x11 在 0x0048DCEF 处查不到 oid 被丢弃),
			// 0x11 to render, then immediately restore other_sid's own account
			// context, or it gets overwritten by my account data and the viewer
			// can see people but cannot move itself (v61b field report).
			// [v82] 0x11 now uses the same layer as 0x3D (MP_RemoteCtx) so the
			// object is created and rendered on the same client layer.
			if (MP_RemoteSend3D()) AccountDataPacket(chr, other_sid);
			CharacterSpawnPacket(chr, x, y, 0, other_sid, MP_RemoteCtx()); // other sees me
			if (MP_RemoteSend3D() && MP_Restore3D())
				AccountDataPacket(other.chr, other_sid);         // [v62] restore other identity
			// [v94] collect for dirB (deferred, sent OUTSIDE the lock below)
			dirB_targets.push_back(other);
		}
		printf("[MP-XVIS] leave t_sid=%d matched=%d\n", t_sid, xvis_matched);
		fflush(stdout);
	}
	MP_MARK("ChangeMap after broadcast loop (v62-order-restore)");
	// [v62] 只有在 selffirst 关闭(回退对照)时才在这里补发自我出生包,
	// 否则会发两次 0x11, 客户端会把自己重建一遍.
	if (!self_spawned) CharacterSpawnPacket(chr, x, y);
	// [v94] dirB deferred: 让后进者(t_sid)看到已在场的 other.
	// Sleep 移到锁外; 延迟 3.0s 等 CN 客户端 ChangeMap 流程彻底关闭
	// (经典非对称: A 看得到 B, B 看不到 A, 因 B 的出生包在其自身
	// ChangeMap 期间到达被静默丢弃). 0x3D/0x11 间留 50ms, 再补发一次
	// (retry) 兜底更慢才稳定的客户端. 放在 self spawn 之后, 后进者先
	// 看到自己, 不会被阻塞黑屏.
	for (auto &other : dirB_targets) {
		Sleep(3000);
		if (MP_RemoteSend3D()) AccountDataPacket(other.chr, t_sid);
		Sleep(50);
		CharacterSpawnPacket(other.chr, other.x, other.y, 0, t_sid, MP_RemoteCtx());
		if (MP_RemoteSend3D() && MP_Restore3D())
			AccountDataPacket(chr, t_sid); // [v62] restore my identity
		// retry once: 1.5s 后再补一发, 覆盖更慢稳定的客户端
		Sleep(1500);
		if (MP_RemoteSend3D()) AccountDataPacket(other.chr, t_sid);
		Sleep(50);
		CharacterSpawnPacket(other.chr, other.x, other.y, 0, t_sid, MP_RemoteCtx());
		if (MP_RemoteSend3D() && MP_Restore3D())
			AccountDataPacket(chr, t_sid);
		printf("[MP-XVIS] deferred dirB v94 ctx=%d -> sid=%d other oid=%08X (retry x2)\n",
			MP_RemoteCtx() ? 1 : 0, t_sid, (unsigned)other.chr.id);
		fflush(stdout);
	}
#else
	CharacterSpawnPacket(chr, x, y);
#endif
	MP_MARK("ChangeMap after self CharacterSpawnPacket");
	// [v54p] 撤销 v54l/m/o 所有延迟重发场景方案 -- 客户端 ChangeMap 流程只能在
	// 登录时执行一次, 稳定后再触发无论如何都会崩/卡(无论延迟多久/补多少包).
	// 接受 v54c 单边互见(先进看后进), 后进者看先进者留作里程碑 3 优化.
	MP_MARK("ChangeMap exit");
}

// enter map by login or something
void SetMap(TenviCharacter &chr, WORD map_id) {
	MP_MARK("SetMap before get_map/FindSpawnPoint");
	TenviSpawnPoint spawn_point = tenvi_data.get_map(map_id)->FindSpawnPoint(0);
	MP_MARK("SetMap after FindSpawnPoint");
	float sx = (float)spawn_point.x;
	float sy = (float)spawn_point.y;
#ifdef MP_SERVER
	// [MP] Every player enters via spawn point 0, so two characters would land on
	// EXACTLY the same pixel and visually stack into what looks like one character.
	// Spread them horizontally by session id so both are clearly distinguishable.
	sx += (float)(((t_sid % 7) - 3) * 65);
	printf("[TenviServer] MARK SetMap sid=%d map=%d pos=(%.0f,%.0f)\n",
		t_sid, (int)map_id, sx, sy);
#endif
	ChangeMap(chr, map_id, sx, sy);
	MP_MARK("SetMap after ChangeMap");
}

// enter map by portal
void UsePortal(TenviCharacter &chr, DWORD portal_id) {
	TenviPortal portal = tenvi_data.get_map(chr.map)->FindPortal(portal_id); // current map
	TenviPortal next_portal = tenvi_data.get_map(portal.next_mapid)->FindPortal(portal.next_id); // next map

	ChangeMap(chr, portal.next_mapid, next_portal.x, next_portal.y);
}

void ItemShop(TenviCharacter &chr, bool bEnter) {
	if (bEnter) {
		SetMap(chr, MAPID_ITEM_SHOP);
	}
	else {
		SetMap(chr, chr.map_return);
	}
}

void Park(TenviCharacter &chr, bool bEnter) {
	if (bEnter) {
		SetMap(chr, MAPID_PARK);
	}
	else {
		SetMap(chr, chr.map_return);
	}
}

void Event(TenviCharacter &chr, bool bEnter) {
	if (bEnter) {
		SetMap(chr, MAPID_EVENT);
	}
	else {
		SetMap(chr, chr.map_return);
	}
}

// ========== TENVI Server Main ==========
bool FakeServer(ClientPacket &cp) {
	CLIENT_PACKET header = cp.DecodeHeader();

	switch (header) {
	// Select Character
	case CP_GAME_START: {
		DWORD character_id = cp.Decode4();
		BYTE channel = cp.Decode1();

#ifdef MP_SERVER
		// [MP] 从 DB 载入本账号角色(含保存的地图/等级), 再选角
		TA.ReloadFromDB();
#endif

		TA.Login(character_id);
		MP_MARK("GAME_START after Login");

#ifdef MP_SERVER
		// [v54j] 防重复登录: 同一角色 id 已在 g_players 在线表里 → 拒绝
		{
			std::lock_guard<std::mutex> lk(g_playersMtx);
			for (auto &kv : g_players) {
				if (kv.second.char_id == character_id) {
					printf("[TenviServer] DUP-LOGIN char_id=%u already online (sid=%d), reject\n",
						character_id, kv.first);
					return false;
				}
			}
		}
#endif

		for (auto &chr : TA.GetCharacters()) {
			if (chr.id == character_id) {
				chr.x = 0.0;
				chr.y = 0.0;
				GetGameServerPacket(); // notify game server ip
				ConnectedPacket(); // connected
				AccountDataPacket(chr);
				PlayerStatPacket(chr);
				PlayerSPPacket(chr);
				PlayerAPPacket(chr);
				MP_MARK("GAME_START before InitSkill");
				InitSkillPacket(chr);
				MP_MARK("GAME_START before SetMap");

				SetMap(chr, 8003);   // [v54d] 强制出生在魔法密林, 避免 db 残留地图导致双开不同图
				MP_MARK("GAME_START after SetMap");
				BoardPacket(Board_Spawn, L"Riremito", L"Tenvi JP v127");
				BoardPacket(Board_AddInfo, L"Riremito", L"Tenvi JP v127");
				MP_MARK("GAME_START done");
				return true;
			}
		}
		return false;
	}
	// Create New Character
	case CP_CREATE_CHARACTER: {
		std::wstring character_name = cp.DecodeWStr1();
		BYTE job_mask = cp.Decode1(); // 0x11 to 0x24
		WORD job_id = cp.Decode2(); // 4,5,6
		WORD character_skin = cp.Decode2(); // 1,2,3
		WORD character_hair = cp.Decode2();
		WORD character_face = cp.Decode2();
		WORD character_cloth = cp.Decode2();
		WORD guardian_color = cp.Decode2();

		WORD guardian_head = cp.Decode2();
		WORD guardian_body = cp.Decode2();
		WORD guardian_weapon = cp.Decode2();

		std::vector<WORD> guardian_equip;
		guardian_equip.push_back(guardian_head);
		guardian_equip.push_back(guardian_body);
		guardian_equip.push_back(guardian_weapon);

		TA.AddCharacter(character_name, job_mask, job_id, character_skin, character_hair, character_face, character_cloth, guardian_color, guardian_equip);
#ifdef MP_SERVER
		// [MP] 新角色写库, 下次登录仍在
		if (!TA.GetAccount().empty()) {
			db().insertChar(TA.GetAccount(), TA.GetCharacters().back());
		}
#endif
		WorldSelectPacket();   // 跳到世界选择(跟点"返回"一样)
		WorldListPacket();      // 列出世界(用户点世界,客户端自动 CP_WORLDLIST,服务端发 CharacterListPacket_Test)
		CharacterListPacket_Test(); // 也发角色列表(客户端缓存,选完世界直接显示)
		return true;
	}
	// Delete Character
	case CP_DELETE_CHARACTER: {
		DWORD character_id = cp.Decode4();
		// [FIX] 真正删除角色: 从内存列表移除 + 写回DB + 通知客户端
		auto &chars = TA.GetCharacters();
		for (auto it = chars.begin(); it != chars.end(); ++it) {
			if (it->id == character_id) {
#ifdef MP_SERVER
				if (!TA.GetAccount().empty()) {
					db().deleteChar(TA.GetAccount(), character_id);
				}
#endif
				chars.erase(it);
				break;
			}
		}
		DeleteCharacter();           // 0x07 删除成功通知
		WorldSelectPacket();         // 跳到世界选择
		WorldListPacket();           // 列出世界
		CharacterListPacket_Test();  // 缓存角色列表
		return true;
	}
	// Character Select to World Select
	case CP_BACK_TO_LOGIN_SERVER: {
		WorldSelectPacket(); // back to login server
		WorldListPacket(); // show world list
		return true;
	}
	// Game Server to Login Server
	case CP_LOGOUT: {
		GetLoginServerPacket();// notify login server ip
		ConnectedPacket(); // connected
		CharacterListPacket_Test();
		return true;
	}
	case CP_GUARDIAN_RIDE: {
		cp.Decode1(); // on off
		return true;
	}
	case CP_USE_AP: {
		TenviCharacter &chr = TA.GetOnline();
		BYTE stat = cp.Decode1();
		chr.UseAP(stat);
		PlayerAPPacket(chr);
		PlayerStatPacket(chr);
		return true;
	}
	case CP_GUARDIAN_SUMMON: {
		BYTE flag = cp.Decode1(); // on off
		GuardianSummonPacket(TA.GetOnline(), flag ? true : false);
		return true;
	}
	case CP_EMOTION: {
		TenviCharacter &chr = TA.GetOnline();
		BYTE emotion = cp.Decode1();
		EmotionPacket(chr, emotion);
		return true;
	}
	case CP_UPDATE_PROFILE: {
		std::wstring wText = cp.DecodeWStr1();
		return true;
	}
	case CP_WORLD_MAP_OPEN:
	{
		BYTE area_code = cp.Decode1();
		//WorldMapUpdatePacket(area_code);
		WorldMapUpdatePacketTest(area_code);
		return true;
	}
	case CP_ITEM_SHOP: {
		BYTE flag = cp.Decode1();
		ItemShop(TA.GetOnline(), flag ? true : false);
		return true;
	}
	case CP_HIT: {
		TenviCharacter &chr = TA.GetOnline();

		DWORD hit_from = cp.Decode4();
		DWORD hit_to = cp.Decode4();

		if (chr.id != hit_to) {
			HitPacket(hit_from, hit_to);
			RemoveObjectPacket(hit_to);
			return true;
		}

		if (hit_to == chr.id) {
			PlayerHitPacket(chr);
			return true;
		}

		return true;
	}
	case CP_USE_SP: {
		TenviCharacter &chr = TA.GetOnline();
		WORD skill_id = cp.Decode2();
		chr.UseSP(skill_id);
		UpdateSkillPacket(chr, skill_id);
		PlayerSPPacket(chr);
		return true;
	}
	case CP_USE_PORTAL: {
		TenviCharacter &chr = TA.GetOnline();
		DWORD portal_id = cp.Decode4();
		// cp.DecodeWStr1();
		UsePortal(chr, portal_id);
		return true;
	}
	case CP_PLAYER_REVIVE: {
		//cp.Decode1();
		PlayerRevivePacket(TA.GetOnline());
		return true;
	}
	case CP_CHANGE_CHANNEL: {
		BYTE channel = cp.Decode1();
		return true;
	}
	case CP_NPC_TALK: {
		DWORD object_id = cp.Decode4();
		DWORD unk2 = cp.Decode4();
		DWORD npc_type = cp.Decode4();
		DWORD unk3 = cp.Decode4();
#ifdef MP_SERVER
		printf("[NPC-TALK] object=%u unk2=%u npc_type=%u unk3=%u\n",
			(unsigned)object_id, (unsigned)unk2, (unsigned)npc_type, (unsigned)unk3);
		fflush(stdout);
#endif
		// Send NPC dialogue response (CN v126 has full .tv data now)
		DWORD talk_id = (npc_type != 0) ? npc_type : object_id;
		NPCTalkPacket(talk_id, L"Hello! NPC obj=" + std::to_wstring(object_id));
		return true;
	}
	case CP_PLAYER_CHAT: {
		BYTE type = cp.Decode1(); // 0 = map chat
		cp.Decode1(); // 1
		cp.Decode1(); // 0
		std::wstring message = cp.DecodeWStr1();

		// 聊天命令
		// /mall - 打开商城(测试)
		if (message == L"/mall") {
			TenviCharacter &chr = TA.GetOnline();
			// 发送商城物品列表
			ServerPacket mall(SP_ITEM_SHOP);
			mall.Encode4(chr.id); // character id
			mall.Encode4(0);      // shop type = normal
			// 3个物品: 红药水 蓝药水 新手剑
			mall.Encode2(3); // item count
			// Item 1: 红药水 (id=2000, price=10 gold)
			mall.Encode4(2000); mall.Encode4(10); mall.Encode4(100); mall.Encode2(0);
			// Item 2: 蓝药水 (id=2001, price=10 gold)
			mall.Encode4(2001); mall.Encode4(10); mall.Encode4(100); mall.Encode2(0);
			// Item 3: 新手剑 (id=1302000, price=50 gold)
			mall.Encode4(1302000); mall.Encode4(50); mall.Encode4(1); mall.Encode2(0);
			SendPacket(mall);
		}
		// /heal - 满血满蓝
		if (message == L"/heal") {
			TenviCharacter &chr = TA.GetOnline();
			chr.stat_hp = 9999;
			chr.stat_mp = 9999;
			PlayerStatPacket(chr);
		}
		// /map <id> - 传送
		if (_wcsnicmp(message.c_str(), L"/map ", 5) == 0) {
			int map_id = _wtoi(&message.c_str()[5]);
			SetMap(TA.GetOnline(), map_id);
		}

		// @ 命令 (含 @map/@npc)
		if (message.length() && message.at(0) == L'@') {
			if (_wcsnicmp(message.c_str(), L"@map ", 5) == 0) {
				int map_id = _wtoi(&message.c_str()[5]);
				SetMap(TA.GetOnline(), map_id);
			}
			if (_wcsnicmp(message.c_str(), L"@npc ", 5) == 0) {
				DWORD npc_id = (DWORD)_wtoi(&message.c_str()[5]);
				NPCTalkPacket(npc_id, L"[Tenvi MP] NPC ID=" + std::to_wstring(npc_id) + L" - chat command test");
			}
			return true;
		}

		return true;
	}
	case CP_PARK: {
		BYTE flag = cp.Decode1();
		Park(TA.GetOnline(), flag ? true : false);
		return true;
	}
	case CP_PARK_BATTLE_FIELD: // ???
	case CP_EVENT: {
		BYTE flag = cp.Decode1();
		Event(TA.GetOnline(), flag ? true : false);
		return true;
	}
	case CP_TIME_GET_TIME: {
		cp.Decode1(); // 0
		cp.Decode4(); // time
		return true;
	}
	default:
	{
		break;
	}
	}

	return false;
}
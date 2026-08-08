#include"FakeServer.h"
#include"AutoResponse.h"
#include"TemporaryData.h"
#include <mutex>
#include <map>

#ifdef MP_SERVER
#include "../StandaloneServer/db.h"
#include <cstdio>
// [DIAG] 崩溃定位标记: 直接写 stdout(服务端无缓冲, 落到 server_live.log)
#define MP_MARK(msg) do { printf("[TenviServer] MARK %s\n", msg); } while(0)
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
void CharacterSpawnPacket(TenviCharacter &chr, float x = 0, float y = 0, int target_sid = -1, bool context = true) {
	ServerPacket sp(SP_CHARACTER_SPAWN);
	sp.Encode4(chr.id); // 0048DB9B id, where checks id?
	sp.EncodeFloat(x); // 0048DBA5, coordinate x
	sp.EncodeFloat(y); // 0048DBAF, corrdinate y
	sp.Encode1(0); // 0048DBB9, direction 0 = left, 1 = right
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
	else MP_BroadcastToSid(target_sid, sp, false);
}

// 0x12
// [v57] 0x12 移除随 0x11 一起退回 CField(ctx=0): 移除层必须与渲染层一致,
// 否则下线后对方仍显示. 远程 0x11 已退回 CField, 移除也必须同步.
void RemoveObjectPacket(DWORD object_id, int target_sid = -1) {
	ServerPacket sp(SP_REMOVE_OBJECT);
	sp.Encode4(object_id); // not only for character
	if (target_sid < 0) SendPacket(sp);
	else MP_BroadcastToSid(target_sid, sp, false);
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
	else MP_BroadcastToSid(target_sid, sp, false);
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
// [v57] 移动同步: 0x0C 原样广播给同图其他玩家. 远程对象已退回 CField(ctx=0),
// 移动包必须同层(ctx=0)注入, 对方角色才能在 CField 里动起来. 包体原样转发(含 opcode).
void MP_ForwardToSameMap(const BYTE *pkt, DWORD len) {
	WORD my_map = 0;
	{
		std::lock_guard<std::mutex> lk(g_playersMtx);
		auto it = g_players.find(t_sid);
		if (it == g_players.end()) {
			printf("[MP-FWD] sid=%d not in players table, skip\n", t_sid);
			return;
		}
		my_map = it->second.map;
	}
	std::vector<int> targets;
	{
		std::lock_guard<std::mutex> lk(g_playersMtx);
		for (auto &kv : g_players) {
			if (kv.first == t_sid) continue;
			if (kv.second.map == my_map) targets.push_back(kv.first);
		}
	}
	BYTE op = (len > 0) ? pkt[0] : 0;
	DWORD oid = 0;
	if (len >= 5) oid = *(DWORD *)(pkt + 1);
	printf("[MP-FWD] sid=%d map=%d targets=%zu op=%02X oid=%08X\n",
		(int)t_sid, (int)my_map, targets.size(), (unsigned)op, (unsigned)oid);
	if (targets.empty()) return;
	ServerPacket sp;
	sp.Raw(pkt, len);
	for (int sid : targets) {
		printf("[MP-FWD]   -> sid=%d\n", sid);
		MP_BroadcastToSid(sid, sp, false);
	}
}
#else
void MP_ForwardToSameMap(const BYTE *, DWORD) {}
#endif

#ifndef MP_SERVER
// [MP] dll 侧无其它连接, 跨连接广播为空操作(符号需存在供链接)
void MP_BroadcastToSid(int sid, ServerPacket &sp, bool context) {}
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
	}
	MP_MARK("ChangeMap after players-table insert");
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
			AccountDataPacket(chr, other_sid);                   // 别人看到自己: 先给自己建角色对象
			CharacterSpawnPacket(chr, x, y, other_sid);
			// [v56] 后进看先进尝试: 给当前玩家也发对方的 0x3D + 0x11,
			// 让后进者在自己的 ChangeMap 窗口里创建先进者对象.
			AccountDataPacket(other.chr, t_sid);
			CharacterSpawnPacket(other.chr, other.x, other.y, t_sid);
		}
		printf("[MP-XVIS] leave t_sid=%d matched=%d\n", t_sid, xvis_matched);
		fflush(stdout);
	}
	MP_MARK("ChangeMap after broadcast loop (v61-diag instrumented)");
#endif
	CharacterSpawnPacket(chr, x, y);
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
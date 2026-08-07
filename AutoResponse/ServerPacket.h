#ifndef __SERVERPACKET_H__
#define __SERVERPACKET_H__

// Server to Client (Recv, In)

#include<Windows.h>
#include<string>
#include<vector>

enum SERVER_PACKET {
	SP_BEGIN,
	SP_VERSION,
	SP_CRASH,
	SP_LOGIN_FAILED,
	SP_CHARACTER_SELECT,
	SP_CHARACTER_LIST,
	SP_DELETE_CHARACTER_MSG,
	SP_WORLD_SELECT,
	SP_WORLD_LIST,
	SP_CS_INVITED,
	SP_CS_KOREAN_MSG,
	SP_GET_GAME_SERVER,
	SP_GET_LOGIN_SERVER,
	SP_CONNECTED,
	SP_MAP_RESET,
	SP_MAP_CHANGE,
	SP_CHARACTER_SPAWN,
	SP_REMOVE_OBJECT,
	SP_CREATE_OBJECT,
	SP_ACTIVATE_OBJECT,
	SP_NPC_TALK,
	SP_HIT,
	SP_SHOW_OBJECT,
	SP_IN_MAP_TELEPORT,
	SP_ACCOUNT_DATA,
	SP_PLAYER_HIT,
	SP_PLAYER_LEVEL_UP,
	SP_PLAYER_STAT_EXP,
	SP_PLAYER_STAT_SP,
	SP_PLAYER_STAT_AP,
	SP_PLAYER_STAT_ALL,
	SP_GUARDIAN_SUMMON,
	SP_EMOTION,
	SP_WORLD_MAP_UPDATE,
	SP_PLAYER_REVIVE,
	SP_ITEM_SHOP_ERROR,
	SP_ITEM_SHOP,
	SP_UPDATE_SKILL,
	SP_PLAYER_SKILL_ALL,
	SP_FRIEND_REQUEST,
	SP_GM_MSG,
	SP_GUILD_RANK_DOWN,
	SP_BOARD,
	SP_UNKNOWN,
	SP_END,
};

class ServerPacket {
private:
	static BYTE opcode[];

public:
	static BYTE* GetOpcode();

private:
	std::vector<BYTE> packet;

public:
	ServerPacket(SERVER_PACKET header);
	ServerPacket() {}   // [v55] 空构造, 配合 Raw() 转发原始包

	std::vector<BYTE>& get();
	// [v55] 直接塞原始包体(不解析). 内联实现避免增量构建缓存旧 obj 导致链接失败
	inline void Raw(const BYTE *data, DWORD len) { packet.assign(data, data + len); }
	void Encode1(BYTE val);
	void Encode2(WORD val);
	void Encode4(DWORD val);
	void EncodeWStr1(std::wstring val);
	void EncodeWStr2(std::wstring val);
	void Encode8(ULONGLONG val);
	void EncodeFloat(float val);
};

void SetServerPacketHeader_JP_v127();
void SetServerPacketHeader_CN_v126();
void SetServerPacketHeader_HK_v102();
void SetServerPacketHeader_KRX_v107();
void SetServerPacketHeader_KR_v200();

#endif
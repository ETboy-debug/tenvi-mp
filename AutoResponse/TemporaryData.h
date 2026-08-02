#ifndef __TEMPORARYDATA_H__
#define __TEMPORARYDATA_H__

#include<Windows.h>
#include<string>
#include<vector>

typedef struct{
	WORD id;
	BYTE level;
} TenviSkill;

enum TenviStat {
	TS_STR,
	TS_DEX,
	TS_HP,
	TS_INT,
	TS_MP,
};

class TenviCharacter {
private:
	static DWORD id_counter;

public:
	DWORD id;
	std::wstring name;
	BYTE job_mask;
	WORD job;
	WORD skin;
	WORD hair;
	WORD face;
	WORD cloth;
	WORD gcolor; // guadian
	WORD map;
	WORD map_return;
	BYTE level;
	WORD sp; // skill point
	WORD ap;
	// stat
	WORD stat_str; // 椡
	WORD stat_dex; // 晀彿
	WORD stat_hp; // 懱椡
	WORD stat_int; // 抦擻
	WORD stat_mp; //抦宐
	// data
	float x;
	float y;
	int gold; // [MP] GM 可改, 存档持久化

	std::vector<WORD> equipped;
	std::vector<WORD> gequipped;
	std::vector<TenviSkill> skill;

	TenviCharacter(std::wstring nName, BYTE nJob_Mask, WORD nJob, WORD nSkin, WORD nHair, WORD nFace, WORD nCloth, WORD nGColor, std::vector<WORD> &nGEquipped);

	void TestSilva();
	bool UseSP(WORD skill_id);
	bool UseAP(BYTE stat_id);
	void SetMapReturn(WORD map_return_id);

	// [MP] 加载 DB 后把 id 发号器抬到最大已用 id 之上, 避免新创角色撞 id
	static void ReserveId(DWORD maxId);
};

class TenviAccount {
private:
	std::vector<TenviCharacter> characters;
	DWORD online_id;

public:
	BYTE slot;
	std::wstring account; // [MP] 当前连接的账号名(由客户端 ini 上报)

	TenviAccount();
	bool FindCharacter(DWORD id, TenviCharacter *found);
	std::vector<TenviCharacter>& GetCharacters();
	bool AddCharacter(std::wstring nName, BYTE nJob_Mask, WORD nJob, WORD nSkin, WORD nHair, WORD nFace, WORD nCloth, WORD nGColor, std::vector<WORD> &nGEquipped);
	bool Login(DWORD id);
	TenviCharacter& GetOnline();

	// [MP] 仅服务端使用
	void SetAccount(const std::wstring &a) { account = a; }
	const std::wstring &GetAccount() { return account; }
#ifdef MP_SERVER
	void ReloadFromDB(); // 从 DB 载入本账号角色(空则自动建默认角色)
#endif
};

#endif
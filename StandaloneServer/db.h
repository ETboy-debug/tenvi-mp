#ifndef __TENVI_DB_H__
#define __TENVI_DB_H__
//
// db.h - 冲锋岛服务端 文件式数据库（零外部依赖）
//
// 数据以纯文本行存于 <服务端目录>/tenvi.db:
//   A <账号>
//   C <账号> <id> <名称> <job_mask> <job> <skin> <hair> <face> <cloth> <gcolor> <level> <map> <gold> <x> <y>
// 角色名称按 UTF-8 存储（避免 GBK 歧义）。名称约定不含空格。
//
// 这是存档的唯一真相来源：登录/选角/列表都从 DB 读，创角/换图/GM 写回 DB。
// 仅在服务端(MP_SERVER)编译使用。
//
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include "TemporaryData.h"

struct DBCharRow {
	DWORD id = 0;
	std::wstring name;
	BYTE job_mask = 0;
	WORD job = 0, skin = 0, hair = 0, face = 0, cloth = 0, gcolor = 0;
	BYTE level = 1;
	WORD map = 8003;
	int gold = 0;
	float x = 0, y = 0;
};

inline std::string WToUtf8(const std::wstring &s) {
	std::string out;
	if (s.empty()) return out;
	int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
	if (n <= 0) return out;
	out.resize(n);
	WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, NULL, NULL);
	return out;
}
inline std::wstring Utf8ToW(const std::string &s) {
	std::wstring out;
	if (s.empty()) return out;
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
	if (n <= 0) return out;
	out.resize(n);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
	return out;
}

class TenviDB {
public:
	void open(const std::wstring &exeDir) {
		std::lock_guard<std::mutex> lk(m_);
		dir_ = exeDir;
		if (!dir_.empty() && dir_.back() != L'\\' && dir_.back() != L'/') dir_ += L'\\';
		path_ = dir_ + L"tenvi.db";
		load();
	}

	void upsertAccount(const std::wstring &acc) {
		std::lock_guard<std::mutex> lk(m_);
		accounts_[acc] = 1;
		save();
	}

	bool accountExists(const std::wstring &acc) {
		std::lock_guard<std::mutex> lk(m_);
		return accounts_.find(acc) != accounts_.end();
	}

	// 读取账号的角色；若账号无任何角色，自动建一个默认角色并存盘
	std::vector<TenviCharacter> loadChars(const std::wstring &acc) {
		std::lock_guard<std::mutex> lk(m_);
		std::vector<TenviCharacter> out;
		auto it = chars_.find(acc);
		if (it == chars_.end() || it->second.empty()) {
			std::vector<WORD> emptyG;
			TenviCharacter hero(acc, (BYTE)((1 << 4) | 4), 6, 3, 19, 24, 479, 157, emptyG);
			hero.TestSilva();
			hero.level = 1;
			hero.id = nextId();
			chars_[acc].push_back(toRow(hero));
			accounts_[acc] = 1;
			save();
			out.push_back(hero);
			return out;
		}
		for (auto &row : it->second) {
			out.push_back(fromRow(row));
		}
		return out;
	}

	void insertChar(const std::wstring &acc, const TenviCharacter &chr) {
		std::lock_guard<std::mutex> lk(m_);
		DBCharRow row = toRow(chr);
		auto &v = chars_[acc];
		bool found = false;
		for (auto &r : v) { if (r.id == row.id) { r = row; found = true; break; } }
		if (!found) v.push_back(row);
		accounts_[acc] = 1;
		save();
	}

	void updateCharMap(const std::wstring &acc, DWORD id, WORD map, float x, float y) {
		std::lock_guard<std::mutex> lk(m_);
		auto it = chars_.find(acc);
		if (it == chars_.end()) return;
		for (auto &r : it->second) {
			if (r.id == id) { r.map = map; r.x = x; r.y = y; save(); return; }
		}
	}

	void updateCharStat(const std::wstring &acc, DWORD id, BYTE level, int gold) {
		std::lock_guard<std::mutex> lk(m_);
		auto it = chars_.find(acc);
		if (it == chars_.end()) return;
		for (auto &r : it->second) {
			if (r.id == id) { r.level = level; r.gold = gold; save(); return; }
		}
	}

	const std::map<std::wstring, int> &accounts() { return accounts_; }
	const std::map<std::wstring, std::vector<DBCharRow>> &chars() { return chars_; }

private:
	DBCharRow toRow(const TenviCharacter &c) {
		DBCharRow r;
		r.id = c.id; r.name = c.name; r.job_mask = c.job_mask;
		r.job = c.job; r.skin = c.skin; r.hair = c.hair; r.face = c.face;
		r.cloth = c.cloth; r.gcolor = c.gcolor; r.level = c.level;
		r.map = c.map; r.gold = c.gold; r.x = c.x; r.y = c.y;
		return r;
	}
	TenviCharacter fromRow(const DBCharRow &r) {
		std::vector<WORD> emptyG;
		TenviCharacter c(r.name, r.job_mask, r.job, r.skin, r.hair, r.face, r.cloth, r.gcolor, emptyG);
		c.TestSilva();
		c.id = r.id;
		c.level = r.level;
		c.map = r.map;
		c.gold = r.gold;
		c.x = r.x; c.y = r.y;
		c.map_return = r.map;
		return c;
	}
	DWORD nextId() {
		// 保证全局唯一且大于已加载的最大 id
		DWORD maxId = 1337;
		for (auto &kv : chars_) for (auto &r : kv.second) if (r.id > maxId) maxId = r.id;
		DWORD id = (DWORD)InterlockedIncrement((volatile LONG *)&idCounter_);
		if (id <= maxId) { idCounter_ = maxId + 1; id = (DWORD)InterlockedIncrement((volatile LONG *)&idCounter_); }
		return id;
	}
	void load() {
		accounts_.clear(); chars_.clear(); idCounter_ = 1337;
		FILE *f = NULL;
		_wfopen_s(&f, path_.c_str(), L"rb");
		if (!f) return;
		char lineBuf[4096];
		std::vector<char> big;
		while (fgets(lineBuf, sizeof(lineBuf), f)) {
			big.assign(lineBuf, lineBuf + strlen(lineBuf));
			// 处理超长行
			while (!feof(f) && big.size() >= 1 && big.back() != '\n') {
				char chunk[4096];
				if (!fgets(chunk, sizeof(chunk), f)) break;
				big.insert(big.end(), chunk, chunk + strlen(chunk));
			}
			std::string line(big.begin(), big.end());
			// 去掉换行
			while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
			if (line.empty() || line[0] == '#') continue;
			if (line[0] == 'A') {
				std::string name = trim(line.substr(1));
				if (!name.empty()) accounts_[Utf8ToW(name)] = 1;
			}
			else if (line[0] == 'C') {
				// 解析
				std::vector<std::string> tok = split(line, ' ');
				// tok[0]='C' tok[1]=acc tok[2]=id tok[3]=name tok[4..]=nums
				if (tok.size() >= 16) { // C acc id name + 12 个数值字段 = 16 段
					std::wstring acc = Utf8ToW(tok[1]);
					DBCharRow r;
					r.id = (DWORD)atol(tok[2].c_str());
					r.name = Utf8ToW(tok[3]);
					r.job_mask = (BYTE)atoi(tok[4].c_str());
					r.job = (WORD)atoi(tok[5].c_str());
					r.skin = (WORD)atoi(tok[6].c_str());
					r.hair = (WORD)atoi(tok[7].c_str());
					r.face = (WORD)atoi(tok[8].c_str());
					r.cloth = (WORD)atoi(tok[9].c_str());
					r.gcolor = (WORD)atoi(tok[10].c_str());
					r.level = (BYTE)atoi(tok[11].c_str());
					r.map = (WORD)atoi(tok[12].c_str());
					r.gold = atoi(tok[13].c_str());
					r.x = (float)atof(tok[14].c_str());
					r.y = (float)atof(tok[15].c_str());
					chars_[acc].push_back(r);
					if (r.id > idCounter_) idCounter_ = r.id;
				}
			}
		}
		fclose(f);
	}
	void save() {
		// 路径可能含中文(如 D:\冲锋岛\发布\服主端), 必须走宽字符 API,
		// 用 UTF-8 字节串喂 fopen 会被当 ANSI 解析 -> 打不开 -> 存档静默丢失
		std::wstring wtmp = path_ + L".tmp";
		FILE *f = NULL;
		if (_wfopen_s(&f, wtmp.c_str(), L"wb") != 0 || !f) return;
		fprintf(f, "# Tenvi standalone server database\n");
		for (auto &a : accounts_) {
			std::string an = WToUtf8(a.first);
			fprintf(f, "A %s\n", an.c_str());
		}
		for (auto &kv : chars_) {
			std::string an = WToUtf8(kv.first);
			for (auto &r : kv.second) {
				std::string nm = WToUtf8(r.name);
				fprintf(f, "C %s %u %s %u %u %u %u %u %u %u %u %u %d %f %f\n",
					an.c_str(), r.id, nm.c_str(),
					(unsigned)r.job_mask, (unsigned)r.job, (unsigned)r.skin,
					(unsigned)r.hair, (unsigned)r.face, (unsigned)r.cloth,
					(unsigned)r.gcolor, (unsigned)r.level, (unsigned)r.map,
					r.gold, r.x, r.y);
			}
		}
		fclose(f);
		// 原子替换
		DeleteFileW(path_.c_str());
		MoveFileW(wtmp.c_str(), path_.c_str());
	}
	static std::string trim(const std::string &s) {
		size_t a = 0, b = s.size();
		while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
		while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
		return s.substr(a, b - a);
	}
	static std::vector<std::string> split(const std::string &s, char d) {
		std::vector<std::string> out; std::string cur;
		for (char c : s) { if (c == d) { out.push_back(cur); cur.clear(); } else cur += c; }
		out.push_back(cur);
		return out;
	}

	std::wstring dir_;
	std::wstring path_;
	std::map<std::wstring, int> accounts_;
	std::map<std::wstring, std::vector<DBCharRow>> chars_;
	volatile LONG idCounter_ = 1337;
	std::mutex m_;
};

// 头文件内联单例
// 注意: 这里必须是 inline 而不是 static inline。
// static 会让每个 .cpp 各自持有一份独立的 s, 于是 main() 打开的库
// 和 FakeServer/TemporaryData 里读写的库不是同一个 -> 存档全丢。
inline TenviDB &db() {
	static TenviDB s;
	return s;
}

#endif

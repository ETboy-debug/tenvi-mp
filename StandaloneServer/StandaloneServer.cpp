// StandaloneServer.cpp - 冲锋岛(Tenvi) 独立服务端
//
// 架构说明（明文桥接）:
//   客户端原生网络栈带加解密, 而模拟器的 hook 点在明文层。
//   所以不去兼容客户端原生线格式, 而是由注入 DLL(AutoResponse) 另开一条 socket,
//   两端只跑纯明文 Tenvi 包, 帧格式由我们自己定义。
//
//   帧 = [4 字节小端 payload 长度][payload]
//   payload[0] = type: 0 = 游戏明文包, 1 = 控制命令
//
// 多玩家:
//   每个连接一个线程。会话状态 TA 声明为 thread_local(见 FakeServer.h),
//   所以各玩家的角色数据天然隔离, 不会串号。
//   注意: 本版本玩家之间"看不到彼此"(无状态广播), 那是下一阶段的事。
//
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <vector>
#include <string>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include "ClientPacket.h"
#include "ServerPacket.h"
#include "FakeServer.h"
#include "TenviData.h"
#include "../EmuMainTenvi/ConfigTenvi.h"
#include "db.h"
#include <map>
#include <set>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

#define MP_TYPE_GAME 0
#define MP_TYPE_CTRL 1
// [v50] Game packet the client must dispatch through CField (remote players,
// monsters, map objects) instead of CWvsContext. Must match MPClient.h.
#define MP_TYPE_GAME_FIELD 2

#define MP_CTRL_HELLO     1
#define MP_CTRL_WORLDLIST 2
#define MP_CTRL_CHARLIST  3
#define MP_CTRL_LOGIN     4   // [MP] 登录: payload[2..] = utf8 "账号\0密码"
#define MP_CTRL_REGISTER  5   // [MP] 注册: payload[2..] = utf8 "账号\0密码"
#define MP_CTRL_LOGIN_RESULT   6 // 服务端回: payload[2] = 1 成功 / 0 失败(密码错)
#define MP_CTRL_REGISTER_RESULT 7 // 服务端回: payload[2] = 1 成功 / 0 账号已存在

#define MP_MAX_PLAYERS 65535   // [MP] 人数上限(单机私服用, 对小伙伴联机等于无上限; 保留拒绝逻辑防连接风暴)

#define MP_ADMIN_PORT   8788 // [MP] 本地 GM 管理端口(仅 127.0.0.1)

// FakeServer.cpp 里定义的版本包（头文件未声明，这里补上）
void VersionPacket();

// ---- 区域配置：原本由注入 DLL 读 ini 决定，独立服务端固定国服 CN v126 ----
static Region g_region = TENVI_CN;
static wstring g_regionStr = L"CN";
static wstring g_xmlPath = L"tv_xml";

Region GetRegion() { return g_region; }
wstring GetRegionStr() { return g_regionStr; }
wstring GetXMLPath() { return g_xmlPath; }

// ---- 全局(进程级) ----
static int g_port = 8787;
static int g_dump = 8;                 // 每个会话前 N 个收发包打印十六进制
static volatile LONG g_online = 0;     // 当前在线人数
static volatile LONG g_sidSeq = 0;     // 会话编号发号器
static std::mutex g_logMutex;          // 多线程日志不交错

// [MP] GM 管理用: 在线会话表 + 踢人集合 + 互斥
static std::map<int, SOCKET> g_onlineSock;
static std::map<int, std::wstring> g_onlineAcc;
static std::set<int> g_kickSet;
static std::mutex g_adminMutex;

// ---- 会话级(线程局部): 每个玩家一份 ----
static thread_local SOCKET t_client = INVALID_SOCKET;
thread_local int t_sid = 0;
static thread_local int t_sendCount = 0;
static thread_local int t_recvCount = 0;

// 带会话号的日志, 整行加锁输出
static void Log(const char *fmt, ...) {
	char line[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(line, sizeof(line) - 1, fmt, ap);
	line[sizeof(line) - 1] = 0;
	va_end(ap);

	std::lock_guard<std::mutex> lock(g_logMutex);
	if (t_sid > 0) {
		printf("[TenviServer] [#%d] %s\n", t_sid, line);
	}
	else {
		printf("[TenviServer] %s\n", line);
	}
}

static void LogW(const char *prefix, const wstring &s) {
	char b[1024] = { 0 };
	WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, b, sizeof(b) - 1, NULL, NULL);
	Log("%s%s", prefix, b);
}

static void HexDump(const char *tag, const BYTE *p, size_t n) {
	char line[256];
	int off = _snprintf(line, sizeof(line) - 1, "%s (%u bytes):", tag, (unsigned)n);
	size_t show = n > 32 ? 32 : n;
	for (size_t i = 0; i < show && off < (int)sizeof(line) - 8; i++) {
		off += _snprintf(line + off, sizeof(line) - off - 1, " %02X", p[i]);
	}
	if (n > show) {
		_snprintf(line + off, sizeof(line) - off - 1, " ...");
	}
	Log("%s", line);
}

// 替代 AutoResponse 的注入式发送：编码后经本线程的 socket 发回对应客户端
// [v50] frameType selects the client-side dispatch layer:
//   MP_TYPE_GAME       -> CWvsContext (this client's own character)
//   MP_TYPE_GAME_FIELD -> CField      (remote players / monsters / objects)
static void SendPacketTyped(ServerPacket &sp, BYTE frameType) {
	if (t_client == INVALID_SOCKET) {
		return;
	}
	vector<BYTE> data = sp.get();
	DWORD len = (DWORD)data.size() + 1; // + type
	vector<BYTE> frame;
	frame.push_back((BYTE)(len & 0xFF));
	frame.push_back((BYTE)((len >> 8) & 0xFF));
	frame.push_back((BYTE)((len >> 16) & 0xFF));
	frame.push_back((BYTE)((len >> 24) & 0xFF));
	frame.push_back(frameType);
	frame.insert(frame.end(), data.begin(), data.end());

	if (t_sendCount < g_dump && !data.empty()) {
		HexDump("SEND", &data[0], data.size());
		t_sendCount++;
	}

	int r = send(t_client, (const char *)&frame[0], (int)frame.size(), 0);
	if (r == SOCKET_ERROR) {
		Log("send failed, err=%d", WSAGetLastError());
	}
}

void SendPacket(ServerPacket &sp) { SendPacketTyped(sp, MP_TYPE_GAME); }

// [v50] SendPacket2 used to be a plain alias of SendPacket, which silently
// discarded the CField routing intent. It now emits its own frame type.
void SendPacket2(ServerPacket &sp) { SendPacketTyped(sp, MP_TYPE_GAME_FIELD); }
void DelaySendPacket(ServerPacket &sp) { SendPacketTyped(sp, MP_TYPE_GAME); }

// [MP] 向本线程客户端回一条控制结果(登录成功/失败、注册结果等)
static void SendCtrlResult(BYTE cmd, const BYTE *data, DWORD dn) {
	if (t_client == INVALID_SOCKET) return;
	DWORD len = 1 + 1 + dn; // type + cmd + data
	vector<BYTE> frame;
	frame.push_back((BYTE)(len & 0xFF));
	frame.push_back((BYTE)((len >> 8) & 0xFF));
	frame.push_back((BYTE)((len >> 16) & 0xFF));
	frame.push_back((BYTE)((len >> 24) & 0xFF));
	frame.push_back(MP_TYPE_CTRL);
	frame.push_back(cmd);
	if (dn) frame.insert(frame.end(), data, data + dn);
	int r = send(t_client, (const char *)&frame[0], (int)frame.size(), 0);
	if (r == SOCKET_ERROR) Log("SendCtrlResult send failed, err=%d", WSAGetLastError());
}

// [MP] 向指定 socket 发游戏包(供 GM 广播用)
// [v50] frameType carries the client-side dispatch layer, see SendPacketTyped.
static void SendPacketTo(SOCKET s, ServerPacket &sp, BYTE frameType = MP_TYPE_GAME) {
	if (s == INVALID_SOCKET) return;
	vector<BYTE> data = sp.get();
	DWORD len = (DWORD)data.size() + 1;
	vector<BYTE> frame;
	frame.push_back((BYTE)(len & 0xFF));
	frame.push_back((BYTE)((len >> 8) & 0xFF));
	frame.push_back((BYTE)((len >> 16) & 0xFF));
	frame.push_back((BYTE)((len >> 24) & 0xFF));
	frame.push_back(frameType);
	frame.insert(frame.end(), data.begin(), data.end());
	send(s, (const char *)&frame[0], (int)frame.size(), 0);
}

// [v65-order-restore] Runtime knobs, read once from "mp_ctx.cfg" (plain chars,
// no newline required). Exists so behaviour can be flipped in seconds instead
// of waiting for a cloud rebuild - v54..v61 burned days flipping constants.
//   [0] '1' = remote packets go to CWvsContext, '0' = CField      (default '1')
//   [1] '1' = also push 0x3D account data for remote players      (default '1')
//   [2] '1' = after the remote 0x3D+0x11 pair, replay the RECEIVER's own 0x3D
//             so their account context is restored                (default '1')
//   [3] '1' = emit the player's own 0x11 BEFORE the cross-visibility loop
//                                                                 (default '1')
//   [4] '1' = drive remote movement by re-sending 0x11 at the new coordinates,
//             '0' = relay the raw 0x0C verbatim (the old, proven-dead path)
//                                                                 (default '1')
//
// v61b field evidence - all three behaviours below are now proven, not guessed:
//  * 0x3D IS required (v61's "never verified" note was wrong, v54b was right).
//    The 0x11 handler (0x0048DB9B..0x0048DE21) looks the oid up first at
//    0x0048DCEF - "je 0x48dd4c" means object-not-found -> packet dropped - and
//    only reaches the render check at 0x0048DD03 (the one we NOP) afterwards.
//    The object is created by 0x3D (handler 0x00498E4F). With 0x3D off the
//    spawn died at 0x48DCEF and the render patch never even ran. Default OFF->ON.
//  * The remote 0x3D also overwrites the RECEIVER's own account context, which
//    is exactly why the first player rendered the newcomer but then could not
//    move. Knob [2] repairs that by replaying the receiver's own 0x3D after.
//  * The client latches its identity onto the FIRST 0x11 it sees. The joining
//    player received the remote spawn (oid=0x54C) before its own (oid=0x57C),
//    locked onto the wrong character and drove a ghost - it could not move and
//    could not see anyone. Knob [3] sends self spawn first to lock identity.
static void MP_LoadCtxCfg(int &ctx, int &send3d, int &restore3d, int &selffirst,
		int &moveaspawn, int &rebuild) {
	ctx = 1;
	send3d = 1;
	restore3d = 1;
	selffirst = 1;
	moveaspawn = 1;
	rebuild = 1;
	FILE *f = NULL;
	if (fopen_s(&f, "mp_ctx.cfg", "r") == 0 && f) {
		int c0 = fgetc(f);
		if (c0 == '0') ctx = 0;
		int c1 = fgetc(f);
		if (c1 == '0') send3d = 0;
		int c2 = fgetc(f);
		if (c2 == '0') restore3d = 0;
		int c3 = fgetc(f);
		if (c3 == '0') selffirst = 0;
		int c4 = fgetc(f);
		if (c4 == '0') moveaspawn = 0;
		int c5 = fgetc(f);
		if (c5 == '0') rebuild = 0;
		fclose(f);
	}
}

// One lazy load shared by every knob, so the config line is logged exactly once.
static void MP_Cfg(int &ctx, int &send3d, int &restore3d, int &selffirst,
		int &moveaspawn, int &rebuild) {
	static int s_ctx = -1, s_3d = -1, s_rst = -1, s_first = -1, s_move = -1, s_rebuild = -1;
	if (s_ctx < 0) {
		MP_LoadCtxCfg(s_ctx, s_3d, s_rst, s_first, s_move, s_rebuild);
		Log("[MP-CFG] v102 ctx=%d (%s) send0x3D=%d restore0x3D=%d selfSpawnFirst=%d moveAsSpawn=%d rebuildMove=%d",
			s_ctx, s_ctx ? "CWvsContext" : "CField", s_3d, s_rst, s_first, s_move, s_rebuild);
	}
	ctx = s_ctx;
	send3d = s_3d;
	restore3d = s_rst;
	selffirst = s_first;
	moveaspawn = s_move;
	rebuild = s_rebuild;
}

bool MP_RemoteCtx() {
	int ctx, send3d, restore3d, selffirst, moveaspawn, rebuild;
	MP_Cfg(ctx, send3d, restore3d, selffirst, moveaspawn, rebuild);
	return ctx != 0;
}

bool MP_RemoteSend3D() {
	int ctx, send3d, restore3d, selffirst, moveaspawn, rebuild;
	MP_Cfg(ctx, send3d, restore3d, selffirst, moveaspawn, rebuild);
	return send3d != 0;
}

bool MP_Restore3D() {
	int ctx, send3d, restore3d, selffirst, moveaspawn, rebuild;
	MP_Cfg(ctx, send3d, restore3d, selffirst, moveaspawn, rebuild);
	return restore3d != 0;
}

bool MP_SelfSpawnFirst() {
	int ctx, send3d, restore3d, selffirst, moveaspawn, rebuild;
	MP_Cfg(ctx, send3d, restore3d, selffirst, moveaspawn, rebuild);
	return selffirst != 0;
}

bool MP_MoveAsSpawn() {
	int ctx, send3d, restore3d, selffirst, moveaspawn, rebuild;
	MP_Cfg(ctx, send3d, restore3d, selffirst, moveaspawn, rebuild);
	return moveaspawn != 0;
}

bool MP_RebuildMove() {
	int ctx, send3d, restore3d, selffirst, moveaspawn, rebuild;
	MP_Cfg(ctx, send3d, restore3d, selffirst, moveaspawn, rebuild);
	return rebuild != 0;
}

// [MP] 把包发给指定 sid 的连接(供 FakeServer 做跨玩家广播)
// [v50] context=false means the receiving client must route this through
// CField. Cross-player packets are, by definition, remote to the receiver -
// this is exactly the information that used to be dropped here.
void MP_BroadcastToSid(int sid, ServerPacket &sp, bool context) {
	SOCKET s = INVALID_SOCKET;
	{
		std::lock_guard<std::mutex> lk(g_adminMutex);
		auto it = g_onlineSock.find(sid);
		if (it != g_onlineSock.end()) s = it->second;
	}
	// [v61-diag] This path used to be completely silent. Every cross-player
	// packet (spawn/account/move) goes through here; if the target sid is not
	// in g_onlineSock the packet was dropped without a trace, which is why
	// v57/v58/v60 were all diagnosed blind.
	{
		vector<BYTE> pv = sp.get();
		int op = pv.empty() ? -1 : (int)pv[0];
		Log("[MP-SEND] to_sid=%d op=%02X len=%d ctx=%d sock=%s",
			sid, op, (int)pv.size(), context ? 1 : 0,
			(s != INVALID_SOCKET) ? "OK" : "MISSING");
	}
	if (s != INVALID_SOCKET) {
		SendPacketTo(s, sp, context ? MP_TYPE_GAME : MP_TYPE_GAME_FIELD);
	}
}

// [MP] GM 广播: 给所有在线客户端发一条 Board 公告
// 注意: CN v126 目前没有确认过 SP_BOARD 的 opcode(CN_v126_SP.cpp 里是注释掉的),
// 发一个 opcode=0 的包客户端会当未知包处理, 有崩溃风险 -> 没映射就直接拒绝。
static bool AdminBroadcast(const std::wstring &msg) {
	if (ServerPacket::GetOpcode()[SP_BOARD] == 0) {
		Log("broadcast unsupported: SP_BOARD opcode not mapped for this region");
		return false;
	}
	std::lock_guard<std::mutex> lock(g_adminMutex);
	for (auto &kv : g_onlineSock) {
		ServerPacket sp(SP_BOARD);
		sp.Encode1(0);          // Board_Spawn
		sp.Encode4(3131);
		sp.Encode4(1337);
		sp.EncodeWStr1(L"GM");  // owner
		sp.EncodeWStr1(msg);    // message
		sp.Encode4(0);
		sp.Encode4(0);
		sp.Encode1(0);
		sp.Encode1(3);
		SendPacketTo(kv.second, sp);
	}
	return true;
}

// [MP] GM 管理端口(本地 127.0.0.1:8788)命令处理线程
static DWORD WINAPI AdminThread(LPVOID) {
	SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls == INVALID_SOCKET) { Log("admin: socket failed"); return 0; }
	BOOL reuse = TRUE;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 仅本机
	a.sin_port = htons((u_short)MP_ADMIN_PORT);
	if (bind(ls, (sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
		Log("admin: bind failed, err=%d (port %d in use?)", WSAGetLastError(), MP_ADMIN_PORT);
		closesocket(ls);
		return 0;
	}
	listen(ls, SOMAXCONN);
	Log("admin GM port listening on 127.0.0.1:%d", MP_ADMIN_PORT);

	while (true) {
		SOCKET c = accept(ls, NULL, NULL);
		if (c == INVALID_SOCKET) break;
		char buf[4096] = {};
		int n = recv(c, buf, sizeof(buf) - 1, 0);
		if (n > 0) {
			buf[n] = 0;
			std::string line = buf;
			// 去掉换行
			while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

			// 简单命令解析: CMD arg1 arg2 ...
			std::vector<std::string> tok;
			std::string cur;
			for (char ch : line) { if (ch == ' ') { tok.push_back(cur); cur.clear(); } else cur += ch; }
			tok.push_back(cur);

			std::string cmd = tok.empty() ? "" : tok[0];
			std::string resp;

			if (cmd == "LIST") {
				std::lock_guard<std::mutex> lk(g_adminMutex);
				resp += "ONLINE " + std::to_string(g_onlineSock.size()) + "\n";
				for (auto &kv : g_onlineSock) {
					std::string acc = WToUtf8(g_onlineAcc[kv.first]);
					resp += std::to_string(kv.first) + "\t" + (acc.empty() ? "(none)" : acc) + "\n";
				}
				resp += "END\n";
			}
			else if (cmd == "KICK" && tok.size() >= 2) {
				int sid = atoi(tok[1].c_str());
				{
					std::lock_guard<std::mutex> lk(g_adminMutex);
					g_kickSet.insert(sid);
				}
				resp = "KICKED " + std::to_string(sid) + "\n";
			}
			else if (cmd == "BCAST" && tok.size() >= 2) {
				// 拼接剩余参数为消息
				std::string m;
				for (size_t i = 1; i < tok.size(); i++) { if (i > 1) m += " "; m += tok[i]; }
				resp = AdminBroadcast(Utf8ToW(m)) ? "BCAST OK\n" : "BCAST UNSUPPORTED\n";
			}
			else if (cmd == "NPCTEST" && tok.size() >= 2) {
				// NPCTEST <opcode_hex> [object_id] - 向第一个在线玩家发NPC对话测试包
				BYTE testOp = (BYTE)strtoul(tok[1].c_str(), NULL, 16);
				DWORD objId = tok.size() >= 3 ? (DWORD)atoi(tok[2].c_str()) : 19;
				SOCKET s = INVALID_SOCKET;
				{
					std::lock_guard<std::mutex> lk(g_adminMutex);
					if (!g_onlineSock.empty()) s = g_onlineSock.begin()->second;
				}
				if (s != INVALID_SOCKET) {
					// 构造 NPC talk 包: [opcode][npc_obj 4B][msg_type 0][text WStr2]
					std::vector<BYTE> raw;
					raw.push_back(testOp);
					raw.push_back(objId & 0xFF); raw.push_back((objId>>8)&0xFF);
					raw.push_back((objId>>16)&0xFF); raw.push_back((objId>>24)&0xFF);
					raw.push_back(0); // msg_type=0
					std::wstring msg = L"NPC TEST op=0x" + std::to_wstring(testOp);
					raw.push_back((msg.length()>>0)&0xFF); raw.push_back((msg.length()>>8)&0xFF);
					for (wchar_t ch : msg) { raw.push_back(ch&0xFF); raw.push_back((ch>>8)&0xFF); }
					// 包 MP 头
					DWORD packLen = (DWORD)raw.size() + 1;
					std::vector<BYTE> frame;
					frame.push_back(packLen & 0xFF); frame.push_back((packLen>>8)&0xFF);
					frame.push_back((packLen>>16)&0xFF); frame.push_back((packLen>>24)&0xFF);
					frame.push_back(MP_TYPE_GAME);
					frame.insert(frame.end(), raw.begin(), raw.end());
					int r = send(s, (const char*)&frame[0], (int)frame.size(), 0);
					resp = "NPCTEST op=0x" + std::to_string(testOp) + " obj=" + std::to_string(objId) + " sent=" + std::to_string(r) + "B\n";
				} else {
					resp = "NO PLAYER ONLINE\n";
				}
			}
			else if (cmd == "SETLV" && tok.size() >= 3) {
				std::wstring acc = Utf8ToW(tok[1]);
				BYTE lv = (BYTE)atoi(tok[2].c_str());
				// 对该账号下所有角色设置等级(下一轮选角/列表生效)
				auto it = db().chars().find(acc);
				if (it != db().chars().end()) {
					for (auto &r : it->second) db().updateCharStat(acc, r.id, lv, r.gold);
					resp = "SETLV OK\n";
				}
				else resp = "NOACCOUNT\n";
			}
			else if (cmd == "SETGOLD" && tok.size() >= 3) {
				std::wstring acc = Utf8ToW(tok[1]);
				int g = atoi(tok[2].c_str());
				auto it = db().chars().find(acc);
				if (it != db().chars().end()) {
					for (auto &r : it->second) db().updateCharStat(acc, r.id, r.level, g);
					resp = "SETGOLD OK\n";
				}
				else resp = "NOACCOUNT\n";
			}
			else {
				resp = "UNKNOWN " + cmd + "\n";
			}

			send(c, resp.c_str(), (int)resp.size(), 0);
		}
		closesocket(c);
	}
	closesocket(ls);
	return 0;
}

static void HandleCtrl(BYTE cmd, const BYTE *p, DWORD n) {
	switch (cmd) {
	case MP_CTRL_HELLO:
		Log("client says HELLO -> sending version packet");
		VersionPacket();
		break;
	case MP_CTRL_WORLDLIST:
		Log("ctrl: world list");
		WorldListPacket();
		break;
	case MP_CTRL_CHARLIST:
		Log("ctrl: character list");
		// [MP] 0x04(切到角色选择屏)改由客户端 WorldSelectButton_Hook 同步注入,
		// 避免异步时机错位导致 UI 崩溃。这里只回 0x05 真实角色数据。
		CharacterListPacket_Test();
		break;
	case MP_CTRL_LOGIN: {
		// [MP] payload[2..] = utf8 "账号\0密码"
		// 登录 = 自动注册 + 校验合一：新号自动建，老号验密码
		std::string body;
		if (n >= 3) body.assign((const char *)(p + 2), n - 2);
		size_t z = body.find('\0');
		std::string acc8 = (z == std::string::npos) ? body : body.substr(0, z);
		std::string pw8  = (z == std::string::npos) ? std::string() : body.substr(z + 1);
		std::wstring wacc = Utf8ToW(acc8);
		std::wstring wpw  = Utf8ToW(pw8);
		if (wacc.empty()) wacc = L"Player";

		if (!db().accountExists(wacc)) {
			// 新账号 -> 自动注册
			db().registerAccount(wacc, wpw);
			Log("ctrl: auto-registered new account = %s", acc8.c_str());
		} else if (!db().checkPassword(wacc, wpw)) {
			// 老账号 -> 校验密码
			Log("ctrl: login FAILED (wrong password) account = %s", acc8.c_str());
			BYTE fail = 0;
			SendCtrlResult(MP_CTRL_LOGIN_RESULT, &fail, 1);
			break; // 不 SetAccount, 客户端停留在登录界面
		}

		TA.SetAccount(wacc);
		TA.ReloadFromDB();
		{
			std::lock_guard<std::mutex> lock(g_adminMutex);
			g_onlineAcc[t_sid] = wacc;
		}
		BYTE ok = 1;
		SendCtrlResult(MP_CTRL_LOGIN_RESULT, &ok, 1);
		Log("ctrl: login OK account = %s (chars=%d)", acc8.c_str(), (int)TA.GetCharacters().size());
		break;
	}
	case MP_CTRL_REGISTER: {
		// [MP] payload[2..] = utf8 "账号\0密码"
		std::string body;
		if (n >= 3) body.assign((const char *)(p + 2), n - 2);
		size_t z = body.find('\0');
		std::string acc8 = (z == std::string::npos) ? body : body.substr(0, z);
		std::string pw8  = (z == std::string::npos) ? std::string() : body.substr(z + 1);
		std::wstring wacc = Utf8ToW(acc8);
		std::wstring wpw  = Utf8ToW(pw8);
		if (wacc.empty()) {
			Log("ctrl: register FAILED (empty account)");
			break;
		}
		bool ok = db().registerAccount(wacc, wpw);
		BYTE rb = ok ? 1 : 0;
		SendCtrlResult(MP_CTRL_REGISTER_RESULT, &rb, 1);
		Log("ctrl: register account = %s -> %s", acc8.c_str(), ok ? "OK" : "EXISTS");
		break;
	}
	default:
		Log("unknown ctrl cmd = %u", (unsigned)cmd);
		break;
	}
}

// 处理一条完整 payload
static void HandlePayload(const BYTE *p, DWORD n) {
	if (n < 1) {
		return;
	}
	BYTE type = p[0];
	if (type == MP_TYPE_CTRL) {
		if (n >= 2) {
			HandleCtrl(p[1], p, n);
		}
		return;
	}
	if (type != MP_TYPE_GAME || n < 2) {
		return;
	}
	// [v55] 移动同步: 客户端发出的 0x0C 移动包原样转发给同图其他人
	if (p[1] == 0x0C) {
		DWORD oid = (n >= 6) ? *(DWORD *)(p + 2) : 0;
		printf("[MP-HDL] 0x0C from sid=%d len=%d oid=%08X\n", (int)t_sid, (int)(n - 1), (unsigned)oid);
		MP_ForwardToSameMap(p + 1, n - 1);
	}
	if (t_recvCount < g_dump) {
		HexDump("RECV", p + 1, n - 1);
		t_recvCount++;
	}
	ClientPacket cp((BYTE *)(p + 1), n - 1);
	if (!FakeServer(cp)) {
		// [DEBUG] 记录未识别的游戏包 opcode
		BYTE unk_op = (n >= 2) ? p[1] : 0;
		printf("[UNK-RECV] opcode=0x%02X len=%d\n", (unsigned)unk_op, (int)(n - 1));
		// Dump all unknown packets where opcode is 0x52 (likely chat) or len < 200
		if (unk_op == 0x52 || (int)(n - 1) < 200) {
			int dumpLen = (n - 1 < 120) ? (n - 1) : 120;
			for (int i = 1; i <= dumpLen; i++) printf("%02X ", (unsigned)(BYTE)p[i]);
			printf("|");
			for (int i = 1; i <= dumpLen; i++) { char ch = p[i]; printf("%c", (ch>=32&&ch<127)?ch:'.'); }
			printf("\n");
		}
		fflush(stdout);
	}
}

static void ServeClient() {
	vector<BYTE> buf;
	char tmp[16384];
	while (true) {
		// [MP] GM 踢人: 被标记则断开
		{
			std::lock_guard<std::mutex> lock(g_adminMutex);
			if (g_kickSet.count(t_sid)) {
				Log("kicked by GM (sid=%d)", t_sid);
				break;
			}
		}
		int n = recv(t_client, tmp, sizeof(tmp), 0);
		if (n <= 0) {
			break;
		}
		buf.insert(buf.end(), tmp, tmp + n);
		while (buf.size() >= 4) {
			DWORD len = (DWORD)buf[0] | ((DWORD)buf[1] << 8) | ((DWORD)buf[2] << 16) | ((DWORD)buf[3] << 24);
			if (len == 0 || len > 65536) {
				Log("!! bad frame length = %u, dropping buffer", len);
				HexDump("BADBUF", &buf[0], buf.size() > 32 ? 32 : buf.size());
				buf.clear();
				break;
			}
			if (buf.size() < 4 + len) {
				break;
			}
			HandlePayload(&buf[4], len);
			buf.erase(buf.begin(), buf.begin() + 4 + len);
		}
	}
}

// 每个玩家一个线程。TA 是 thread_local, 所以会话状态天然隔离。
static DWORD WINAPI ClientThread(LPVOID param) {
	t_client = (SOCKET)(UINT_PTR)param;
	t_sid = (int)InterlockedIncrement(&g_sidSeq);
	t_sendCount = 0;
	t_recvCount = 0;

	LONG online = InterlockedIncrement(&g_online);
	Log("player joined (online=%d)", online);

	// [MP] 注册到 GM 在线表
	{
		std::lock_guard<std::mutex> lock(g_adminMutex);
		g_onlineSock[t_sid] = t_client;
		g_onlineAcc[t_sid] = TA.GetAccount();
	}

	ServeClient();

	// [MP] 从 GM 在线表移除
	{
		std::lock_guard<std::mutex> lock(g_adminMutex);
		g_onlineSock.erase(t_sid);
		g_onlineAcc.erase(t_sid);
		g_kickSet.erase(t_sid);
	}
	MP_RemovePlayer(t_sid); // [MP] 通知同图玩家移除本对象(锁外调用, 避免重入 g_adminMutex)

	closesocket(t_client);
	t_client = INVALID_SOCKET;
	online = InterlockedDecrement(&g_online);
	Log("player left (online=%d)", online);
	return 0;
}

int main(int argc, char **argv) {
	// 日志直出，别让 stdout 缓冲把日志卡在管道里（启动器要实时读）
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	// 参数：第一个非选项参数 = xml 数据目录；-p 端口；-d 十六进制转储条数
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
			g_port = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			g_dump = atoi(argv[++i]);
		}
		else if (argv[i][0] != '-') {
			wchar_t wbuf[512] = { 0 };
			MultiByteToWideChar(CP_ACP, 0, argv[i], -1, wbuf, 511);
			g_xmlPath = wbuf;
		}
	}

	Log("=== Tenvi standalone server (multi-player) ===");
	Log("server build = MP_SERVER_V89_OID_REWRITE");
	LogW("xml path = ", g_xmlPath);
	LogW("region   = ", g_regionStr);
	Log("port     = %d", g_port);
	Log("max players = %d", MP_MAX_PLAYERS);

	// [MP] 打开文件数据库(存于服务端 exe 同目录)
	{
		wchar_t exepath[1024] = { 0 };
		DWORD lp = GetModuleFileNameW(NULL, exepath, 1023);
		std::wstring dir;
		if (lp > 0) {
			std::wstring p(exepath, lp);
			size_t pos = p.find_last_of(L"\\/");
			dir = (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
		}
		else {
			dir = L".";
		}
	db().open(dir);
	LogW("db path  = ", dir + L"\\tenvi.db");
	}

	// 加载地图/NPC 数据（FakeServer 换图、刷怪要用）
	tenvi_data.set_xml_path(g_xmlPath);
	Log("xml data loaded.");

	// 初始化国服 v126 的 opcode 编解码表
	SetClientPacketHeader_CN_v126();
	SetServerPacketHeader_CN_v126();

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		Log("WSAStartup failed");
		return 1;
	}

	// [MP] 启动 GM 管理端口线程（必须在 WSAStartup 之后，否则 socket() 会失败）
	{
		HANDLE h = CreateThread(NULL, 0, AdminThread, NULL, 0, NULL);
		if (h) CloseHandle(h);
	}

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
	if (listenSock == INVALID_SOCKET) {
		Log("socket failed");
		return 1;
	}

	BOOL reuse = TRUE;
	setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡, 外网玩家才连得进来
	addr.sin_port = htons((u_short)g_port);
	if (bind(listenSock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		Log("bind failed, err=%d (port %d already in use?)", WSAGetLastError(), g_port);
		return 1;
	}
	listen(listenSock, SOMAXCONN);
	Log("listening on 0.0.0.0:%d, waiting for players...", g_port);

	while (true) {
		sockaddr_in ca = {};
		int calen = sizeof(ca);
		SOCKET s = accept(listenSock, (sockaddr *)&ca, &calen);
		if (s == INVALID_SOCKET) {
			Log("accept failed, err=%d", WSAGetLastError());
			break;
		}

		if (g_online >= MP_MAX_PLAYERS) {
			Log("server full (%d), rejecting %s", MP_MAX_PLAYERS, inet_ntoa(ca.sin_addr));
			closesocket(s);
			continue;
		}

		int flag = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
		Log("connection from %s", inet_ntoa(ca.sin_addr));

		HANDLE h = CreateThread(NULL, 0, ClientThread, (LPVOID)(UINT_PTR)s, 0, NULL);
		if (h) {
			CloseHandle(h);
		}
		else {
			Log("CreateThread failed, dropping connection");
			closesocket(s);
		}
	}

	closesocket(listenSock);
	WSACleanup();
	Log("stopped.");
	return 0;
}

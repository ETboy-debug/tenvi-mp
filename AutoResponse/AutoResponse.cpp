#include"AutoResponse.h"
#include"MPClient.h"
#include <map>
#include <cstring>
#include <cmath>

DWORD Addr_OnPacketClass = 0;
DWORD Addr_OnPacketClass2 = 0;
DWORD Addr_OnPacket2 = 0;

// [v71] Client-side movement interpolation scaffold (default OFF).
// We intercept remote 0x11 spawns here, cache oid + target coords, and
// (when enabled via mp_interp.cfg) would lerp the client's CCharacter object
// each frame so the remote avatar slides instead of teleporting. Writing to
// client memory needs CCharacter x/y offsets still TBD via Cheat Engine, so
// MP_InterpApply() is a no-op + diag log until those offsets are filled in.
// Safe by design: if the cfg is missing or does not say interp=1, nothing
// happens and the existing spawn/teleport path is untouched.
static const char* MP_INTERP_TAG = "MP_INTERP_V77";

struct RemoteInterp {
	DWORD oid;
	float tx, ty;   // target (last server coord from 0x11)
	float cx, cy;   // current client coord - filled once offsets known
	int   stale;    // frames since last 0x11
};

static std::map<DWORD, RemoteInterp> g_interp;
static bool g_interp_announced = false;
static int mp_frame_count = 0;  // per-frame counter, incremented at top of MP_Pump

// [v73] Direct call to CField::GetCharacterByOID(this=CField*, oid) at 0x42ACDD.
// We do NOT hook it: the 0x11 spawn handler resolves remote characters via the
// internal sibling label 0x42AC5C, so a hook on 0x42ACDD would miss them.
// Instead we call 0x42ACDD explicitly every frame from MP_Pump to look up the
// live CCharacter* for each tracked remote oid.  thiscall: ecx=CField*,
// [esp+4]=oid, eax=CCharacter*.
typedef DWORD (__thiscall *GetCharacterByOIDFn)(void *this_ptr, DWORD oid);
static GetCharacterByOIDFn _GetCharacterByOID = (GetCharacterByOIDFn)0x0042ACDD;
static DWORD MP_GetCFieldPtr() { return *(DWORD *)0x006FAF6C; }
static std::map<DWORD, DWORD> g_char_by_oid;  // filled per-frame by direct lookup

// [v73] Interpolation config: reads interp=1, x_off=HEX, y_off=HEX, speed=FLOAT.
// Offsets are byte offsets into the CCharacter object where x/y floats live.
// Speed is the lerp factor per frame (0.1 = move 10% of the gap each frame).
static int g_interp_x_off = 0;
static int g_interp_y_off = 0;
static float g_interp_speed = 0.15f;

// Returns true only if "mp_interp.cfg" (placed next to Tenvi.exe) contains
// the line "interp=1". ASCII filename only - no Chinese in source per build rule.
static bool MP_InterpEnabled() {
	FILE* f = NULL;
	fopen_s(&f, "mp_interp.cfg", "r");
	if (!f) return false;
	char line[128];
	bool on = false;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "interp=1", 8) == 0) on = true;
		else if (strncmp(line, "x_off=0x", 8) == 0) {
			g_interp_x_off = (int)strtol(line + 6, NULL, 16);
		} else if (strncmp(line, "y_off=0x", 8) == 0) {
			g_interp_y_off = (int)strtol(line + 6, NULL, 16);
		} else if (strncmp(line, "speed=", 6) == 0) {
			g_interp_speed = (float)atof(line + 6);
		}
	}
	fclose(f);
	return on;
}

/*
	0055E7E4 - 8B 10                 - mov edx,[eax]
	0055E7E6 - 56                    - push esi
	0055E7E7 - 8B C8                 - mov ecx,eax
	0055E7E9 - FF 52 2C              - call dword ptr [edx+2C] // _OnPacket (its like CWvsContext::OnPacket)
	0055E7EC - 8B 0D 90B16D00        - mov ecx,[006DB190]
	0055E7F2 - 56                    - push esi
	0055E7F3 - E8 BCA9F6FF           - call 004C91B4 // CField::OnPacket ?
*/

// ignore packet encryption
void OnPacketDirectExec(InPacket *p, bool context = true) {
	if (context) {
		void *OnPacketClass = (void *)(*(DWORD *)(*(DWORD *)Addr_OnPacketClass + 0x160));
		void(__thiscall *_OnPacket)(void *, InPacket *) = (decltype(_OnPacket)(*(DWORD *)(*(DWORD *)OnPacketClass + 0x2C)));
		_OnPacket(OnPacketClass, p); // its like CWvsContext::OnPacket
	}
	else {
		void *OnPacketClass2 = (void *)(*(DWORD *)Addr_OnPacketClass2);
		void (__thiscall *_OnPacket2)(void *, InPacket*) = (decltype(_OnPacket2))Addr_OnPacket2;
		_OnPacket2(OnPacketClass2, p);
	}
}

void ProcessPacketExec(std::vector<BYTE> &packet, bool context = true) {
	std::vector<BYTE> buffer;
	// first 4 bytes
	buffer.push_back(0);
	buffer.push_back(0);
	buffer.push_back(0);
	buffer.push_back(0);
	// packet
	buffer.insert(buffer.end(), packet.begin(), packet.end());

	InPacket ip = {};
	ip.unk2 = 2; // always 2
	ip.unk4 = 1; // always 1
	ip.decoded = 4; // ignore first 4 bytes
	ip.packet = &buffer[0]; // real buffer
	ip.length = (WORD)buffer.size(); // real buffer size

	{
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "Exec op=%02X len=%d\n", packet.size()>0?packet[0]:0, (int)packet.size()); fflush(f); fclose(f); }
	}
	return OnPacketDirectExec(&ip, context);
}

void SendPacket(ServerPacket &sp) {
	return ProcessPacketExec(sp.get());
}
void SendPacket2(ServerPacket &sp) {
	return ProcessPacketExec(sp.get(), false);
}

// Delay Execution
std::vector<std::vector<BYTE>> packet_queue;

void DelaySendPacket(ServerPacket &sp) {
	packet_queue.push_back(sp.get());
}

void DelayExecution() {
	if (packet_queue.size()) {
		auto &packet = packet_queue[0];
		ProcessPacketExec(packet); // delay execution
		packet_queue.erase(packet_queue.begin());
	}
}

// [MP] 把独立服务端发来的明文包注入客户端。
// 必须在客户端主线程执行, 所以挂在 ProcessPacketCaller 这个每帧轮询点上。
// [FIX v3] 原子批处理：先把队列中所有包取到本地缓冲，再一口气注入。
// 这样客户端原始代码不会在包序列中间插队（如换地图后发确认包打断出生包）。
// [MP] Batch guard: suppress all client sends during packet injection batch
// to prevent client-side state corruption (e.g. op=1E during op=10 processing)
static bool g_mp_in_batch = false;

void MP_Pump() {
	mp_frame_count++;
	// [v50] Each entry carries the dispatch context supplied by the server
	// (frame type 0 = CWvsContext, type 2 = CField). No more guessing.
	std::vector<std::pair<std::vector<BYTE>, bool>> batch;
	std::vector<BYTE> packet;
	bool srv_ctx = true;
	while (MP_PopPacketEx(packet, srv_ctx)) {
		batch.push_back(std::make_pair(packet, srv_ctx));
	}
	if (batch.empty()) return;
	// [v71] Announce the interpolation scaffold once, so a deployed DLL can
	// be confirmed via the diag log without relying on a version string grep.
	if (!g_interp_announced) {
		g_interp_announced = true;
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[MP-INTERP] %s scaffold loaded\n", MP_INTERP_TAG); fflush(f); fclose(f); }
	}

	g_mp_in_batch = true;  // <-- START guard: block all EnterSendPacket during batch
	// [v50] Dispatch context now comes straight from the server frame type.
	// The server is the only side that actually knows whether a 0x11 spawn
	// belongs to this client's own character or to a remote player, so it
	// tags every packet: SendPacket -> type 0 (CWvsContext), SendPacket2 ->
	// type 2 (CField). The old v46 heuristic ("first 0x11 after a 0x10 is
	// me") broke as soon as the server sent remote spawns first: the second
	// player to log in adopted the FIRST player's object id as its own, so
	// it could not even see itself.
	//
	// g_localObjectId is kept for diagnostics only - it no longer drives any
	// routing decision.
	static DWORD g_localObjectId = 0;
	for (size_t i = 0; i < batch.size(); i++) {
		std::vector<BYTE> &bp = batch[i].first;   // non-const: ProcessPacketExec takes a mutable ref
		bool mp_ctx = batch[i].second;
		BYTE mp_op = (bp.size() > 0) ? bp[0] : 0;
		DWORD oid = 0;
		if (bp.size() >= 5) oid = *(DWORD*)&bp[1];
		// Diagnostics: remember the first self-tagged spawn we ever see.
		if (mp_op == 0x11 && mp_ctx && g_localObjectId == 0) g_localObjectId = oid;
		// [v72a-fix] Cache remote 0x11 spawn coords for interpolation. A remote
		// spawn is ANY 0x11 whose oid is not our own object id. Empirically the
		// deployed server sends all 0x11 spawns on the CWvsContext layer
		// (mp_ctx=true, ctx=1) - NOT CField - yet they render fine, so the old
		// "!mp_ctx" gate was wrong and left g_interp empty (hook never fired).
		// Drop the ctx requirement; identify remote purely by oid != self.
		// 0x11 body: [0]=op, [1..4]=oid, [5..8]=x(float), [9..12]=y(float).
		if (mp_op == 0x11 && oid != g_localObjectId && bp.size() >= 13) {
			float rx = *(float*)&bp[5];
			float ry = *(float*)&bp[9];
			auto it = g_interp.find(oid);
			if (it == g_interp.end()) {
				RemoteInterp ri;
				ri.oid = oid; ri.tx = rx; ri.ty = ry;
				ri.cx = rx; ri.cy = ry; ri.stale = 0;
				g_interp[oid] = ri;
			} else {
				it->second.tx = rx; it->second.ty = ry; it->second.stale = 0;
			}
		// [v77] The deployed server sends initial 0x11 spawns through
		// CWvsContext (ctx=1) so the client renders them, but CField also
		// keeps a CCharacter object for each remote player. The renderable
		// coordinates live in the CField object; the CWvsContext copy is only
		// used for spawn/rendering. Therefore the interpolation target is the
		// CField CCharacter, found via GetCharacterByOID(0x42ACDD), exactly as
		// before. The fix here is a more robust auto-detection of x/y offsets:
		// the v76 heuristic latched onto a +0x0 candidate (object vtable-ish
		// float) as x because it allowed any error < 1.0, which silently broke
		// horizontal movement.
		static bool probe_offsets_found = false;
		if (MP_InterpEnabled() && !probe_offsets_found) {
			DWORD cfield = MP_GetCFieldPtr();
			DWORD char_ptr = (cfield && _GetCharacterByOID) ? _GetCharacterByOID((void*)cfield, oid) : 0;
			if (char_ptr != 0) {
				g_char_by_oid[oid] = char_ptr;
				FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
				if (f) {
					fprintf(f, "[MP-PROBE] Scanning CCharacter at %08X for spawn coords (%.1f, %.1f)\n",
						char_ptr, rx, ry);
					// [v83-diag] Client snaps spawn coords to the platform foothold,
					// so the stored value may differ from the packet coord. Dump
					// plausible floats and report any field within 50.0 of either
					// packet coord so the real x/y offset is visible.
					static int g_probe_dump = 0;
					if (g_probe_dump < 2) {
						g_probe_dump++;
						fprintf(f, "  [MP-PROBE-NEAR] fields within 50.0 of (%.1f,%.1f):\n", rx, ry);
						for (int off = 0; off < 0x2000; off += 4) {
							float val = 0;
							memcpy(&val, (void*)(char_ptr + off), 4);
							if (!isfinite(val)) continue;
							if (fabsf(val - rx) < 50.0f || fabsf(val - ry) < 50.0f)
								fprintf(f, "    +0x%04X = %14.4f\n", off, val);
						}
						fprintf(f, "  [MP-PROBE-DUMP] all floats |v| in [0.5,500000] (0x2000):\n");
						for (int off = 0; off < 0x2000; off += 4) {
							float val = 0;
							memcpy(&val, (void*)(char_ptr + off), 4);
							if (!isfinite(val)) continue;
							if (fabsf(val) < 0.5f || fabsf(val) > 500000.0f) continue;
							fprintf(f, "    +0x%04X = %14.4f\n", off, val);
						}
					}
					int best_y = -1, best_x = -1;
					float best_y_err = 1.0e9f, best_x_err = 1.0e9f;
					// First pass: find y with tight tolerance. x/y are usually
					// close together in the object, so once y is pinned we look
					// for x in a tight window around y.
					const int SCAN_RANGE = 0x4000;
					const float TOL = 0.05f;
					for (int off = 0; off < SCAN_RANGE; off += 4) {
						float val = 0;
						memcpy(&val, (void*)(char_ptr + off), 4);
						if (!isfinite(val)) continue;
						float ey = fabsf(val - ry);
						if (ey < best_y_err && ey < TOL) {
							best_y_err = ey;
							best_y = off;
						}
					}
					if (best_y >= 0) {
						// Look for x within +/-0x40 of y; if not there, fall back
						// to a full-range search for x.
						int x_min = (best_y - 0x40 < 0) ? 0 : (best_y - 0x40);
						int x_max = (best_y + 0x40 > SCAN_RANGE) ? SCAN_RANGE : (best_y + 0x40);
						for (int off = x_min; off < x_max; off += 4) {
							if (off == best_y) continue;
							float val = 0;
							memcpy(&val, (void*)(char_ptr + off), 4);
							if (!isfinite(val)) continue;
							float ex = fabsf(val - rx);
							if (ex < best_x_err && ex < TOL) {
								best_x_err = ex;
								best_x = off;
							}
						}
						// Full fallback if x wasn't right next to y.
						if (best_x < 0) {
							for (int off = 0; off < SCAN_RANGE; off += 4) {
								if (off == best_y) continue;
								float val = 0;
								memcpy(&val, (void*)(char_ptr + off), 4);
								if (!isfinite(val)) continue;
								float ex = fabsf(val - rx);
								if (ex < best_x_err && ex < TOL) {
									best_x_err = ex;
									best_x = off;
								}
							}
						}
					}
					fprintf(f, "  [MP-PROBE] y candidate: +0x%04X err=%.4f\n", best_y, best_y_err);
					fprintf(f, "  [MP-PROBE] x candidate: +0x%04X err=%.4f\n", best_x, best_x_err);
					// Dump the float neighbourhood around y so we can eyeball x.
					if (best_y >= 0) {
						fprintf(f, "  [MP-PROBE] floats around y (+/-0x40):\n");
						int dump_min = (best_y - 0x40 < 0) ? 0 : (best_y - 0x40);
						int dump_max = (best_y + 0x40 > SCAN_RANGE - 4) ? (SCAN_RANGE - 4) : (best_y + 0x40);
						for (int off = dump_min; off <= dump_max; off += 4) {
							float val = 0;
							memcpy(&val, (void*)(char_ptr + off), 4);
							if (isfinite(val)) {
								fprintf(f, "    +0x%04X = %12.4f  %s\n", off, val,
									(off == best_y ? "<-Y" : (fabsf(val - rx) < TOL ? "<-?x" : "")));
							}
						}
					}
					if (best_x >= 0 && best_y >= 0 && best_x != best_y &&
						best_x_err < TOL && best_y_err < TOL) {
						fprintf(f, "  [MP-PROBE] *** AUTO-DETECTED: x_off=0x%X y_off=0x%X ***\n",
							best_x, best_y);
						if (g_interp_x_off == 0) g_interp_x_off = best_x;
						if (g_interp_y_off == 0) g_interp_y_off = best_y;
						probe_offsets_found = true;
					} else {
						fprintf(f, "  [MP-PROBE] no reliable match (tol=%.2f), will retry\n", TOL);
					}
					fflush(f); fclose(f);
				}
			}
		}
			// [v72a-diag] Ungated marker proving the spawn-cache block executes.
			static int spawn_seen = 0;
			if (spawn_seen < 50) {
				spawn_seen++;
				FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
				if (f) { fprintf(f, "[MP-INTERP] SPAWN oid=%08X -> (%.1f,%.1f) g_interp=%d\n",
					oid, rx, ry, (int)g_interp.size()); fflush(f); fclose(f); }
			}
			if (mp_frame_count % 30 == 0) {
				FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
				if (f) { fprintf(f, "[MP-INTERP] cache oid=%08X -> (%.1f,%.1f) total=%d\n",
					oid, rx, ry, (int)g_interp.size()); fflush(f); fclose(f); }
			}
		}
		{
			FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
			if (f) {
				fprintf(f, "[MP-CTX] op=%02X oid=%08X local=%08X ctx=%d src=srv\n",
					mp_op, oid, g_localObjectId, mp_ctx ? 1 : 0);
				fflush(f); fclose(f);
			}
		}
		{
			FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
			if (f) {
				fprintf(f, ">> inject op=%02X len=%d\n", mp_op, (int)bp.size());
				fflush(f); fclose(f);
			}
		}
		// [v83] Skip re-injecting a remote 0x11 whose CCharacter object already
		// exists (a movement update); re-injecting the spawn for an existing oid
		// crashes the client. Movement is driven by writing target coords into
		// CCharacter memory in the interpolation block below. Initial spawns
		// (object not yet tracked) are still injected so the avatar appears.
		bool mp_skip_inject = false;
		if (mp_op == 0x11 && oid != g_localObjectId) {
			auto it = g_char_by_oid.find(oid);
			if (it != g_char_by_oid.end() && it->second != 0) mp_skip_inject = true;
		}
		if (!mp_skip_inject) {
		ProcessPacketExec(bp, mp_ctx);
		{
			FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
			if (f) {
				fprintf(f, "<< done op=%02X\n", mp_op);
				fflush(f); fclose(f);
			}
		}
		}
	}
	{
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "=== BATCH END (%d) ===\n", (int)batch.size()); fflush(f); fclose(f); }
	}
	// [v73] Per-frame movement interpolation - REAL implementation.
	// For each tracked remote player:
	//   1. Look up the live CCharacter* via g_char_by_oid (populated each frame
	//      by a direct call to CField::GetCharacterByOID at 0x42ACDD).
	//   2. Read current x/y from the client object at the configured offsets.
	//   3. Lerp current toward target (tx/ty) by g_interp_speed.
	//   4. Write the smoothed coords back.
	// Result: the remote avatar slides smoothly instead of teleporting.
	// If offsets are not configured (both 0), we fall back to the old
	// no-op behavior so a missing cfg does not crash anything.
	if (MP_InterpEnabled()) {
		bool have_offsets = (g_interp_x_off != 0 || g_interp_y_off != 0);
		static int dbg = 0;
		if (++dbg % 60 == 0) {
			FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
			if (f) { fprintf(f, "[MP-INTERP] ENABLED tracked=%d offsets=(%04X,%04X) speed=%.3f\n",
				(int)g_interp.size(), g_interp_x_off, g_interp_y_off, g_interp_speed);
				fflush(f); fclose(f); }
		}
		// [v73] Resolve live CCharacter* for every tracked remote oid each frame.
		// The 0x11 handler uses an internal sibling (0x42AC5C) so we cannot rely on
		// a hook; we call the clean exported function 0x42ACDD directly instead.
		DWORD cfield = MP_GetCFieldPtr();
		if (cfield && _GetCharacterByOID) {
			for (auto &kv : g_interp) {
				DWORD ptr = _GetCharacterByOID((void*)cfield, kv.first);
				if (ptr) g_char_by_oid[kv.first] = ptr;
			}
		}
		if (have_offsets && !g_interp.empty()) {
			int applied = 0;
			for (auto &kv : g_interp) {
				RemoteInterp &ri = kv.second;
				ri.stale++;
				auto it = g_char_by_oid.find(ri.oid);
				if (it == g_char_by_oid.end() || it->second == 0) continue;
				DWORD char_ptr = it->second;
				// Read current client-side coords
				float cx = 0, cy = 0;
				if (g_interp_x_off) memcpy(&cx, (void*)(char_ptr + g_interp_x_off), 4);
				if (g_interp_y_off) memcpy(&cy, (void*)(char_ptr + g_interp_y_off), 4);
				// Lerp toward target
				float nx = cx + (ri.tx - cx) * g_interp_speed;
				float ny = cy + (ri.ty - cy) * g_interp_speed;
				// Snap if very close (avoids jitter when nearly arrived)
				if (fabsf(nx - ri.tx) < 0.5f) nx = ri.tx;
				if (fabsf(ny - ri.ty) < 0.5f) ny = ri.ty;
				// Write back
				if (g_interp_x_off) memcpy((void*)(char_ptr + g_interp_x_off), &nx, 4);
				if (g_interp_y_off) memcpy((void*)(char_ptr + g_interp_y_off), &ny, 4);
				ri.cx = nx; ri.cy = ny;
				applied++;
			}
			if (applied > 0 && (dbg % 60) == 0) {
				FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
				if (f) { fprintf(f, "[MP-INTERP] applied lerp to %d/%d tracked\n",
					applied, (int)g_interp.size()); fflush(f); fclose(f); }
			}
		} else {
			// No offsets configured - just age the entries like before
			for (auto &kv : g_interp) kv.second.stale++;
		}
	}
	// [v71] Drop stale entries so the map does not grow forever if a remote
	// player leaves and we never get a 0x12 remove. 600 frames ~ 10s @60fps.
	for (auto it = g_interp.begin(); it != g_interp.end(); ) {
		if (it->second.stale > 600) it = g_interp.erase(it);
		else ++it;
	}
	g_mp_in_batch = false;  // <-- END guard: allow sends again
}

// Login Button Click
// [v29] 用户在游戏原生登录界面打字(DLL通过GetAsyncKeyState后台静默捕获)，
//       点"登录"时取出捕获的账号密码 -> 发服务端认证(自动注册+验密合一)。
//       流程: 启动器无账号框 -> 进游戏 -> 登录界面打字(被捕获) ->
//             点登录 -> 这里取凭据 -> 发服务端 -> 成功进游戏
DWORD (__thiscall *_LoginButton)(void *ecx) = NULL;
DWORD __fastcall LoginButton_Hook(void *ecx) {
	if (MP_IsAuthed()) {
		DEBUG(L"[MP] LoginButton: already authed, let pass");
		return 0;
	}

	// 从 GetAsyncKeyState 捕获缓冲区取用户在登录界面输入的账号密码
	std::string acc, pw;
	if (!MP_GetNativeCred(acc, pw)) {
		MessageBoxA(NULL,
			"No account or password entered.\n"
			"\n"
			"1) Click the account field\n"
			"2) Type your account (e.g. ww1111)\n"
			"3) PAUSE half a second\n"
			"4) Type your password (e.g. 1111)\n"
			"5) Click Login.\n"
			"\n"
			"Note: DLL detects account->password boundary by your typing pause.",
			"Tenvi MP", MB_OK | MB_ICONINFORMATION);
		return 0;
	}
	if (pw.empty()) {
		MessageBoxA(NULL,
			"Account captured, but password is empty.\n"
			"\n"
			"You probably typed account + password without a clear pause between them.\n"
			"Try again: type account -> PAUSE 0.5s -> type password -> Login.",
			"Tenvi MP", MB_OK | MB_ICONWARNING);
		return 0;
	}

	DEBUG(L"[MP] Native login: acc=%hs pw=%d chars", acc.c_str(), (int)pw.length());

	// [v33] 登录前确保已连接服务端(解决"开游戏时服务端未启动导致连接线程退出"的问题)
	if (!MP_IsConnected()) {
		DEBUG(L"[MP] Not connected, reconnecting before login...");
		if (!MP_Reconnect()) {
			MessageBoxA(NULL,
				"Cannot connect to server.\n"
				"\n"
				"Please start the server (double-click StartServer.bat)\n"
				"and make sure port 8787 is listening, then retry.",
				"Tenvi MP", MB_OK | MB_ICONERROR);
			return 0;
		}
		// 等一下让 HELLO 握手完成
		Sleep(500);
	}

	MP_SendLogin(acc, pw);

	BYTE res = 0;
	if (!MP_WaitCtrlResult(MP_CTRL_LOGIN_RESULT, 8000, res)) {
		MessageBoxA(NULL, "Login timeout (server not responding?).",
		            "Tenvi MP", MB_OK | MB_ICONERROR);
		MP_ResetLoginState();
		return 0;
	}
	if (res != 1) {
		MessageBoxA(NULL, "Login failed: wrong password or account not found.",
		            "Tenvi MP", MB_OK | MB_ICONWARNING);
		MP_ResetLoginState();
		return 0;
	}

	// 认证成功: 清空凭据(安全)、放行进游戏
	MP_ClearCred();
	MP_SetAuthed(true);
	MP_StopCapture();  // 停止捕获, 不再需要
	MP_SendCtrl(MP_CTRL_WORLDLIST);
	DEBUG(L"[MP] LoginButton: auth OK, sending worldlist");
	return 0;
}

DWORD (__thiscall *_LoginButton_KR)(void *ecx, void *, void *, void *) = NULL;
DWORD __fastcall LoginButton_KR_Hook(void *ecx, void *, void *, void *) {
	MP_SendCtrl(MP_CTRL_WORLDLIST);
	return 0;
}


void (__thiscall *_WorldSelectButton)(void *) = NULL;
void __fastcall WorldSelectButton_Hook(void *ecx) {
	// [MP] 修复角色列表崩溃: 原版 TenviTest 在 EnterSendPacket_Hook 里同步调
	// FakeServer(cp) 把 0x04/0x05 当场注入(同一帧内完成切屏)。我们的桥接把
	// 假服务端挪到远程 StandaloneServer, 回包变异步, 0x04 迟到时客户端 UI 还
	// 停在世界选择界面 -> 崩。这里改为客户端本地同步注入 0x04(切屏),
	// 0x05 角色数据仍由服务端异步补。
	{ FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a"); if (f) { fprintf(f, "WSB clicked -> sync 0x04 + send CHARLIST\n"); fflush(f); fclose(f); } }
	_WorldSelectButton(ecx);
	// 同步注入 0x04 = CharacterSelectPacket (opcode 04 00 FF FF FF FF 00)
	{
		BYTE sel04[7] = { 0x04, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
		std::vector<BYTE> p04(sel04, sel04 + 7);
		ProcessPacketExec(p04);
	}
	// 服务端回 0x05 真实角色数据(异步)
	MP_SendCtrl(MP_CTRL_CHARLIST);
}

bool (__thiscall *_ConnectCaller)(void *ecx, void *v1, void *v2, void *v3) = NULL;
static int connect_call_count = 0;

bool __fastcall ConnectCaller_Hook(void *ecx, void *edx, void *v1, void *v2, void *v3) {
	connect_call_count++;
	DEBUG(L"Connect is called!");
	// [DIAG] log connect attempt
	{ FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a"); if (f) { fprintf(f, "[CONNECT #%d]\n", connect_call_count); fflush(f); fclose(f); } }
	// [MP v13] Like stock TenviTest: just pretend the connection succeeded.
	// The client creates its own connection object in the post-connect flow, and
	// finalizes it on the first native send (see EnterSendPacket_Hook). Our earlier
	// attempts to plant a fake object pointer broke single-player startup.
	return true;
}


void(__thiscall *_EnterSendPacket)(OutPacket *) = NULL;
void __fastcall EnterSendPacket_Hook(OutPacket *op) {
	// [MP] Bridge to standalone server always (so server can respond with more packets)
	MP_SendGame(op->packet, op->encoded);
	// [DIAG] Log outbound packet
	{
		FILE *f = NULL;
		fopen_s(&f, MP_DiagPath(), "a");
		if (f) {
			BYTE opcode = (op->encoded > 0) ? op->packet[0] : 0xFF;
			fprintf(f, "EnterSendPacket op=%02X len=%lu\n", opcode, (unsigned long)op->encoded);
			fflush(f); fclose(f);
		}
	}
	// [MP] During batch injection, skip the ORIGINAL send so injected packets
	// are not echoed back to the server.
	if (g_mp_in_batch) return;

	// [CRITICAL v13] Call the ORIGINAL native send. This is what makes the client
	// create/initialize its connection object (CWvsContext+0x180). Without it,
	// the per-frame getter at 0x00463972 reads a NULL object and crashes every
	// frame. This matches stock TenviTest single-player behavior.
	_EnterSendPacket(op);
}

void (__thiscall *_ProcessPacketCaller)(void *) = NULL;
static bool g_captureStarted = false;  // [v29] 捕获是否已启动(只启动一次)
void __fastcall ProcessPacketCaller_Hook(void *ecx) {
	_ProcessPacketCaller(ecx);
	// [DIAG] Frame counter
	mp_frame_count++;
	if (mp_frame_count <= 30 || mp_frame_count % 10 == 0) {
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[FRAME %d] ProcessPacketCaller end\n", mp_frame_count); fflush(f); fclose(f); }
	}
	// [MP] Inject server packets into client
	MP_Pump();
	// [v29] 延迟启动 GetAsyncKeyState 键盘捕获: 等第10帧时游戏窗口肯定已创建
	// [v30 FIX] 不再传固定HWND(会在启动过程中过期), 改用进程ID判断前台窗口归属
	if (!g_captureStarted && mp_frame_count >= 10) {
		g_captureStarted = true;
		MP_StartCapture();
		DEBUG(L"[MP] Capture started at frame %d (pid-based)", mp_frame_count);
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[MP-CAP] Started at frame %d (pid=%u)\n", mp_frame_count, GetCurrentProcessId()); fflush(f); fclose(f); }
	}
	// [DIAG] Post-pump: confirm MP_Pump returned safely
	if (mp_frame_count > 2190) {
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[FRAME %d] after MP_Pump OK\n", mp_frame_count); fflush(f); fclose(f); }
	}
	DelayExecution();
	// [DIAG] Post-delay: confirm entire frame completed
	if (mp_frame_count > 2190) {
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[FRAME %d] after DelayExecution OK\n", mp_frame_count); fflush(f); fclose(f); }
	}
}

bool AutoResponseHook() {
	Rosemary r;

	switch (GetRegion()) {
	case TENVI_JP: {
		SetServerPacketHeader_JP_v127();
		SetClientPacketHeader_JP_v127();

		Addr_OnPacketClass = 0x006DB164;
		// press button to go world select
		SHookFunction(LoginButton, 0x0052E43B);
		// world select to go character select
		SHookFunction(WorldSelectButton, 0x0052F038);
		// read send packet buffer without using server
		SHookFunction(EnterSendPacket, 0x0055F2A8);
		// ignore connect checks for world select and character select
		SHookFunction(ConnectCaller, 0x0055EFE2);
		// delay execution test
		SHookFunction(ProcessPacketCaller, 0x0055E926);

		// patch
		// portal id to map id
		//r.Patch(0x0042D3DC + 2, L"18");
		// disable spamming character movement packet
		r.Patch(0x00459649, L"B8 01 00 00 00");

		Addr_OnPacketClass2 = 0x006DB190;
		Addr_OnPacket2 = 0x004C91B4;
		return true;
	}
	case TENVI_CN: {
		SetServerPacketHeader_CN_v126();
		SetClientPacketHeader_CN_v126();

		Addr_OnPacketClass = 0x006FAF44;
		SHookFunction(LoginButton, 0x00532FEF);
		SHookFunction(WorldSelectButton, 0x00533D74);
		SHookFunction(EnterSendPacket, 0x0056AADB);
		SHookFunction(ConnectCaller, 0x0056A4FD);
		SHookFunction(ProcessPacketCaller, 0x0056A579);

		// [FIX v19] Surgical byte patches at the 3 NULL-deref virtual-call sites inside the
		// per-frame network session update function (entry 0x4947F6, epilogue 0x494923).
		// Each site is the SAME pattern (confirmed by disassembly):
		//   8B 01 FF 50 78   mov eax,[ecx] ; call [eax+0x78]   (ecx = connection obj = NULL)
		//   85 C0 74 xx      test eax,eax ; je <skip>          (guards dependent code)
		// ecx comes from `mov ecx,[esi+0x15e8/0x15ec/0x15f0]` -- all NULL because ConnectCaller
		// returns true without initializing them. The virtual call on NULL dereferences and crashes.
		// Fix: replace `mov eax,[ecx]; call [eax+0x78]` (5 bytes) with `xor eax,eax` + 3 NOPs.
		// eax=0 acts as a null sentinel; the following `test eax,eax; je` skips dependent calls.
		// No exception handling (VEH) needed -- deterministic and safe.
		r.Patch(0x00494845, L"33 C0 90 90 90");
		r.Patch(0x00494898, L"33 C0 90 90 90");
		r.Patch(0x00494901, L"33 C0 90 90 90");

		// [v54d→v54g] 互见修复演进:
		//   0x48DD03: je 0x48dd4c -- 0x11 spawn 里"非本地角色"跳过渲染 → NOP
		//   [v54g] 撤销 v54d/v54f 对 0x498EA1 和 0x498ECE 的 patch!
		//   实测(v54b, 无任何patch)证明: 后进图者用原始代码就能渲染先进图者,
		//   0x45adeb 不是销毁对象而是设置归属字段. 我之前的 patch 反而破坏了
		//   "后进图者→先进图者"方向. 只保留 0x48DD03 的 NOP.
		r.Patch(0x0048DD03, L"90 90");   // 0x11 handler: 非本地角色也渲染

		Addr_OnPacketClass2 = 0x006FAF70;
		Addr_OnPacket2 = 0x004CBE34;

		// portal id to map id
		//r.Patch(0x0042D569 + 0x02, L"18");
		return true;
	}
	case TENVI_HK: {
		SetServerPacketHeader_HK_v102();
		SetClientPacketHeader_HK_v102();

		Addr_OnPacketClass = 0x0075CF84;
		SHookFunction(LoginButton, 0x0052CFC2);
		SHookFunction(WorldSelectButton, 0x0052DC5A);
		SHookFunction(EnterSendPacket, 0x005AC927);
		SHookFunction(ConnectCaller, 0x005832FE);
		SHookFunction(ProcessPacketCaller, 0x005838C0);

		Addr_OnPacketClass2 = 0x0075CFAC;
		Addr_OnPacket2 = 0x004BB0A5;

		// portal id to map id
		r.Patch(0x0041048F + 0x02, L"18");
		return true;
	}
	case TENVI_KRX: {
		SetServerPacketHeader_KRX_v107();
		SetClientPacketHeader_KRX_v107();

		Addr_OnPacketClass = 0x0075E184;
		SHookFunction(LoginButton_KR, 0x004767A3);
		SHookFunction(WorldSelectButton, 0x00540E22);
		SHookFunction(EnterSendPacket, 0x005CBA0F);
		SHookFunction(ConnectCaller, 0x0059DED0);
		SHookFunction(ProcessPacketCaller, 0x0059E328);

		Addr_OnPacketClass2 = 0x0075E1AC;
		Addr_OnPacket2 = 0x004D017C;

		// portal id to map id
		//r.Patch(0x0042429E + 0x02, L"18");
		return true;
	}
	case TENVI_KR: {
		SetServerPacketHeader_KR_v200();
		SetClientPacketHeader_KR_v200();

		Addr_OnPacketClass = 0x00731764;
		//SHookFunction(LoginButton_KR, 0x004013C8);
		SHookFunction(WorldSelectButton, 0x0051CD40);
		SHookFunction(EnterSendPacket, 0x00593F4B);
		SHookFunction(ConnectCaller, 0x00566AB8);
		SHookFunction(ProcessPacketCaller, 0x00566EF8);

		Addr_OnPacketClass2 = 0x0073178C;
		Addr_OnPacket2 = 0x004B202F;

		// portal id to map id
		//r.Patch(0x00410513 + 0x02, L"18");
		return true;
	}
	default: {
		break;
	}
	}

	return false;
}
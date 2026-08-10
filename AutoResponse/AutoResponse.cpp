#include"AutoResponse.h"
#include"MPClient.h"
#include <map>
#include <vector>
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
static const char* MP_INTERP_TAG = "MP_DLL_V87_FITPROBE";

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

// [v86] Coordinate-offset probe state.
//
// WHY THE v83/v85 PROBE NEVER RAN: it lived INSIDE the batch loop, before
// ProcessPacketExec. During a v84 move sequence (0x12 remove -> 0x3D -> 0x11
// spawn) the object is DELETED at that moment, so GetCharacterByOID returned
// NULL and the whole probe block was skipped. Not one [MP-PROBE] line was ever
// written. The probe now runs AFTER the batch has been injected, when the
// object provably exists again.
//
// Method: multi-round cross-validation against a KNOWN value. The 0x11 packet
// carries the authoritative x, and horizontal position is never snapped by the
// client (only y is pulled onto the platform foothold). So we keep a candidate
// set of offsets whose float matches the packet x, and intersect it every
// round. An offset that survives PROBE_LOCK_ROUNDS distinct positions is the
// real x field - a coincidental match cannot track a moving value.
static bool g_probe_locked = false;
static int  g_probe_round = 0;
static float g_probe_last_x = 1.0e9f;     // last probed target x (dedupe)
static const int   PROBE_SCAN_RANGE  = 0x2000;
static const int   PROBE_LOCK_ROUNDS = 3;

// Linear-fit probe buffers. We no longer require the in-memory value to EQUAL
// the packet coordinate - that assumption broke on every client that stores
// coords as int pixels, or in a shifted/scaled frame. Instead we collect
// (target, mem-as-float, mem-as-int32) at each distinct position and later fit
// mem ~ slope*target + intercept. The real coordinate field is the one whose
// stored value tracks the packet coordinate with slope ~1, regardless of a
// fixed offset or 1:1 scale.
struct ProbeSample { float t; float memf; int memi; };
static std::map<int, std::vector<ProbeSample>> g_probe_xsamp;
static std::map<int, std::vector<ProbeSample>> g_probe_ysamp;

// Locked coordinate layout (runtime only - NOT persisted; the probe re-runs
// each launch so a stale type/scale can never silently corrupt the object).
static bool  g_interp_x_is_int = false;
static float g_interp_x_scale = 1.0f;   // client_val = t*scale + intc
static float g_interp_x_intc  = 0.0f;
static bool  g_interp_y_is_int = false;
static float g_interp_y_scale = 1.0f;
static float g_interp_y_intc  = 0.0f;

// [v86] Suppress-respawn state: once offsets are locked we stop letting the
// despawn/respawn packets touch the client and drive movement by memory write.
static bool g_suppress_active = false;
static int  g_suppress_count = 0;

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
		else if (strncmp(line, "speed=", 6) == 0) {
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

// [v87] Persist only interp/speed. Offsets are intentionally NOT persisted:
// they are discovered live each launch, so a stale type/scale can never
// silently corrupt the remote avatar object on the next run.
static void MP_SaveInterpCfg() {
	FILE *f = NULL;
	fopen_s(&f, "mp_interp.cfg", "w");
	if (!f) return;
	fprintf(f, "interp=1\n");
	fprintf(f, "speed=%.3f\n", g_interp_speed);
	fclose(f);
}

// [v87] Locate the x/y coordinate fields inside a remote CCharacter.
//
// Called AFTER the batch has been injected (object provably exists then).
// The v86 approach required the in-memory float to EXACTLY equal the packet
// coordinate - which fails whenever the client stores coords as int pixels or
// in a shifted/scaled frame. v87 instead collects (target, mem-as-float,
// mem-as-int32) at several distinct positions and fits mem ~ slope*target +
// intercept. The real field is the one that tracks the packet coordinate with
// slope ~1, no matter the storage type or a fixed offset/scale.

// Least-squares fit of mem ~ slope*t + intercept, testing both float-stored and
// int32-stored dependent values. Returns the better channel if it tracks the
// target with slope ~1 within resid_thresh.
struct ProbeFit { bool ok; bool is_int; float slope; float intc; float resid; };
static ProbeFit MP_FitProbeChannel(const std::vector<ProbeSample>& s, float resid_thresh) {
	ProbeFit best; best.ok = false; best.resid = 1.0e9f;
	if (s.size() < 2) return best;
	float n = (float)s.size();
	// float channel
	{
		float st = 0, sy = 0, stt = 0, sty = 0;
		for (size_t i = 0; i < s.size(); i++) { st += s[i].t; sy += s[i].memf; stt += s[i].t*s[i].t; sty += s[i].t*s[i].memf; }
		float den = n*stt - st*st;
		if (fabsf(den) > 1e-3f) {
			float slope = (n*sty - st*sy) / den;
			float intc = (sy - slope*st) / n;
			float rmax = 0;
			for (size_t i = 0; i < s.size(); i++) { float e = fabsf(s[i].memf - (slope*s[i].t + intc)); if (e > rmax) rmax = e; }
			if (fabsf(slope - 1.0f) < 0.35f && rmax < resid_thresh && rmax < best.resid) {
				best.ok = true; best.is_int = false; best.slope = slope; best.intc = intc; best.resid = rmax;
			}
		}
	}
	// int channel
	{
		float st = 0, sy = 0, stt = 0, sty = 0;
		for (size_t i = 0; i < s.size(); i++) { st += s[i].t; sy += (float)s[i].memi; stt += s[i].t*s[i].t; sty += s[i].t*(float)s[i].memi; }
		float den = n*stt - st*st;
		if (fabsf(den) > 1e-3f) {
			float slope = (n*sty - st*sy) / den;
			float intc = (sy - slope*st) / n;
			float rmax = 0;
			for (size_t i = 0; i < s.size(); i++) { float e = fabsf((float)s[i].memi - (slope*s[i].t + intc)); if (e > rmax) rmax = e; }
			if (fabsf(slope - 1.0f) < 0.35f && rmax < resid_thresh && rmax < best.resid) {
				best.ok = true; best.is_int = true; best.slope = slope; best.intc = intc; best.resid = rmax;
			}
		}
	}
	return best;
}

// [v87] Type/scale-aware coordinate read/write for the lerp pass.
static float MP_ReadCoord(DWORD base, int off, bool is_int) {
	if (off == 0) return 0.0f;
	if (is_int) { int iv = 0; memcpy(&iv, (void*)(base + off), 4); return (float)iv; }
	float fv = 0; memcpy(&fv, (void*)(base + off), 4); return fv;
}
static void MP_WriteCoord(DWORD base, int off, bool is_int, float client_val) {
	if (off == 0) return;
	if (is_int) { int iv = (int)roundf(client_val); memcpy((void*)(base + off), &iv, 4); }
	else { memcpy((void*)(base + off), &client_val, 4); }
}
static void MP_ProbeCoordOffsets() {
	if (g_probe_locked || g_interp.empty()) return;
	DWORD cfield = MP_GetCFieldPtr();
	if (!cfield || !_GetCharacterByOID) return;

	DWORD oid = 0, ptr = 0;
	float tx = 0.0f, ty = 0.0f;
	for (std::map<DWORD, RemoteInterp>::iterator it = g_interp.begin(); it != g_interp.end(); ++it) {
		DWORD p = _GetCharacterByOID((void*)cfield, it->first);
		if (p) { oid = it->first; ptr = p; tx = it->second.tx; ty = it->second.ty; break; }
	}
	if (!ptr) return;

	// Require real displacement between rounds - resampling the same spot
	// proves nothing about which field is tracking the position.
	if (fabsf(tx - g_probe_last_x) < 8.0f) return;
	g_probe_last_x = tx;
	g_probe_round++;

	// Collect (target, mem-as-float, mem-as-int32) for every 4-byte slot.
	for (int off = 0; off < PROBE_SCAN_RANGE; off += 4) {
		ProbeSample sx; sx.t = tx;
		memcpy(&sx.memf, (void*)(ptr + off), 4);
		memcpy(&sx.memi, (void*)(ptr + off), 4);
		if (!isfinite(sx.memf)) continue;   // float channel skips NaN/Inf
		g_probe_xsamp[off].push_back(sx);
		ProbeSample sy; sy.t = ty; sy.memf = sx.memf; sy.memi = sx.memi;
		g_probe_ysamp[off].push_back(sy);
	}

	// Diagnostics: print the best-fitting x candidates while probing.
	if (g_probe_round <= 8) {
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) {
			fprintf(f, "[MP-PROBE v87] round=%d oid=%08X ptr=%08X target=(%.1f,%.1f) slots=%d\n",
				g_probe_round, oid, ptr, tx, ty, (int)g_probe_xsamp.size());
			int shown = 0;
			for (std::map<int, std::vector<ProbeSample>>::iterator c = g_probe_xsamp.begin();
				c != g_probe_xsamp.end() && shown < 4; ++c) {
				ProbeFit fit = MP_FitProbeChannel(c->second, 4.0f);
				if (fit.ok) {
					fprintf(f, "    x cand +0x%04X %s slope=%.3f intc=%.1f resid=%.2f\n",
						c->first, fit.is_int ? "int" : "flt", fit.slope, fit.intc, fit.resid);
					shown++;
				}
			}
			fflush(f); fclose(f);
		}
	}

	if (g_probe_round < PROBE_LOCK_ROUNDS) return;

	// Lock x (tight residual) then y (loose: client snaps y to foothold).
	ProbeFit best_x; best_x.ok = false; best_x.resid = 1.0e9f; int x_off = 0;
	for (std::map<int, std::vector<ProbeSample>>::iterator c = g_probe_xsamp.begin(); c != g_probe_xsamp.end(); ++c) {
		ProbeFit fit = MP_FitProbeChannel(c->second, 4.0f);
		if (fit.ok && fit.resid < best_x.resid) { best_x = fit; x_off = c->first; }
	}
	ProbeFit best_y; best_y.ok = false; best_y.resid = 1.0e9f; int y_off = 0;
	for (std::map<int, std::vector<ProbeSample>>::iterator c = g_probe_ysamp.begin(); c != g_probe_ysamp.end(); ++c) {
		ProbeFit fit = MP_FitProbeChannel(c->second, 40.0f);
		if (fit.ok && fit.resid < best_y.resid) { best_y = fit; y_off = c->first; }
	}

	if (!best_x.ok) {
		// Could not lock x - restart probing (clears stale samples).
		g_probe_round = 0; g_probe_last_x = 1.0e9f;
		g_probe_xsamp.clear(); g_probe_ysamp.clear();
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[MP-PROBE v87] x LOCK FAILED at round %d - see candidates above\n", PROBE_LOCK_ROUNDS); fflush(f); fclose(f); }
		return;
	}

	g_interp_x_off = x_off;
	g_interp_y_off = y_off;   // 0 is valid: x-only writes still kill the flicker
	g_interp_x_is_int = best_x.is_int; g_interp_x_scale = best_x.slope; g_interp_x_intc = best_x.intc;
	g_interp_y_is_int = best_y.ok ? best_y.is_int : false;
	g_interp_y_scale = best_y.ok ? best_y.slope : 1.0f;
	g_interp_y_intc  = best_y.ok ? best_y.intc  : 0.0f;
	g_probe_locked = true;

	f = NULL; fopen_s(&f, MP_DiagPath(), "a");
	if (f) {
		fprintf(f, "[MP-PROBE v87] *** LOCKED x_off=0x%X(%s s=%.3f i=%.1f) y_off=0x%X(%s s=%.3f i=%.1f) ***\n",
			x_off, best_x.is_int ? "int" : "flt", best_x.slope, best_x.intc,
			y_off, best_y.ok ? (best_y.is_int ? "int" : "flt") : "none", best_y.ok ? best_y.slope : 0, best_y.ok ? best_y.intc : 0);
		fflush(f); fclose(f);
	}
}

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

	// [v86] FLICKER ROOT CAUSE + FIX.
	//
	// The server moves a remote player by despawn-respawn:
	//     0x12 (remove) -> 0x3D (account data) -> 0x11 (spawn at new pos) -> 0x3D
	// Even though all four land in the SAME frame, the client genuinely destroys
	// and recreates the CCharacter every single step: sprites are reloaded and
	// the animation state resets to frame 0. That is the flicker. No amount of
	// interpolation, tick-rate or buffering tuning can fix it, because the object
	// being interpolated is thrown away several times per second.
	//
	// Once the coordinate offsets are known we do not need the rebuild at all:
	// we swallow 0x12 / the rebuild 0x3D / 0x11 and let the per-frame lerp below
	// write the new position straight into the surviving object. The avatar is
	// never destroyed, so it never flickers, and it slides instead of teleporting.
	//
	// The trailing 0x3D restores the RECEIVING player's own account data and is
	// always forwarded. If offsets are not locked yet, nothing is suppressed and
	// behaviour is byte-for-byte identical to v85.
	std::vector<char> mp_suppress(batch.size(), 0);
	if (g_probe_locked && g_interp_x_off != 0) {
		DWORD cf = MP_GetCFieldPtr();
		for (size_t i = 0; i + 1 < batch.size(); i++) {
			if (batch[i].first.size() < 5 || batch[i].first[0] != 0x12) continue;
			DWORD roid = *(DWORD*)&batch[i].first[1];
			if (roid == 0 || g_interp.find(roid) == g_interp.end()) continue;
			// Only suppress when the object is alive right now - otherwise we
			// would swallow the rebuild of an avatar that is genuinely gone.
			DWORD live = (cf && _GetCharacterByOID) ? _GetCharacterByOID((void*)cf, roid) : 0;
			if (!live) continue;
			int j = -1;
			for (size_t k = i + 1; k < batch.size() && k <= i + 3; k++) {
				if (batch[k].first.size() >= 5 && batch[k].first[0] == 0x11 &&
					*(DWORD*)&batch[k].first[1] == roid) { j = (int)k; break; }
			}
			if (j < 0) continue;   // lone 0x12 = real departure, let it through
			mp_suppress[i] = 1;
			mp_suppress[j] = 1;
			for (int k = (int)i + 1; k < j; k++)
				if (!batch[k].first.empty() && batch[k].first[0] == 0x3D) mp_suppress[k] = 1;
			g_suppress_active = true;
			g_suppress_count++;
			if (g_suppress_count <= 5 || (g_suppress_count % 50) == 0) {
				FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
				if (f) {
					fprintf(f, "[MP-SUPPRESS v86] respawn swallowed #%d oid=%08X ptr=%08X -> memory-write move\n",
						g_suppress_count, roid, live);
					fflush(f); fclose(f);
				}
			}
		}
	}

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
		// [v86] The old in-loop probe was removed: it ran BEFORE injection,
		// when the 0x12 had already deleted the object, so GetCharacterByOID
		// always returned NULL and it never executed once. The real probe now
		// runs after the batch is injected (see MP_ProbeCoordOffsets below).
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
		// [v85] ROOT CAUSE of the v84 "we cannot see each other" regression.
		// v84 moves a remote player with a despawn-respawn sequence:
		//     0x12 (remove object) -> 0x3D (account data) -> 0x11 (spawn at new pos)
		// The v83 skip test below consulted the g_char_by_oid CACHE, and 0x12
		// never cleared that cache. So the 0x12 really did delete the remote
		// avatar, then the 0x11 that was supposed to rebuild it was silently
		// dropped ("already exists") - the avatar disappeared permanently the
		// moment either player moved. Two fixes:
		//   (a) 0x12 evicts the oid from the cache (the pointer is dead anyway).
		//   (b) The skip test queries CField live instead of trusting the cache,
		//       so we only skip when the object genuinely still exists.
		// [v86] Only evict when the 0x12 is actually delivered. A suppressed
		// 0x12 never reaches the client, so the object - and its pointer -
		// stay valid and must remain cached.
		if (mp_op == 0x12 && oid != 0 && !mp_suppress[i]) {
			g_char_by_oid.erase(oid);
			FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
			if (f) {
				fprintf(f, "[MP-RESPAWN MP_DLL_V85_RESPAWN_CACHE_FIX] op=12 oid=%08X cache evicted\n", oid);
				fflush(f); fclose(f);
			}
		}
		bool mp_skip_inject = false;
		if (mp_op == 0x11 && oid != g_localObjectId && !mp_suppress[i]) {
			DWORD cfield_chk = MP_GetCFieldPtr();
			DWORD live_ptr = (cfield_chk && _GetCharacterByOID)
				? _GetCharacterByOID((void*)cfield_chk, oid) : 0;
			if (live_ptr != 0) {
				g_char_by_oid[oid] = live_ptr;
				mp_skip_inject = true;
			} else {
				g_char_by_oid.erase(oid);
			}
			FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
			if (f) {
				fprintf(f, "[MP-RESPAWN MP_DLL_V85_RESPAWN_CACHE_FIX] op=11 oid=%08X live=%08X skip=%d\n",
					oid, live_ptr, mp_skip_inject ? 1 : 0);
				fflush(f); fclose(f);
			}
		}
		if (!mp_skip_inject && !mp_suppress[i]) {
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
	// [v86] Probe AFTER injection - this is the whole point. At this moment the
	// 0x11 has rebuilt the CCharacter, so GetCharacterByOID actually resolves.
	// Cheap no-op once locked.
	if (MP_InterpEnabled()) MP_ProbeCoordOffsets();
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
			// [v85] Also EVICT entries whose object no longer exists. The old
			// code only ever wrote non-null pointers, so a removed character
			// left a dangling pointer in the cache forever - which is what let
			// the v83 skip test believe a despawned avatar was still alive.
			std::vector<DWORD> dead_oids;
			for (auto &kv : g_interp) {
				DWORD ptr = _GetCharacterByOID((void*)cfield, kv.first);
				if (ptr) g_char_by_oid[kv.first] = ptr;
				else dead_oids.push_back(kv.first);
			}
			for (size_t d = 0; d < dead_oids.size(); d++)
				g_char_by_oid.erase(dead_oids[d]);
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
			// Read current client-space coords (type/scale aware).
			float cur_cx = MP_ReadCoord(char_ptr, g_interp_x_off, g_interp_x_is_int);
			float cur_cy = MP_ReadCoord(char_ptr, g_interp_y_off, g_interp_y_is_int);
			// Inverse to packet space, lerp, forward back to client space.
			float cur_px = (g_interp_x_scale != 0.0f) ? (cur_cx - g_interp_x_intc) / g_interp_x_scale : cur_cx;
			float nxt_px = cur_px + (ri.tx - cur_px) * g_interp_speed;
			if (fabsf(nxt_px - ri.tx) < 0.5f) nxt_px = ri.tx;
			float nxt_cx = (g_interp_x_scale != 0.0f) ? (nxt_px * g_interp_x_scale + g_interp_x_intc) : nxt_px;
			MP_WriteCoord(char_ptr, g_interp_x_off, g_interp_x_is_int, nxt_cx);
			ri.cx = nxt_cx;

			float cur_py = (g_interp_y_scale != 0.0f) ? (cur_cy - g_interp_y_intc) / g_interp_y_scale : cur_cy;
			float nxt_py = cur_py + (ri.ty - cur_py) * g_interp_speed;
			if (fabsf(nxt_py - ri.ty) < 0.5f) nxt_py = ri.ty;
			float nxt_cy = (g_interp_y_scale != 0.0f) ? (nxt_py * g_interp_y_scale + g_interp_y_intc) : nxt_py;
			MP_WriteCoord(char_ptr, g_interp_y_off, g_interp_y_is_int, nxt_cy);
			ri.cy = nxt_cy;
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
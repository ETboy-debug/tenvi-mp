#include"AutoResponse.h"
#include"MPClient.h"
#include <map>
#include <set>
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
static const char* MP_INTERP_TAG = "MP_DLL_V171_MODE_LIVEPROBE";

struct RemoteInterp {
	DWORD oid;
	float tx, ty;   // target (last server coord from 0x11)
	float cx, cy;   // current client coord - filled once offsets known
	int   stale;    // frames since last 0x11
};

static std::map<DWORD, RemoteInterp> g_interp;
static std::map<DWORD, bool> g_spawned;   // [v135] remote oid already created
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
// [v155] speed 0.15 -> 1.0: V154 user shows residual/afterimage and flicker
// because DLL writes the coord slowly (speed 0.15) while the client ALSO
// updates the object internally each frame; the two writers desync -> the
// rendered position keeps trailing the DLL-written position (afterimage),
// and the rebuild staircase that still slips through produces a per-frame
// flicker. Speed 1.0 means "snap to target this frame" -> no lerp lag ->
// afterimage goes away. Suppression already runs (locked=1) so rebuild
// staircases to that oid are swallowed; the flicker is from the client
// rendering the snap-to position before the next animation tick settles.
static float g_interp_speed = 1.0f;

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
// [v136] Absolute addresses for the memory-write lerp (set by mp_interp.cfg
// mem_x=/mem_y=/mem_lock=1, or found by MP_MemScanForPair). Declared here so
// the cfg parser in MP_InterpEnabled (line ~125) can reference them.
static DWORD  g_mem_x_addr = 0, g_mem_y_addr = 0;
static bool   g_mem_locked = false;
// [v137] remove-oid -> tick when the last 0x12 for that oid was seen.
// A 0x11 for the same oid within 500ms is a rebuild pair (swallow it, the
// memory-write lerp owns the position). No self/peer identity guessing.
static std::map<DWORD, DWORD> g_remove_tick;
// [v137] addresses that the sanity check rejected (writes don't move the
// avatar) are blacklisted so MP_MemScanForPair stops re-locking them.
static std::set<DWORD> g_mem_bad;
static int  g_probe_round = 0;
static float g_probe_last_x = 1.0e9f;     // last probed target x (dedupe)
static float g_probe_last_y = 1.0e9f;     // last probed target y (dedupe)
static const int   PROBE_SCAN_RANGE  = 0x2000;
static const int   PROBE_LOCK_ROUNDS = 3;
// [v148] coordinate-PAIR votes: key = (x_off<<16)|y_off, value = agreeing
// rounds. Rebuild recreates the avatar object every ~0.35s, so rounds sample
// different objects; a (x,y) float pair found in 2+ DIFFERENT objects proves
// the struct layout offsets, which are fixed per client build.
// [v152] value = {count, last_vx}: also require the field VALUE to move
// between agreeing rounds (real coord tracks the walking avatar; a constant
// zero field stays 0). Rejects the V149 fake pair without narrowing the
// search window back down.
struct ProbeVote { int count; float last_vx; };
static std::map<unsigned long long, ProbeVote> g_probe_pairs;

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
static bool g_scan = false;   // [v168] dump remote char memory for offset discovery
// [v171] Suppression strategy selector. The rebuild staircase is
//   0x12(remove) -> 0x3D(mover account data) -> 0x11(spawn) -> 0x3D(receiver)
// mode=0 : swallow 0x12 + middle 0x3D + 0x11   (v138 behaviour, needs lerp)
// mode=1 : swallow ONLY 0x12                   (avatar is never destroyed, the
//          0x3D+0x11 still reach the client -> the client's own 0x11 handler
//          takes the "update existing avatar" branch = native smooth, no
//          memory write and no offset guessing required)
// mode=2 : swallow 0x12 + middle 0x3D, let 0x11 through
static int  g_sup_mode = 0;
// [v171] Extra mirror offsets written alongside x_off/y_off (probe found
// 0x11A4/0x11A8 and mirrors 0x17A4/0x17A8 holding the same coordinate).
static int  g_interp_x_off2 = 0;
static int  g_interp_y_off2 = 0;
// [v171] Live-field auto-probe (scan=3): poke a value into each candidate
// offset, read it back one frame later. A field the client rewrites every
// frame (render/physics state) reverts; a dead cache keeps the poked value.
// This identifies the render coordinate without any user eyeballing.
static int  g_live_probe_cursor = 0;
static bool g_live_probe_done = false;
static int  g_scan_kind = 0;  // [v171] value of scan= (1=match probe, 3=live probe)

// [v169] SEH-guarded 4-byte read. The wide probe walks up to 8KB past the
// character pointer, which can cross the end of the allocation (or hit an
// object that was freed between frames). Keep this in its own function -
// __try/__except cannot coexist with C++ object unwinding in one scope.
// [v171] SEH-guarded 4-byte write. The live-field probe restores the original
// value one frame after poking it; by then the avatar object may already have
// been freed by the rebuild staircase, so an unguarded write would crash the
// client.
static bool MP_SafeWrite4(DWORD addr, DWORD val) {
	__try {
		*(DWORD*)addr = val;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}
static bool MP_SafeRead4(DWORD addr, DWORD *out) {
	__try {
		*out = *(DWORD*)addr;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// [v197] full-process render-position hunt candidate (see scan=4 block)
struct MP_ScanCand {
	DWORD addr;
	bool is_int;
};

// [v86] Suppress-respawn state: once offsets are locked we stop letting the
// despawn/respawn packets touch the client and drive movement by memory write.
static bool g_suppress_active = false;
static int  g_suppress_count = 0;

// [v93] Interpolation is now required for movement: bare 0x11 updates for an
// existing remote object are ignored by the client, so the DLL must swallow
// them and write coords itself. The cfg is still read for speed tuning and
// can explicitly disable with "interp=0", but default is ON.
static bool MP_InterpEnabled() {
	FILE* f = NULL;
	fopen_s(&f, "D:/mp_interp.cfg", "r");
	if (!f) return false;   // [v168] missing cfg => interp OFF (safe: native drive)
	char line[128];
	bool on = true;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "interp=0", 8) == 0) on = false;
		else if (strncmp(line, "interp=1", 8) == 0) on = true;
		else if (strncmp(line, "speed=", 6) == 0) {
			g_interp_speed = (float)atof(line + 6);
		}
		else if (strncmp(line, "x_off=", 6) == 0) {
			g_interp_x_off = (int)strtol(line + 6, NULL, 0);
		}
		else if (strncmp(line, "y_off=", 6) == 0) {
			g_interp_y_off = (int)strtol(line + 6, NULL, 0);
		}
		// [v131] locked=1 forces the suppress+lerp path without running the
		// probe (which never locks on this client: 0x11 spawns land in the
		// CField hashmap unreliably). Offsets come from scan_coords.py.
		else if (strncmp(line, "locked=", 7) == 0) {
			if (strtol(line + 7, NULL, 0) != 0) g_probe_locked = true;
		}
		else if (strncmp(line, "scan=", 5) == 0) {
			long sv = strtol(line + 5, NULL, 0);
			if (sv != 0) g_scan = true;
			g_scan_kind = (int)sv;          // [v171] 1=match probe, 3=live-field probe
		}
		// [v171] suppression strategy (see g_sup_mode comment)
		else if (strncmp(line, "mode=", 5) == 0) {
			g_sup_mode = (int)strtol(line + 5, NULL, 0);
		}
		// [v171] mirror coordinate offsets written together with x_off/y_off
		else if (strncmp(line, "x_off2=", 7) == 0) {
			g_interp_x_off2 = (int)strtol(line + 7, NULL, 0);
		}
		else if (strncmp(line, "y_off2=", 7) == 0) {
			g_interp_y_off2 = (int)strtol(line + 7, NULL, 0);
		}
		// [v136] mem_x=/mem_y= hard-code the absolute addresses found by
		// scan_coords.py (heap, not the exe static segment the old in-DLL
		// scan kept locking). Locking a static address wrote nothing that
		// moves the avatar, so the sanity check unlocked and re-locked in a
		// loop (samples kept climbing) while rebuild flicker continued.
		else if (strncmp(line, "mem_x=", 6) == 0) {
			g_mem_x_addr = (DWORD)strtoul(line + 6, NULL, 0);
		}
		else if (strncmp(line, "mem_y=", 6) == 0) {
			g_mem_y_addr = (DWORD)strtoul(line + 6, NULL, 0);
		}
		else if (strncmp(line, "mem_lock=1", 10) == 0) {
			g_mem_locked = true;
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

// [MP] 
// ,  ProcessPacketCaller 
// [FIX v3] 
// 
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
	// [v146] STRICT: x/y coords are stored as float with slope==1.0, intc==0.
	// The old +/-0.35 slope gate + int channel let the probe lock garbage
	// (v87 locked x_off=0x4: the float bit-pattern read as int32 accidentally
	// tracked tx with slope 1.03 -> writing it moved nothing / risked crash).
	// Only a float slot with slope ~1.0 and intc ~0 can be the coordinate.
	{
		float st = 0, sy = 0, stt = 0, sty = 0;
		for (size_t i = 0; i < s.size(); i++) { st += s[i].t; sy += s[i].memf; stt += s[i].t*s[i].t; sty += s[i].t*s[i].memf; }
		float den = n*stt - st*st;
		if (fabsf(den) > 1e-3f) {
			float slope = (n*sty - st*sy) / den;
			float intc = (sy - slope*st) / n;
			float rmax = 0;
			for (size_t i = 0; i < s.size(); i++) { float e = fabsf(s[i].memf - (slope*s[i].t + intc)); if (e > rmax) rmax = e; }
			if (fabsf(slope - 1.0f) < 0.05f && fabsf(intc) < 500.0f && rmax < resid_thresh && rmax < best.resid) {
				best.ok = true; best.is_int = false; best.slope = slope; best.intc = intc; best.resid = rmax;
			}
		}
	}
	// int channel DISABLED [v146]: coordinate floats read as int32 produce
	// bogus near-linear fits (bit patterns). Real coords are float only.
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
// [v133] Memory-scan coordinate finder. _GetCharacterByOID (CField hashmap)
// is unreliable on this client (V124 rule: 0x11 spawns land there only
// sometimes), so probing offsets inside the object always failed. Instead we
// scan OUR OWN process memory for a float equal to the last 0x11 target x
// with a float equal to target y within +-64 bytes. Once the absolute
// addresses survive two distinct coordinate samples we lock them and lerp
// directly into them each frame (suppression keeps the avatar alive, so the
// addresses stay valid).
static std::map<unsigned long long, int> g_mem_cand;
static float  g_mem_last_tx = 1.0e9f, g_mem_last_ty = 1.0e9f;

static bool MP_MemScanForPair(float tx, float ty, DWORD &out_x, DWORD &out_y) {
	MEMORY_BASIC_INFORMATION mbi;
	unsigned char *base = (unsigned char*)0x00010000;
	while ((DWORD)base < 0x7FFF0000) {
		if (!VirtualQuery(base, &mbi, sizeof(mbi))) break;
		DWORD sz = (DWORD)mbi.RegionSize;
		if (sz && mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
			(mbi.Protect & 0xFF) && !(mbi.Protect & PAGE_GUARD)) {
			unsigned char *p = base;
			unsigned char *end = base + sz;
			for (; p + 4 <= end; p += 4) {
				// [v137] skip blacklisted (sanity-rejected) addresses
				if (!g_mem_bad.empty() && g_mem_bad.count((DWORD)p)) { p += 4; continue; }
				float fx = *(float*)p;
				if (fabsf(fx - tx) <= 2.0f) {
					unsigned char *q0 = p - 64; if (q0 < base) q0 = base;
					unsigned char *q1 = p + 64; if (q1 > end - 4) q1 = end - 4;
					for (unsigned char *q = q0; q <= q1; q += 4) {
						float fy = *(float*)q;
						if (fabsf(fy - ty) <= 2.0f) {
							out_x = (DWORD)p; out_y = (DWORD)q;
							return true;
						}
					}
				}
			}
		}
		base += sz ? sz : 0x1000;
	}
	return false;
}

static void MP_MemSearchCoords() {
	if (g_mem_locked || g_interp.empty()) return;
	float tx = 0.0f, ty = 0.0f;
	{
		std::map<DWORD, RemoteInterp>::iterator it = g_interp.begin();
		if (it == g_interp.end()) return;
		tx = it->second.tx; ty = it->second.ty;
	}
	float dx = tx - g_mem_last_tx, dy = ty - g_mem_last_ty;
	if (sqrtf(dx * dx + dy * dy) < 30.0f) return;   // need displacement
	g_mem_last_tx = tx; g_mem_last_ty = ty;
	DWORD xa = 0, ya = 0;
	if (!MP_MemScanForPair(tx, ty, xa, ya)) {
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[MP-MEM v133] no pair for (%.1f,%.1f)\n", tx, ty); fflush(f); fclose(f); }
		return;
	}
	unsigned long long key = ((unsigned long long)xa << 32) | ya;
	g_mem_cand[key]++;
	if (g_mem_cand.size() > 64) g_mem_cand.clear();
	FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
	if (f) { fprintf(f, "[MP-MEM v133] cand x=0x%08X y=0x%08X for (%.1f,%.1f)\n", xa, ya, tx, ty); fflush(f); fclose(f); }
	for (std::map<unsigned long long, int>::iterator c = g_mem_cand.begin(); c != g_mem_cand.end(); ++c) {
		if (c->second >= 2) {
			g_mem_x_addr = (DWORD)(c->first >> 32);
			g_mem_y_addr = (DWORD)(c->first & 0xFFFFFFFF);
			g_mem_locked = true;
			FILE *ff = NULL; fopen_s(&ff, MP_DiagPath(), "a");
			if (ff) { fprintf(ff, "[MP-MEM v133] *** LOCKED x=0x%08X y=0x%08X samples=%d ***\n", g_mem_x_addr, g_mem_y_addr, c->second); fflush(ff); fclose(ff); }
			return;
		}
	}
}

static void MP_ProbeCoordOffsets() {
	if (g_probe_locked || g_interp.empty()) return;
	DWORD cfield = MP_GetCFieldPtr();
	if (!cfield || !_GetCharacterByOID) return;

	DWORD oid = 0, ptr = 0;
	float tx = 0.0f, ty = 0.0f;
	// [v132] Sample ONLY 2..8 frames after a 0x11 spawn. At that moment the
	// client has created the avatar (so GetCharacterByOID resolves) and the
	// next rebuild that would destroy it is still ~20 frames away. Sampling
	// outside this window read a dead or not-yet-registered object, which is
	// why every PROBE round saw a different ptr and the linear fit failed.
	// [v147] DROP the window: CField hashmap registration is ASYNC (V124
	// disassembly proof) so the 2..8 frame window often has NO entry yet ->
	// GetCharacterByOID returns 0 -> probe silently returns forever (V146
	// never printed a single MP-PROBE line). Sample ANY frame where the
	// lookup resolves; the displacement gate below still requires >=8px of
	// travel between rounds, and the strict fit (v146: float-only, slope
	// +/-0.05, intc < 500) rejects garbage offsets.
	{
		std::map<DWORD, RemoteInterp>::iterator best = g_interp.end();
		for (std::map<DWORD, RemoteInterp>::iterator it = g_interp.begin();
			it != g_interp.end(); ++it) {
			DWORD p = _GetCharacterByOID((void*)cfield, it->first);
			if (!p) continue;
			best = it;
			ptr = p;
			break;
		}
		if (best == g_interp.end() || !ptr) return;
		oid = best->first;
		tx = best->second.tx;
		ty = best->second.ty;
	}

	// Require real displacement between rounds - resampling the same spot
	// proves nothing about which field is tracking the position.
	// [v93] Use 2-D displacement so jumping / ladder / vertical-only motion
	// also triggers probe samples, not just horizontal walking.
	float dx = tx - g_probe_last_x;
	float dy = ty - g_probe_last_y;
	if (sqrtf(dx * dx + dy * dy) < 8.0f) return;
	g_probe_last_x = tx;
	g_probe_last_y = ty;
	g_probe_round++;

	// [v148] coordinate-PAIR matching (replaces linear regression): rebuild
	// recreates the avatar object every ~0.35s, so rounds sample DIFFERENT
	// objects and slope-fitting across them fails (V147 LOCK FAILED). Each
	// round scans the CURRENT object: y is foothold-snapped (stable), find a
	// value == ty, take the adjacent x (within 32B, sane map coord near tx).
	// The pair offset is fixed by the struct layout so it survives rebuilds;
	// 2 agreeing rounds (in different objects) lock it.
	// [v149] pass 0 = float, pass 1 = int32 pixels: V148 found NOTHING with
	// float-only because this client stores coords as int pixels (v95 note:
	// "client that stores coords as int pixels").
	// [v150] x-match tightened 2000 -> 30: V149 locked constant fields
	// (vx=0.0 / vy=374.0) because ANY field within 2000 of tx qualified, so a
	// constant-zero field next to a constant-374 field voted in and wrote to
	// garbage. A real coord pair must have x EXACTLY at the target (avatar
	// sits at tx right after a rebuild 0x11), so 30px is generous.
	// [v151] x-match 30 -> 120, AND pick the MINIMUM-diff field, not the
	// first match. V150 locked NOTHING (0 LOCKED both clients) because the
	// avatar does NOT sit exactly at tx - it walks toward tx, so the
	// in-memory x lags the target by 17..200px depending on when the probe
	// samples; 30px almost never matched. Also, first-match scanning let a
	// near-tx junk field shadow the real x (which sits further along the
	// scan). Scanning ALL candidates in y+/-32 and keeping the one with the
	// smallest |vx-tx| picks the real x; the 2-round vote still filters
	// junk. 120px still excludes the V149 fake pair (vx=0.0 vs tx=-220).
	// [v152] x-match 120 -> 300 and window +/-32 -> +/-64: V151 locked
	// NOTHING (0 LOCKED) while the target x raced -285..666 in ~100px/round;
	// a fast-walking avatar's in-memory x can lag tx by >120px, so 120 never
	// matched. 300 covers a full round of travel. V149's fake pair (vx=0.0,
	// constant zero field) is now rejected by the vx-follows-tx vote: a real
	// coord field's value CHANGES as the avatar walks (2 rounds differ), a
	// constant-zero field does not.
	// [v153] KNOWN-CANDIDATE FIRST: V152 proved (0x11A4,0x11A8) flt is the
	// real coord pair on the working side (vx EXACTLY == tx: -455.0, -627.8,
	// -488.1 ... and vy EXACTLY == ty on every hit; the v98 hard-coded
	// offsets were right all along - the old crash was a timing issue, not a
	// wrong offset). Try that pair directly FIRST; only if it fails do the
	// full scan. This makes the lagging side (V78 non-symmetric visibility)
	// lock instantly when its GetCharacterByOID pointer is a valid avatar.
	// [v154] V153 locked NOTHING: the 0x11A4 flt candidate did NOT hit on
	// either client this run (target raced -285..-966 ~150px/round), while a
	// full-scan hit appeared at (0x638,0x5F8) INT (vx=-75 vs tx=-206 - a real
	// in-motion lag, y=255 vs ty=257). So: add (0x638,0x5F8) int as a second
	// known candidate, widen x-match 300 -> 800 (fast walkers lag the target
	// by hundreds of px), and run known candidates BEFORE the full scan.
	// [v157] DROP the (0x638,0x5F8) int candidate: V156 logs prove it is a
	// CONSTANT field (vx=-75.0 on every round -> REJECT constant field) that
	// kept pre-empting the real 0x11A4 pair (found_pair=true set by the junk
	// candidate, then REJECT cleared it, next round re-hit it -> infinite
	// reject loop, real candidate never ran). V152/V154 already PROVED
	// (0x11A4,0x11A8) flt is the real coord (vx EXACTLY == tx). Keep ONLY
	// that candidate, with y-match 30 for jump coverage.
	int x_off = 0, y_off = 0; bool pair_is_int = false; bool found_pair = false;
	{
		float kx = *(float*)(ptr + 0x11A4);
		float ky = *(float*)(ptr + 0x11A8);
		if (isfinite(kx) && isfinite(ky) && fabs(kx - tx) < 300.0 && fabs(ky - ty) <= 30.0) {
			x_off = 0x11A4; y_off = 0x11A8; pair_is_int = false; found_pair = true;
		}
	}
	if (!found_pair) {
	for (int pass = 0; pass < 2 && !found_pair; pass++) {
		double best_diff = 1.0e9; int bx = 0, by = 0;
		for (int off_y = 0; off_y < PROBE_SCAN_RANGE; off_y += 4) {
			double vy = (pass == 0) ? (double)*(float*)(ptr + off_y) : (double)(*(int*)(ptr + off_y));
			double tyv = (pass == 0) ? (double)ty : (double)(int)ty;
			if (!isfinite((float)vy) || fabs(vy - tyv) > 30.0) continue;
			for (int off_x = off_y - 64; off_x <= off_y + 64; off_x += 4) {
				if (off_x < 0 || off_x + 4 > PROBE_SCAN_RANGE) continue;
				double vx = (pass == 0) ? (double)*(float*)(ptr + off_x) : (double)(*(int*)(ptr + off_x));
				double txv = (pass == 0) ? (double)tx : (double)(int)tx;
				if (!isfinite((float)vx) || vx < -5000.0 || vx > 5000.0) continue;
				double d = fabs(vx - txv);
				if (d < 800.0 && d < best_diff) { best_diff = d; bx = off_x; by = off_y; }
			}
		}
		if (best_diff < 1.0e8) { x_off = bx; y_off = by; pair_is_int = (pass == 1); found_pair = true; }
	}
	}

	if (g_probe_round <= 8) {
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) {
			fprintf(f, "[MP-PROBE v157] round=%d oid=%08X ptr=%08X target=(%.1f,%.1f) pair=%s",
				g_probe_round, oid, ptr, tx, ty, found_pair ? "yes" : "no");
			if (found_pair) fprintf(f, " x_off=0x%X y_off=0x%X %s (vx=%.1f vy=%.1f)", x_off, y_off,
				pair_is_int ? "int" : "flt",
				pair_is_int ? (double)*(int*)(ptr+x_off) : (double)*(float*)(ptr+x_off),
				pair_is_int ? (double)*(int*)(ptr+y_off) : (double)*(float*)(ptr+y_off));
			fprintf(f, "\n");
			fflush(f); fclose(f);
		}
	}

	if (g_probe_round < PROBE_LOCK_ROUNDS) return;
	if (!found_pair) {
		// No (x,y) pair in this object - restart probing.
		g_probe_round = 0; g_probe_last_x = 1.0e9f; g_probe_last_y = 1.0e9f;
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[MP-PROBE v157] NO PAIR at round %d\n", PROBE_LOCK_ROUNDS); fflush(f); fclose(f); }
		return;
	}
	// Cross-round vote: same pair seen 2+ times (in rebuilt objects) locks.
	// [v152] The pair's field VALUE must move between agreeing rounds: a real
	// coord field tracks the walking avatar (vx changes with tx), a constant
	// zero field (V149 fake) stays 0. First agreeing round records last_vx;
	// the second must differ by >= 8px to lock.
	unsigned long long pk = ((unsigned long long)(unsigned)x_off << 16) | (unsigned)(y_off & 0xFFFF);
	ProbeVote &vote = g_probe_pairs[pk];
	vote.count++;
	{
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[MP-PROBE v157] vote x_off=0x%X y_off=0x%X %s n=%d\n", x_off, y_off,
			pair_is_int ? "int" : "flt", vote.count); fflush(f); fclose(f); }
	}
	if (vote.count < 2) {
		vote.last_vx = pair_is_int ? (float)(*(int*)(ptr + x_off)) : (float)(*(float*)(ptr + x_off));
		return;
	}
	// Second agreeing round: the field must have MOVED (followed the avatar).
	float now_vx = pair_is_int ? (float)(*(int*)(ptr + x_off)) : (float)(*(float*)(ptr + x_off));
	if (fabsf(now_vx - vote.last_vx) < 8.0f) {
		// Constant field (V149 fake pair). Drop the vote and keep probing.
		g_probe_pairs.erase(pk);
		g_probe_round = 0; g_probe_last_x = 1.0e9f; g_probe_last_y = 1.0e9f;
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[MP-PROBE v157] REJECT constant field x_off=0x%X y_off=0x%X (vx %.1f == %.1f)\n",
			x_off, y_off, now_vx, vote.last_vx); fflush(f); fclose(f); }
		return;
	}

	g_interp_x_off = x_off;
	g_interp_y_off = y_off;   // 0 is valid: x-only writes still kill the flicker
	g_interp_x_is_int = pair_is_int; g_interp_x_scale = 1.0f; g_interp_x_intc = 0.0f;
	g_interp_y_is_int = pair_is_int; g_interp_y_scale = 1.0f; g_interp_y_intc = 0.0f;
	g_probe_locked = true;

	FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
	if (f) {
		fprintf(f, "[MP-PROBE v157] *** LOCKED x_off=0x%X y_off=0x%X (%s) ***\n", x_off, y_off,
			pair_is_int ? "int" : "flt");
		fflush(f); fclose(f);
	}
}

// [v96] Per-frame movement interpolation - runs unconditionally every frame
// so the remote avatar keeps sliding between server packets.
// [v139] Safety gate: never write client memory unless suppression has
// actually activated (g_suppress_active). Writing without suppression means
// the avatar is still being rebuilt, so any locked address goes stale or was
// a static/incorrect one -> crash (V138: "<9" gate disabled suppression, the
// mem-write still ran and killed the client).
// [v145] SMOOTH-mode memory write: in smooth mode (cfg 11110010, no rebuild
// 0x12/0x3D/0x11) the remote avatar object is NOT rebuilt, so the locked
// mem_x/mem_y address stays valid and writing needs no suppression (which
// would swallow packets and crash - the V140 crash). The sanity check below
// (|cur - tx| > 800) still unlocks and re-scans if the address ever goes
// stale, so the V138 crash class is covered.
static void MP_ApplyInterpolation() {
	if (g_mem_locked && g_mem_x_addr) {
		for (std::map<DWORD, RemoteInterp>::iterator it = g_interp.begin(); it != g_interp.end(); ++it) {
			RemoteInterp &ri = it->second;
			float cur = *(float*)g_mem_x_addr;
			// [v133] sanity: if the locked address stopped tracking the target
			// (avatar was rebuilt and the address is stale) stop writing.
			// [v137] blacklist the address so the scan does not re-lock it.
			if (ri.tx != 0.0f && fabsf(cur - ri.tx) > 800.0f) {
				if (g_mem_x_addr) g_mem_bad.insert(g_mem_x_addr);
				g_mem_locked = false; g_mem_x_addr = 0; g_mem_y_addr = 0;
				return;
			}
			float nxt = cur + (ri.tx - cur) * g_interp_speed;
			if (fabsf(nxt - ri.tx) < 0.5f) nxt = ri.tx;
			*(float*)g_mem_x_addr = nxt;
			if (g_mem_y_addr) {
				float cury = *(float*)g_mem_y_addr;
				float nxty = cury + (ri.ty - cury) * g_interp_speed;
				if (fabsf(nxty - ri.ty) < 0.5f) nxty = ri.ty;
				*(float*)g_mem_y_addr = nxty;
			}
			ri.cx = nxt;
		}
		return;
	}
	bool have_offsets = (g_interp_x_off != 0 || g_interp_y_off != 0);
	static int dbg = 0;
	if (++dbg % 60 == 0) {
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) { fprintf(f, "[MP-INTERP] ENABLED tracked=%d offsets=(%04X,%04X) speed=%.3f\n",
			(int)g_interp.size(), g_interp_x_off, g_interp_y_off, g_interp_speed);
			fflush(f); fclose(f); }
	}
	// [v73] Resolve live CCharacter* for every tracked remote oid each frame.
	DWORD cfield = MP_GetCFieldPtr();
	if (cfield && _GetCharacterByOID) {
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
		int applied = 0, rejected = 0;
		for (auto &kv : g_interp) {
			RemoteInterp &ri = kv.second;
			ri.stale++;
			auto it = g_char_by_oid.find(ri.oid);
			if (it == g_char_by_oid.end() || it->second == 0) continue;
			DWORD char_ptr = it->second;
			// [v171] Sanity-gate the object BEFORE writing. The v169 probe proved
			// _GetCharacterByOID hands back a recycled/stale object most of the
			// time (2 pointers alternating, neither ever held a coordinate), so
			// every previous lerp attempt wrote into a dead allocation - the
			// write "succeeded" (3377 log lines) while the rendered avatar was a
			// different object. Require the field to read back as a plausible map
			// coordinate; otherwise skip and report.
			DWORD rawx = 0, rawy = 0;
			if (!MP_SafeRead4(char_ptr + g_interp_x_off, &rawx) ||
				!MP_SafeRead4(char_ptr + g_interp_y_off, &rawy)) { rejected++; continue; }
			float probe_x, probe_y;
			memcpy(&probe_x, &rawx, 4); memcpy(&probe_y, &rawy, 4);
			bool plausible = (probe_x > -30000.0f && probe_x < 30000.0f &&
							  probe_y > -30000.0f && probe_y < 30000.0f &&
							  probe_x == probe_x && probe_y == probe_y);
			if (!plausible) {
				rejected++;
				if (rejected <= 6) {
					FILE *rf = NULL; fopen_s(&rf, MP_DiagPath(), "a");
					if (rf) { fprintf(rf, "[MP-INTERP v171] STALE ptr=%08X oid=%08X read=(%.1f,%.1f) want=(%.1f,%.1f) -> skip\n",
						char_ptr, ri.oid, probe_x, probe_y, ri.tx, ri.ty); fflush(rf); fclose(rf); }
				}
				continue;
			}
			float cur_cx = MP_ReadCoord(char_ptr, g_interp_x_off, g_interp_x_is_int);
			float cur_cy = MP_ReadCoord(char_ptr, g_interp_y_off, g_interp_y_is_int);
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
			// [v171] mirror fields (probe saw the same coordinate duplicated at
			// 0x17A4/0x17A8). Writing both covers the case where the renderer
			// reads the mirror rather than the primary copy.
			if (g_interp_x_off2) MP_WriteCoord(char_ptr, g_interp_x_off2, g_interp_x_is_int, nxt_cx);
			if (g_interp_y_off2) MP_WriteCoord(char_ptr, g_interp_y_off2, g_interp_y_is_int, nxt_cy);
			applied++;
			// [v171] Write-back verification, throttled. If the value we read one
			// frame later is not what we wrote, the client owns the field (good -
			// it is live) ; if it matches exactly forever while the avatar stays
			// put, the field is a dead cache and this offset is wrong.
			static int vdbg = 0;
			if ((++vdbg % 40) == 0) {
				DWORD bx = 0, by = 0; float rbx = 0, rby = 0;
				MP_SafeRead4(char_ptr + g_interp_x_off, &bx);
				MP_SafeRead4(char_ptr + g_interp_y_off, &by);
				memcpy(&rbx, &bx, 4); memcpy(&rby, &by, 4);
				FILE *vf = NULL; fopen_s(&vf, MP_DiagPath(), "a");
				if (vf) { fprintf(vf, "[MP-INTERP v171] ptr=%08X oid=%08X pre=(%.1f,%.1f) wrote=(%.1f,%.1f) readback=(%.1f,%.1f) tgt=(%.1f,%.1f)\n",
					char_ptr, ri.oid, cur_cx, cur_cy, nxt_cx, nxt_cy, rbx, rby, ri.tx, ri.ty);
					fflush(vf); fclose(vf); }
			}
		}
		if (applied > 0 || rejected > 0) {
			static int adbg = 0;
			if ((++adbg % 40) == 0) {
				FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
				if (f) { fprintf(f, "[MP-INTERP] applied=%d rejected=%d /%d tracked (x_off=%04X y_off=%04X mode=%d)\n",
					applied, rejected, (int)g_interp.size(), g_interp_x_off, g_interp_y_off, g_sup_mode);
					fflush(f); fclose(f); }
			}
		}
	} else {
		for (auto &kv : g_interp) kv.second.stale++;
	}
	// [v169] Wide-range coordinate field probe. v168 dumped only 0x000-0x1FF and
	// the render coords were not in range. Instead of dumping raw memory, scan
	// 0..0x2000 of the character object and log ONLY offsets whose float/int
	// value matches the known target position (tx/ty). Cross-sample intersection
	// then yields the true render-coordinate offsets. Emits one line per position
	// change per oid, so the log stays small.
	// [v171] scan=3 : LIVE-SOURCE probe. The v169 match-probe only proved which
	// fields *hold* a coordinate, not which one the renderer *reads*. Here we
	// poke +137 into every plausible float field of the avatar object and read
	// it back one frame later:
	//   readback ~= poked  -> the client continued from OUR value, so this field
	//                         is the authoritative position source (what we want)
	//   readback ~= before -> the client ignored/overwrote us: dead cache
	// Pointers and huge/zero values are skipped so the poke can never corrupt a
	// vtable or heap pointer. Runs in 40-field batches every 3rd frame.
	if (g_scan_kind == 3 && !g_interp.empty()) {
		static int   pk_off[40];
		static DWORD pk_before[40], pk_poked[40];
		static int   pk_n = 0;
		static DWORD pk_ptr = 0;
		static int   pk_frame = 0;
		if ((++pk_frame % 3) == 0) {
			if (pk_n > 0 && pk_ptr) {
				FILE *lf = NULL;
				for (int i = 0; i < pk_n; i++) {
					DWORD nowraw = 0;
					if (MP_SafeRead4(pk_ptr + pk_off[i], &nowraw)) {
						float fb, fp, fn;
						memcpy(&fb, &pk_before[i], 4);
						memcpy(&fp, &pk_poked[i], 4);
						memcpy(&fn, &nowraw, 4);
						if (fn == fn && fabsf(fn - fp) < 30.0f && nowraw != pk_poked[i]) {
							if (!lf) fopen_s(&lf, "D:/mp_live.log", "a");
							if (lf) fprintf(lf, "[LIVE-SOURCE] off=%04X before=%.1f poked=%.1f readback=%.1f\n",
								pk_off[i], fb, fp, fn);
						}
					}
					MP_SafeWrite4(pk_ptr + pk_off[i], pk_before[i]);
				}
				if (lf) { fflush(lf); fclose(lf); }
				pk_n = 0;
			}
			DWORD cp = 0;
			for (std::map<DWORD, RemoteInterp>::iterator kv = g_interp.begin(); kv != g_interp.end(); ++kv) {
				DWORD c = g_char_by_oid[kv->first];
				if (c) { cp = c; break; }
			}
			if (cp) {
				pk_ptr = cp;
				int o = g_live_probe_cursor;
				for (; o <= PROBE_SCAN_RANGE && pk_n < 40; o += 4) {
					DWORD raw = 0;
					if (!MP_SafeRead4(cp + o, &raw)) break;
					float fv; memcpy(&fv, &raw, 4);
					if (!(fv == fv)) continue;
					if (fabsf(fv) < 0.5f || fabsf(fv) > 30000.0f) continue;
					float pv = fv + 137.0f;
					DWORD pr; memcpy(&pr, &pv, 4);
					pk_off[pk_n] = o; pk_before[pk_n] = raw; pk_poked[pk_n] = pr;
					if (!MP_SafeWrite4(cp + o, pr)) break;
					pk_n++;
				}
				if (o > PROBE_SCAN_RANGE) {
					g_live_probe_cursor = 0;
					FILE *lf = NULL; fopen_s(&lf, "D:/mp_live.log", "a");
					if (lf) { fprintf(lf, "--- round end (ptr=%08X) ---\n", cp); fflush(lf); fclose(lf); }
				} else {
					g_live_probe_cursor = o;
				}
			}
		}
	}
	// [v197] FULL-PROCESS render-position hunt (scan=4). The v195 live-probe
	// (scan=3) proved the CField-hashmap avatar has NO live coordinate field
	// (302 probe rounds, 0 LIVE-SOURCE hits) - the renderer reads the position
	// from an object the CField hashmap does not cover. So scan the ENTIRE
	// readable RW process memory for a float (or int) pair equal to the remote
	// avatar's known rendered position (its spawn coords, cached from the 0x11
	// birth). Lock every match and drive all of them each frame with the server
	// target - the rendered copy is among them, so the avatar glides.
	// Candidates that the client reverts (>100px) are dropped (client-owned
	// fields); fields that keep our value are plain copies we keep writing.
	// [v216-safe] PRECISE walk-animation write ONLY. Position is driven by
	// the network 0x11 stream (smooth + stable, no crash). We do NOT scan
	// memory: the full-process scan crashed the 2nd client in v197/v206/v214,
	// and interp=1's auto-probe locked offsets that lerp then wrote -> crash.
	// Here we only set the move-state fields of the KNOWN CField avatar object
	// returned by GetCharacterByOID - the very offsets (0x898 moving, 0x8A0
	// stance) the game's own 0x19 handler writes every frame, so it can never
	// corrupt the heap. Result: smooth glide + walk animation + 2-client safe.
	if (g_scan_kind == 4 && !g_interp.empty()) {
		std::map<DWORD, RemoteInterp>::iterator kv = g_interp.begin();
		if (kv != g_interp.end()) {
			DWORD oid = kv->first;
			float tx = kv->second.tx, ty = kv->second.ty;
			static float s_lx = 0, s_ly = 0;
			float dx = tx - s_lx, dy = ty - s_ly;
			bool moving = (fabsf(dx) > 0.3f || fabsf(dy) > 0.3f);
			int face = (dx < 0.0f) ? 1 : 0;
			DWORD cfield = MP_GetCFieldPtr();
			if (cfield && _GetCharacterByOID) {
				DWORD cobj = _GetCharacterByOID((void*)cfield, oid);
				if (cobj) {
					MP_SafeWrite4(cobj + 0x898, moving ? 1 : 0);
					MP_SafeWrite4(cobj + 0x8A0, (DWORD)face);
				}
			}
			s_lx = tx; s_ly = ty;
			FILE *lf = NULL; fopen_s(&lf, "D:/mp_scan.log", "a");
			if (lf) { fprintf(lf, "[MP-SCAN v216] oid=%08X tgt=(%.1f,%.1f) moving=%d face=%d\n", oid, tx, ty, moving?1:0, face); fflush(lf); fclose(lf); }
		}
	}
	if (g_scan && g_scan_kind != 3 && g_scan_kind != 4 && !g_interp.empty()) {
		static std::map<DWORD, std::pair<float, float> > last_probe;
		FILE *sf = NULL;
		for (std::map<DWORD, RemoteInterp>::iterator kv = g_interp.begin(); kv != g_interp.end(); ++kv) {
			DWORD cp = g_char_by_oid[kv->first];
			if (!cp) continue;
			float tx = kv->second.tx, ty = kv->second.ty;
			if (tx == 0.0f && ty == 0.0f) continue;
			std::map<DWORD, std::pair<float, float> >::iterator lp = last_probe.find(kv->first);
			if (lp != last_probe.end() &&
				fabs((double)(lp->second.first - tx)) < 6.0 &&
				fabs((double)(lp->second.second - ty)) < 6.0) continue;
			last_probe[kv->first] = std::make_pair(tx, ty);

			if (!sf) fopen_s(&sf, "D:/mp_probe.log", "a");
			if (!sf) break;
			fprintf(sf, "[MP-PROBE] oid=%08X tx=%.2f ty=%.2f ptr=%08X\n", kv->first, tx, ty, cp);
			char xb[1600], yb[1600];
			xb[0] = 0; yb[0] = 0;
			int xn = 0, yn = 0;
			for (int o = 0; o <= 0x2000; o += 4) {
				DWORD raw = 0;
				if (!MP_SafeRead4(cp + o, &raw)) break;
				float fv; memcpy(&fv, &raw, 4);
				int iv = (int)raw;
				if (fv > -100000.0f && fv < 100000.0f) {
					if (fabs((double)(fv - tx)) < 24.0 && xn < 24) {
						xn++; size_t l = strlen(xb);
						_snprintf_s(xb + l, sizeof(xb) - l, _TRUNCATE, " %X:f%.1f", o, fv);
					}
					if (fabs((double)(fv - ty)) < 24.0 && yn < 24) {
						yn++; size_t l = strlen(yb);
						_snprintf_s(yb + l, sizeof(yb) - l, _TRUNCATE, " %X:f%.1f", o, fv);
					}
				}
				if (iv > -100000 && iv < 100000) {
					if (fabs((double)iv - (double)tx) < 24.0 && xn < 24) {
						xn++; size_t l = strlen(xb);
						_snprintf_s(xb + l, sizeof(xb) - l, _TRUNCATE, " %X:i%d", o, iv);
					}
					if (fabs((double)iv - (double)ty) < 24.0 && yn < 24) {
						yn++; size_t l = strlen(yb);
						_snprintf_s(yb + l, sizeof(yb) - l, _TRUNCATE, " %X:i%d", o, iv);
					}
				}
			}
			fprintf(sf, "  X:%s\n  Y:%s\n", xb, yb);
			break;
		}
		if (sf) { fflush(sf); fclose(sf); }
	}
}

void MP_Pump() {
	mp_frame_count++;
	// [v129] Interpolation + probe RE-ENABLED. v99 disabled them after the
	// probe locked hardcoded offsets 0x11A4/0x11A8 and crashed; the current
	// probe is v87 multi-round linear fit with residual gates (x<=4, y<=40)
	// and writes only after g_probe_locked. MP_WriteCoord no-ops on off==0.
	// Remote rebuild is suppressed once locked; the lerp writes coords each
	// frame, which is the only path to true smooth movement.
	if (MP_InterpEnabled() || g_scan) MP_ApplyInterpolation();

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
	// [v140] diag: log suppression gate state once per batch so we can see
	// why suppression does/doesn't activate (g_probe_locked from cfg locked=1,
	// batch composition, and the op code actually seen for 0x12 packets).
	{
		FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
		if (f) {
			fprintf(f, "[MP-DIAG v140] batch=%d locked=%d suppress_active=%d ops=", (int)batch.size(), g_probe_locked?1:0, g_suppress_active?1:0);
			for (size_t di = 0; di < batch.size() && di < 12; di++) {
				if (batch[di].first.empty()) continue;
				fprintf(f, "%02X ", batch[di].first[0]);
			}
			fprintf(f, "\n");
			fflush(f); fclose(f);
		}
	}
	if (g_probe_locked) {   // [v133] locked=1 forces suppression even before offsets are known
		// [v138] Full-staircase suppression. The rebuild staircase is
		//   0x12(remove oid) -> 0x3D(account data oid) -> 0x11(spawn oid)
		//   -> 0x3D(restore RECEIVER's own account data).
		// v137 swallowed only 0x12+0x11: the middle 0x3D still injected the
		// mover's account data into an avatar that no longer existed ->
		// client crashed. Here we swallow the WHOLE staircase (0x12 + the
		// 0x3D whose char id == removed oid + the 0x11) so the avatar never
		// flickers and the memory-write lerp owns the position. The
		// trailing 0x3D (different oid = the receiver's own data) is let
		// through. 0x3D oid lives at offset 5: [0]=op [1..4]=0 [5..8]=chr.id.
		DWORD now_ms = (DWORD)GetTickCount();
		for (size_t i = 0; i < batch.size(); i++) {
			// [v139] op-dependent length gate. 0x12/0x11 only carry the 4-byte
			// oid at [1..4] (5 bytes total); 0x3D additionally has chr.id at
			// [5..8] (>=9). The old uniform "<9" gate filtered out EVERY 0x12
			// (5 bytes) so suppression never activated while the mem-write
			// still ran -> wrote a stale/static address -> client crashed.
			if (batch[i].first.size() < 5) continue;
			BYTE op = batch[i].first[0];
			if (op == 0x3D && batch[i].first.size() < 9) continue;
			DWORD roid = *(DWORD*)&batch[i].first[1];
			if (roid == 0) continue;
			if (op == 0x12) {
				g_remove_tick[roid] = now_ms;
				mp_suppress[i] = 1;
				g_suppress_active = true;
				g_suppress_count++;
				if (g_suppress_count <= 8 || (g_suppress_count % 50) == 0) {
					FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
					if (f) {
						fprintf(f, "[MP-SUPPRESS v171 mode=%d] swallowed 0x12 #%d oid=%08X\n", g_sup_mode, g_suppress_count, roid);
						fflush(f); fclose(f);
					}
				}
			} else if (op == 0x3D) {
				// account-data oid at [5..8]; swallow only if it belongs to a
				// freshly removed avatar (the middle step of the staircase).
				DWORD data_oid = *(DWORD*)&batch[i].first[5];
				std::map<DWORD, DWORD>::iterator rt = g_remove_tick.find(data_oid);
				// [v171] mode=1 lets the account-data packet through: the avatar
				// was never destroyed, so its data must stay in sync.
				if (g_sup_mode != 1 &&
					rt != g_remove_tick.end() && (now_ms - rt->second) < 800) {
					mp_suppress[i] = 1;
					g_suppress_active = true;
					g_suppress_count++;
					if (g_suppress_count <= 8 || (g_suppress_count % 50) == 0) {
						FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
						if (f) {
							fprintf(f, "[MP-SUPPRESS v138] swallowed 0x3D(oid=%08X) #%d\n", data_oid, g_suppress_count);
							fflush(f); fclose(f);
						}
					}
				}
			} else if (op == 0x11) {
				std::map<DWORD, DWORD>::iterator rt = g_remove_tick.find(roid);
				// [v171] mode=1/2 let the spawn packet reach the client. Because
				// the matching 0x12 was swallowed the avatar still exists, so the
				// client's 0x11 handler updates it in place instead of creating a
				// new one -> native movement, no flicker, no memory write.
				if (g_sup_mode == 0 &&
					rt != g_remove_tick.end() && (now_ms - rt->second) < 800) {
					mp_suppress[i] = 1;
					g_suppress_active = true;
					g_suppress_count++;
					if (g_suppress_count <= 8 || (g_suppress_count % 50) == 0) {
						FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
						if (f) {
							fprintf(f, "[MP-SUPPRESS v138] swallowed 0x11 #%d oid=%08X\n", g_suppress_count, roid);
							fflush(f); fclose(f);
						}
					}
				} else {
					g_remove_tick.erase(roid);
				}
			}
		}
		for (std::map<DWORD, DWORD>::iterator it = g_remove_tick.begin(); it != g_remove_tick.end(); ) {
			if (now_ms - it->second > 800) g_remove_tick.erase(it++);
			else ++it;
		}
	}

	for (size_t i = 0; i < batch.size(); i++) {
		std::vector<BYTE> &bp = batch[i].first;   // non-const: ProcessPacketExec takes a mutable ref
		bool mp_ctx = batch[i].second;
		BYTE mp_op = (bp.size() > 0) ? bp[0] : 0;
		DWORD oid = 0;
		if (bp.size() >= 5) oid = *(DWORD*)&bp[1];
		// [v162-diag] 0x0C :  CField hashmap  oid ,
		//  dump  12 "0x0C "
		//   live=0  ->  CField hashmap(, )
		//   live!=0 ->  hashmap, /, 
		if (mp_op == 0x0C && bp.size() >= 5) {
			DWORD cfield = MP_GetCFieldPtr();
			DWORD live = (cfield && _GetCharacterByOID) ? _GetCharacterByOID((void*)cfield, oid) : 0;
			static int oc_chk = 0;
			if (oc_chk < 40 || (oc_chk % 20) == 0) {
				FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
				if (f) {
					fprintf(f, "[MP-0C-CHK v162] oid=%08X live=%08X ctx=%d len=%d bytes=", oid, live, mp_ctx?1:0, (int)bp.size());
					for (int bi = 0; bi < 12 && bi < (int)bp.size(); bi++) fprintf(f, "%02X ", bp[bi]);
					if (bp.size() >= 6) fprintf(f, " count=%d", (int)bp[5]);
					fprintf(f, "\n");
					fflush(f); fclose(f);
				}
			}
			oc_chk++;
		}
		// [v194] SP 0x19 move diagnostics. Parse our 9-byte wire format
		// ([0]=op [1..4]=oid [5]=count [6..7]=dx [8]=stance), resolve the
		// CField object and read its coordinate BEFORE the packet is injected,
		// then compare with the value logged when the PREVIOUS 0x19 for the
		// same oid arrived (that previous packet WAS processed by then). If the
		// native handler applied our dx, the position drifts between packets;
		// if it is frozen the format/handler path is wrong. No suppression, no
		// writes - pure observation.
		if (mp_op == 0x19 && bp.size() >= 9) {
			static int g_19diag = 0;
			BYTE cnt = bp[5];
			short rdx = 0;
			if (bp.size() >= 8) rdx = (short)*(WORD*)&bp[6];
			BYTE rst = bp[8];
			DWORD cfield = MP_GetCFieldPtr();
			DWORD live = (cfield && _GetCharacterByOID) ? _GetCharacterByOID((void*)cfield, oid) : 0;
			float px = 0, py = 0;
			if (live) { MP_SafeRead4(live + 0x11A4, (DWORD*)&px); MP_SafeRead4(live + 0x11A8, (DWORD*)&py); }
			static std::map<DWORD, float> g_19prev;
			float prev_x = 0;
			std::map<DWORD, float>::iterator pit = g_19prev.find(oid);
			if (pit != g_19prev.end()) prev_x = pit->second;
			g_19prev[oid] = px;
			if (g_19diag < 60 || (g_19diag % 15) == 0) {
				FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
				if (f) {
					fprintf(f, "[MP-19DIAG] oid=%08X cnt=%d dx=%d stance=%d ptr=%08X pos=(%.1f,%.1f) prevx=%.1f len=%d bytes=",
						oid, (int)cnt, (int)rdx, (int)rst, live, px, py, prev_x, (int)bp.size());
					for (int bi = 0; bi < 12 && bi < (int)bp.size(); bi++) fprintf(f, "%02X ", bp[bi]);
					fprintf(f, "\n"); fflush(f); fclose(f);
				}
			}
			g_19diag++;
		}
		// Diagnostics: remember the first self-tagged spawn we ever see.
		if (mp_op == 0x11 && mp_ctx && g_localObjectId == 0) g_localObjectId = oid;
		// [v72a-fix] Cache remote 0x11 spawn coords for interpolation. A remote
		// spawn is ANY 0x11 whose oid is not our own object id. Empirically the
		// deployed server sends all 0x11 spawns on the CWvsContext layer
		// (mp_ctx=true, ctx=1) - NOT CField - yet they render fine, so the old
		// "!mp_ctx" gate was wrong and left g_interp empty (hook never fired).
		// Drop the ctx requirement; identify remote purely by oid != self.
		// [v90-diag] 0x11 :  + ,  DLL .
		// A (op 1): [0]=op [1..4]=oid [5..8]=x [9..12]=y
		// B (op 2): [0..1]=op [2..5]=oid [6..9]=x [10..13]=y
		if (mp_op == 0x11 && oid != g_localObjectId) {
			static int g_11dump = 0;
			if (g_11dump < 12 && bp.size() >= 17) {
				g_11dump++;
				FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a");
				if (f) {
					fprintf(f, "[MP-11DUMP %d] oid=%08X size=%d bytes=", g_11dump, oid, (int)bp.size());
					for (int bi = 0; bi < 17 && bi < (int)bp.size(); bi++) fprintf(f, "%02X ", bp[bi]);
					float a_x = *(float*)&bp[5], a_y = *(float*)&bp[9];
					float b_x = *(float*)&bp[6], b_y = *(float*)&bp[10];
					fprintf(f, " | A(x=%.1f y=%.1f) B(x=%.1f y=%.1f)\n", a_x, a_y, b_x, b_y);
					fflush(f); fclose(f);
				}
			}
		}
		// 0x11 body: [0]=op, [1..4]=oid, [5..8]=x(float), [9..12]=y(float).
		if (mp_op == 0x11 && oid != g_localObjectId && bp.size() >= 13) {
			float rx = *(float*)&bp[5];
			float ry = *(float*)&bp[9];
			g_spawned[oid] = true;   // [v135] first 0x11 seen -> avatar exists now
			// [v204] poison guard: never seed g_interp with (0,0). A stray
			// origin update would zero the scan=4 target and permanently
			// block the scan gate (|px|>1) on the later-joining client.
			// Keep the last good position instead.
			if (rx == 0.0f && ry == 0.0f) {
				// avatar exists; just no coordinate update this packet
			} else {
				auto it = g_interp.find(oid);
				if (it == g_interp.end()) {
					RemoteInterp ri;
					ri.oid = oid; ri.tx = rx; ri.ty = ry;
					ri.cx = rx; ri.cy = ry; ri.stale = 0;
					g_interp[oid] = ri;
				} else {
					it->second.tx = rx; it->second.ty = ry; it->second.stale = 0;
				}
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
			// [v90] Only swallow the 0x11 once the coordinate probe has
			// locked. Before lock the lerp cannot write the new position,
			// so swallowing would freeze the remote avatar. Until then, let
			// the client process the 0x11 natively (it relocates the existing
			// object; the server no longer sends a 0x12 delete, so no strobe).
			// [v98] v97 swallowed every remote 0x11 and wrote coords at the
			// hard-coded offsets (0x11A4/0x11A8). This caused an access-violation
			// crash inside Tenvi.exe (c0000005 at 0x001bcdc1) because the offsets
			// are not safe / wrong for this client build. Revert to the v96
			// behaviour: only suppress the respawn sequence (0x12->0x3D->0x11)
			// to avoid the strobe, and let the client handle bare 0x11 updates
			// natively. Interpolation stays disabled until we have a verified
			// safe coordinate offset.
			if (g_probe_locked && mp_suppress[i]) mp_skip_inject = true;
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
	// [v99] Coordinate probe DISABLED. The probe was auto-locking offsets that
	// later caused access-violation crashes when lerp wrote to them. Do not
	// run any memory discovery until we can validate offsets offline.
	// [v146] MP_MemSearchCoords DISABLED: it locks the exe static segment
	// (0x001AF4DC - same address in every process, a constant, not the avatar)
	// because it scans ALL MEM_PRIVATE from 0x00010000. Writing it never moves
	// the avatar and it pre-empts the probe. Probe (MP_ProbeCoordOffsets) runs
	// after a rebuild 0x11, when the live avatar exists in the CField hashmap,
	// and locks a *stable object offset* (x_off/y_off) that survives rebuilds
	// because MP_ApplyInterpolation re-resolves GetCharacterByOID every frame.
	if (MP_InterpEnabled()) {
		if (!g_mem_locked) MP_ProbeCoordOffsets();
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
// [v29] (DLLGetAsyncKeyState)
//       "" -> (+)
//       :  ->  -> () ->
//              ->  ->  -> 
DWORD (__thiscall *_LoginButton)(void *ecx) = NULL;
DWORD __fastcall LoginButton_Hook(void *ecx) {
	if (MP_IsAuthed()) {
		DEBUG(L"[MP] LoginButton: already authed, let pass");
		return 0;
	}

	//  GetAsyncKeyState 
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

	// [v33] ("")
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
		//  HELLO 
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

	// : ()
	MP_ClearCred();
	MP_SetAuthed(true);
	MP_StopCapture();  // , 
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
	// [MP] :  TenviTest  EnterSendPacket_Hook 
	// FakeServer(cp)  0x04/0x05 ()
	//  StandaloneServer, , 0x04  UI 
	//  ->  0x04(),
	// 0x05 
	{ FILE *f = NULL; fopen_s(&f, MP_DiagPath(), "a"); if (f) { fprintf(f, "WSB clicked -> sync 0x04 + send CHARLIST\n"); fflush(f); fclose(f); } }
	_WorldSelectButton(ecx);
	//  0x04 = CharacterSelectPacket (opcode 04 00 FF FF FF FF 00)
	{
		BYTE sel04[7] = { 0x04, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
		std::vector<BYTE> p04(sel04, sel04 + 7);
		ProcessPacketExec(p04);
	}
	//  0x05 ()
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
static bool g_captureStarted = false;  // [v29] ()
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
	// [v29]  GetAsyncKeyState : 10
	// [v30 FIX] HWND(), ID
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

		// [v54dv54g] :
		//   0x48DD03: je 0x48dd4c -- 0x11 spawn ""  NOP
		//   [v54g]  v54d/v54f  0x498EA1  0x498ECE  patch!
		//   (v54b, patch): ,
		//   0x45adeb .  patch 
		//   "".  0x48DD03  NOP.
		r.Patch(0x0048DD03, L"90 90");   // 0x11 handler: 

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
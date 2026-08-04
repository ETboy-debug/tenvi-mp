#include"AutoResponse.h"
#include"MPClient.h"

DWORD Addr_OnPacketClass = 0;
DWORD Addr_OnPacketClass2 = 0;
DWORD Addr_OnPacket2 = 0;

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
		FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
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
	std::vector<std::vector<BYTE>> batch;
	std::vector<BYTE> packet;
	while (MP_PopPacket(packet)) {
		batch.push_back(packet);
	}
	if (batch.empty()) return;

	g_mp_in_batch = true;  // <-- START guard: block all EnterSendPacket during batch
	for (size_t i = 0; i < batch.size(); i++) {
		{
			FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
			if (f) {
				const std::vector<BYTE> &bp = batch[i];
				fprintf(f, ">> inject op=%02X len=%d\n", bp.size()>0?bp[0]:0, (int)bp.size());
				fflush(f); fclose(f);
			}
		}
		ProcessPacketExec(batch[i]);
		{
			FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
			if (f) {
				const std::vector<BYTE> &bp = batch[i];
				fprintf(f, "<< done op=%02X\n", bp.size()>0?bp[0]:0);
				fflush(f); fclose(f);
			}
		}
	}
	{
		FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
		if (f) { fprintf(f, "=== BATCH END (%d) ===\n", (int)batch.size()); fflush(f); fclose(f); }
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
			"No account entered.\n"
			"\n"
			"Please type your account in the login form,\n"
			"press TAB to switch to password field,\n"
			"type your password, then click Login.",
			"Tenvi MP", MB_OK | MB_ICONINFORMATION);
		return 0;
	}
	if (pw.empty()) {
		MessageBoxA(NULL,
			"Account captured but no password.\n"
			"After typing the account, press TAB to move to\n"
			"the password field, type it, then click Login.",
			"Tenvi MP", MB_OK | MB_ICONWARNING);
		return 0;
	}

	DEBUG(L"[MP] Native login: acc=%hs pw=%d chars", acc.c_str(), (int)pw.length());
	MP_SendLogin(acc, pw);

	BYTE res = 0;
	if (!MP_WaitCtrlResult(MP_CTRL_LOGIN_RESULT, 8000, res)) {
		MessageBoxA(NULL, "Login timeout (server not responding?).",
		            "Tenvi MP", MB_OK | MB_ICONERROR);
		MP_ResetLoginState();
		return 0;
	}
	if (res != 1) {
		MessageBoxA(NULL, "Login failed: wrong password or account error.",
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
	{ FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a"); if (f) { fprintf(f, "WSB clicked -> sync 0x04 + send CHARLIST\n"); fflush(f); fclose(f); } }
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
	{ FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a"); if (f) { fprintf(f, "[CONNECT #%d]\n", connect_call_count); fflush(f); fclose(f); } }
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
		fopen_s(&f, "D:/mp_diag.log", "a");
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
static int mp_frame_count = 0;
static bool g_captureStarted = false;  // [v29] 捕获是否已启动(只启动一次)
void __fastcall ProcessPacketCaller_Hook(void *ecx) {
	_ProcessPacketCaller(ecx);
	// [DIAG] Frame counter
	mp_frame_count++;
	if (mp_frame_count <= 30 || mp_frame_count % 10 == 0) {
		FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
		if (f) { fprintf(f, "[FRAME %d] ProcessPacketCaller end\n", mp_frame_count); fflush(f); fclose(f); }
	}
	// [MP] Inject server packets into client
	MP_Pump();
	// [v29] 延迟启动 GetAsyncKeyState 键盘捕获: 等第10帧时游戏窗口肯定已创建
	if (!g_captureStarted && mp_frame_count >= 10) {
		g_captureStarted = true;
		HWND gameWnd = GetForegroundWindow();  // 最可能的游戏窗口
		if (!gameWnd) gameWnd = GetActiveWindow();
		if (gameWnd) {
			MP_StartCapture(gameWnd);
			DEBUG(L"[MP] Capture started at frame %d, hwnd=%p", mp_frame_count, gameWnd);
			FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
			if (f) { fprintf(f, "[MP-CAP] Started at frame %d, hwnd=%p\n", mp_frame_count, (void*)gameWnd); fflush(f); fclose(f); }
		}
	}
	// [DIAG] Post-pump: confirm MP_Pump returned safely
	if (mp_frame_count > 2190) {
		FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
		if (f) { fprintf(f, "[FRAME %d] after MP_Pump OK\n", mp_frame_count); fflush(f); fclose(f); }
	}
	DelayExecution();
	// [DIAG] Post-delay: confirm entire frame completed
	if (mp_frame_count > 2190) {
		FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "a");
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
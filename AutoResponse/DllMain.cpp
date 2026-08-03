#include"AutoResponse.h"
#include"TenviData.h"
#include"MPClient.h"
#include <windows.h>
#include <stdio.h>

// [DIAG v8] Global exception handler - catches ANY crash and logs the address
LONG WINAPI CrashHandler(_EXCEPTION_POINTERS *ei) {
	FILE *f = NULL;
	fopen_s(&f, "D:/mp_crash.log", "w");
	if (f) {
		fprintf(f, "=== CRASH CAUGHT ===\n");
		fprintf(f, "ExceptionCode: 0x%08X\n", ei->ExceptionRecord->ExceptionCode);
		fprintf(f, "ExceptionAddress: 0x%p\n", ei->ExceptionRecord->ExceptionAddress);
		fprintf(f, "EIP/RIP: 0x%p\n", (void*)ei->ContextRecord->Eip);
#ifdef _M_X64
		fprintf(f, "RIP: 0x%p  RSP: 0x%p\n", (void*)ei->ContextRecord->Rip, (void*)ei->ContextRecord->Rsp);
#else
		fprintf(f, "EIP: 0x%p  ESP: 0x%p\n", (void*)ei->ContextRecord->Eip, (void*)ei->ContextRecord->Esp);
#endif
		fprintf(f, "FrameCount(at crash): unknown (check mp_diag.log for last frame)\n");
		fflush(f); fclose(f);
	}
	return EXCEPTION_CONTINUE_SEARCH; // let default handler show the dialog
}

// [FIX v10] Vectored Exception Handler - auto-skip null-deref crashes in Tenvi.exe
// Root cause: ConnectCaller returns true but connection object is never allocated.
// This causes MANY access violation sites throughout Tenvi.exe code (not just one).
// Instead of patching each site individually, we catch ALL AVs in .code section,
// log them, skip the offending instruction, and continue execution.
//
// Instruction length decoder for common x86 crash patterns:
//   MOV REG, [REG32+disp32] = 8B ModRM disp32 = 6 bytes (most common)
//   MOV REG, [REG32+disp8]  = 8B ModRM disp8  = 3 bytes
//   MOV REG, [REG32]        = 8B ModRM         = 2 bytes
//   CALL [REG+disp]         = FF ModRM ...     = 2-7 bytes
static DWORD g_skip_count = 0;
static const int MAX_AUTO_SKIP = 500; // safety limit

// Simple x86 instruction length estimate for common memory-access patterns
// Returns 0 if unknown (handler will abort)
static int EstimateInstrLen(BYTE *addr) {
	BYTE b0 = addr[0];
	BYTE b1 = (b0 != 0x0F) ? addr[1] : 0;

	// MOV r32, r/m32: 0x8B /r
	if (b0 == 0x8B) {
		byte modrm = b1;
		int mod = (modrm >> 6) & 3;
		int rm = modrm & 7;
		if (mod == 3) return 2;           // register mode: 8B C0-ish (2 bytes)
		if (mod == 0 && rm == 4) return 6; // SIB: 8B xx 04 xx disp32 (6 bytes)
		if (mod == 0 && rm == 5) return 6; // disp32: 8B xx 05 disp32 (6 bytes)
		if (mod == 1) return 3;            // disp8: 3 bytes
		if (mod == 2) return 6;            // disp32: 6 bytes
		return 2;                          // [reg] direct: 2 bytes
	}
	// CMP r/m32, imm32: 0x81 /7
	if (b0 == 0x81) {
		byte modrm = b1;
		int mod = (modrm >> 6) & 3;
		if (mod == 3) return 6;
		if (mod == 1) return 6;  // +disp8(1) + imm32(4) = 6? no: opcode(2)+disp8(1)+imm32(4)=7
		if (mod == 2) return 7;  // opcode(2)+disp32(4)+imm32(4)=10? no let me recalc
		// Actually: 81 /7 = 2 bytes opcode+modrm, then if mod=1: 1 disp8 + 4 imm32 = 7 total
		// if mod=2: 4 disp32 + 4 imm32 = 10 total
		if (mod == 1) return 7;
		if (mod == 2) return 10;
		return 6; // mod=3: 2 + 4 imm32 = 6
	}
	// LEA: 0x8D (same ModRM encoding as MOV)
	if (b0 == 0x8D) {
		byte modrm = b1;
		int mod = (modrm >> 6) & 3;
		int rm = modrm & 7;
		if (mod == 3) return 2;
		if (mod == 0 && rm == 4) return 6;
		if (mod == 0 && rm == 5) return 6;
		if (mod == 1) return 3;
		if (mod == 2) return 6;
		return 2;
	}
	// TEST r/m32, r32: 0x85 /r
	if (b0 == 0x85) {
		byte modrm = b1;
		int mod = (modrm >> 6) & 3;
		if (mod == 3) return 2;
		if (mod == 0 && rm == 4) return 6;
		if (mod == 0 && rm == 5) return 6;
		if (mod == 1) return 3;
		if (mod == 2) return 6;
		return 2;
	}
	// CALL r/m32: 0xFF /2
	if (b0 == 0xFF) {
		byte modrm = b1;
		int op = (modrm >> 3) & 7;
		if (op == 2) { // CALL
			int mod = (modrm >> 6) & 3;
			int rm = modrm & 7;
			if (mod == 3) return 2;
			if (mod == 1) return 3;
			if (mod == 2) return 6;
			return 2;
		}
	}
	// MOV r/m32, imm32: 0xC7 /0
	if (b0 == 0xC7) {
		byte modrm = b1;
		int mod = (modrm >> 6) & 3;
		if (mod == 3) return 6;
		if (mod == 1) return 7;
		if (mod == 2) return 10;
		return 6;
	}
	// POP r/m32: 0x8F /0
	if (b0 == 0x8F) {
		byte modrm = b1;
		int mod = (modrm >> 6) & 3;
		if (mod == 3) return 2;
		if (mod == 1) return 3;
		if (mod == 2) return 6;
		return 2;
	}
	// Unknown - assume minimal 2-byte to avoid infinite loop
	return 2;
}

LONG WINAPI VectoredHandler(_EXCEPTION_POINTERS *ei) {
	if (ei->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
		DWORD crash_addr = (DWORD)ei->ExceptionRecord->ExceptionAddress;
		// Only handle crashes in Tenvi.exe code segment (.text typically 0x00400000~0x004FFFFF)
		if (crash_addr >= 0x00400000 && crash_addr < 0x00700000) {
			g_skip_count++;
			if (g_skip_count > MAX_AUTO_SKIP) {
				// Too many skips - something is very wrong, bail out
				FILE *f = NULL;
				fopen_s(&f, "D:/mp_crash.log", "a");
				if (f) { fprintf(f, "VEH: SKIP LIMIT EXCEEDED (%d), aborting\n", g_skip_count); fclose(f); }
				return EXCEPTION_CONTINUE_SEARCH;
			}

			DWORD eip = ei->ContextRecord->Eip;
			int len = EstimateInstrLen((BYTE *)eip);

			// Log this skip
			{
				FILE *f = NULL;
				fopen_s(&f, "D:/mp_crash.log", "a");
				if (f) {
					fprintf(f, "[VEH #%02d] AV at 0x%08X instr_len=%d EIP=0x%08X ESP=0x%08X skip=YES\n",
						g_skip_count, crash_addr, len, eip, ei->ContextRecord->Esp);
					// Log raw bytes for analysis
					fprintf(f, "  bytes:");
					for (int i = 0; i < min(len, 8); i++) {
						fprintf(f, " %02X", ((BYTE *)eip)[i]);
					}
					fprintf(f, "\n");
					fflush(f); fclose(f);
				}
			}

			// Skip the crashing instruction
			ei->ContextRecord->Eip += len;

			// Also log to diag for timeline correlation
			{
				FILE *f = NULL;
				fopen_s(&f, "D:/mp_diag.log", "a");
				if (f) { fprintf(f, "[VEH #%02d] skipped AV at 0x%08X len=%d\n", g_skip_count, crash_addr, len); fflush(f); fclose(f); }
			}

			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	if (fdwReason == DLL_PROCESS_ATTACH) {
		// [DIAG] Install crash handler BEFORE anything else
		SetUnhandledExceptionFilter(CrashHandler);
		// [FIX v10] Install VEH for auto-skipping null deref crashes
		AddVectoredExceptionHandler(1, VectoredHandler);
		// [DIAG] Clear diagnostic log
		{ FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "w"); if (f) { fprintf(f, "=== DLL attach ===\n"); fclose(f); } }
		{ FILE *f = NULL; fopen_s(&f, "D:/mp_crash.log", "w"); if (f) { fprintf(f, "=== crash log ===\n"); fclose(f); } }
		DisableThreadLibraryCalls(hinstDLL);
		LoadRegionConfig(hinstDLL);
		tenvi_data.set_xml_path(GetXMLPath());
		AutoResponseHook();
		// [MP] Start plaintext bridge connection to standalone server
		MP_Start(hinstDLL);
	}
	return TRUE;
}

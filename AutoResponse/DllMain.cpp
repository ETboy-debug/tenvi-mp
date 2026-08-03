#include"AutoResponse.h"
#include"TenviData.h"
#include"MPClient.h"
#include <windows.h>
#include <stdio.h>

// [DIAG] Global exception handler - logs crash info
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
	return EXCEPTION_CONTINUE_SEARCH;
}

// [FIX v18] Minimal VEH - ZERO I/O inside exception context.
// v17's VEH had fopen_s/fprintf inside the handler which can cause NESTED
// exceptions in an AV context → Windows bypasses VEH and kills the process.
// v18 rule: VEH body only modifies EIP and increments a counter. Nothing else.
// Diagnostics: check g_veh_skip_count after crash, or look for [VEH] in diag.log.
// Shared across DllMain.cpp (define) and AutoResponse.cpp (extern read) for VEH diagnostics
volatile LONG g_veh_skip_count = 0;

LONG WINAPI VectoredHandler(_EXCEPTION_POINTERS *ei) {
	// Only handle access violations (NULL deref)
	if (ei->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;

	DWORD crashAddr = (DWORD)ei->ExceptionRecord->ExceptionAddress;
	// Cover the entire per-frame network session update function (0x4947F6~0x494923)
	// All three code paths dereference NULL connection object pointers:
	//   path1a: [esi+0x15e8]  path1b: [esi+0x15ec]  path2:  [esi+0x15f0]
	if (crashAddr >= 0x004947F6 && crashAddr <= 0x00494923) {
		InterlockedIncrement(&g_veh_skip_count);
		// Jump to function epilogue: 0x494921 = pop esi / pop ebp / ret 0x10
		ei->ContextRecord->Eip = 0x00494921;
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	if (fdwReason == DLL_PROCESS_ATTACH) {
		// [DIAG] Install crash handler BEFORE anything else
		SetUnhandledExceptionFilter(CrashHandler);
		// [FIX v17] Install precise VEH for NULL-deref skip in network update function
		AddVectoredExceptionHandler(1, VectoredHandler);
		// [DIAG] Clear diagnostic log  (V18 marker)
			{ FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "w"); if (f) { fprintf(f, "=== DLL attach V18 ===\n"); fclose(f); } }
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

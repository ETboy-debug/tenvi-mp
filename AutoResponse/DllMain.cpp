#include"AutoResponse.h"
#include"TenviData.h"
#include"MPClient.h"
#include <windows.h>
#include <stdio.h>

// [DIAG] Global exception handler - logs crash info (catches any OTHER crash,
// not the known NULL-deref sites which are now patched at the byte level in v19)
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

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	if (fdwReason == DLL_PROCESS_ATTACH) {
		// [DIAG] Install crash handler BEFORE anything else
		SetUnhandledExceptionFilter(CrashHandler);
		// [DIAG] Clear diagnostic log  (V57r marker)
			{ FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "w"); if (f) { fprintf(f, "=== DLL attach V57r ===\n"); fclose(f); } }
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

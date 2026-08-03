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

// [FIX v17] Precise VEH - only skips known NULL-deref crashes in the per-frame
// network session update function (0x4947F6~0x494921). All other exceptions pass through.
// When a crash occurs at a known bad address, we redirect EIP to the function epilogue
// (0x494921 = "ret 0x10") so the frame continues normally.
// Known crash points from v13/v15 testing:
//   0x494901 - path2: mov eax,[esi+0x15f0] then use eax (NULL connection slot 2)
//   0x494898 - path1b: mov ecx,[esi+0x15ec] then call ecx (NULL connection slot 1)
//   0x4948xx - path1a: mov edx,[esi+0x15e8] then use edx (NULL connection slot 0)
// We cover the entire range 0x494800~0x494920 to catch any variant.
#define CRASH_FUNC_ENTRY  0x004947F6
#define CRASH_FUNC_EPILOGUE 0x00492921

LONG WINAPI VectoredHandler(_EXCEPTION_POINTERS *ei) {
	// Only handle access violations (NULL deref)
	if (ei->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;

	DWORD crashAddr = (DWORD)ei->ExceptionRecord->ExceptionAddress;
	// Check if crash is inside the known-bad function range
	if (crashAddr >= 0x00494800 && crashAddr <= 0x0049920) {
		// Log the skip
		FILE *f = NULL;
		fopen_s(&f, "D:/mp_diag.log", "a");
		if (f) {
			fprintf(f, "[VEH SKIP] NULL-deref at 0x%08X -> jump to ret\n", crashAddr);
			fflush(f); fclose(f);
		}
		// Jump to function epilogue (ret 0x10) — safely exits the function
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
			// [DIAG] Clear diagnostic log  (V17 marker)
			{ FILE *f = NULL; fopen_s(&f, "D:/mp_diag.log", "w"); if (f) { fprintf(f, "=== DLL attach V17 ===\n"); fclose(f); } }
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

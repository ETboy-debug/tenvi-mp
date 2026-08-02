#include"AutoResponse.h"
#include"TenviData.h"
#include"MPClient.h"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	if (fdwReason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hinstDLL);
		LoadRegionConfig(hinstDLL);
		tenvi_data.set_xml_path(GetXMLPath());
		AutoResponseHook();
		// [MP] 启动到独立服务端的明文桥接连接
		MP_Start(hinstDLL);
	}
	return TRUE;
}
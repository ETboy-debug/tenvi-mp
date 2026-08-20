#include"TenviData.h"
#include"../EmuMainTenvi/ConfigTenvi.h"
#include<locale>
#include<codecvt>
#include<mutex>
#include<cstdio>

TenviData tenvi_data; // global

// [MP]  vector 
// ,  push_back  -> ,
// /
static std::mutex g_mapMutex;

TenviMap* TenviData::get_map(DWORD id) {
#ifdef MP_SERVER
	printf("[TenviServer] MARK get_map(%u) entry\n", (unsigned)id); fflush(stdout);
#endif
	std::lock_guard<std::mutex> lock(g_mapMutex);

	// map data is already loaded
	for (auto map : data_map) {
		if (map->GetID() == id) {
			return map;
		}
	}

#ifdef MP_SERVER
	printf("[TenviServer] MARK get_map(%u) before new TenviMap\n", (unsigned)id); fflush(stdout);
#endif
	// load map data
	TenviMap *map = new TenviMap(id);
#ifdef MP_SERVER
	printf("[TenviServer] MARK get_map(%u) after new TenviMap\n", (unsigned)id); fflush(stdout);
#endif
	data_map.push_back(map);
	return map;
}

void TenviData::set_xml_path(std::wstring path) {
	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
	xml_path = converter.to_bytes(path);
	region_str = converter.to_bytes(GetRegionStr());
}

std::string TenviData::get_xml_path() {
	return xml_path;
}

std::string TenviData::get_region_str() {
	return region_str;
}
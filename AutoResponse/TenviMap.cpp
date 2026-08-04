#include"rapidxml/rapidxml.hpp"
#include"rapidxml/rapidxml_utils.hpp"
#include"TenviMap.h"
#include"TenviData.h"
#include"../EmuMainTenvi/ConfigTenvi.h"
#include<cstdio>
#include<cstring>
#include<memory>
#ifdef MP_SERVER
#define TM_MARK(...) do { printf(__VA_ARGS__); fflush(stdout); } while(0)
#else
#define TM_MARK(...) do { } while(0)
#endif

TenviMap::TenviMap(DWORD mapid) {
	TM_MARK("[TenviServer] MARK TenviMap ctor entry id=%u\n", (unsigned)mapid);
	id = mapid;
	LoadXML();
	TM_MARK("[TenviServer] MARK TenviMap ctor exit id=%u\n", (unsigned)mapid);
}

rapidxml::xml_node<>* xml_find_dir(rapidxml::xml_node<>* parent, std::string name) {
	TM_MARK("[TenviServer] MARK xml_find_dir(%s) entry parent=%p\n", name.c_str(), (void*)parent);
	if (!parent) return NULL;
	int i = 0;
	for (rapidxml::xml_node<>* child = parent->first_node(); child; child = child->next_sibling()) {
		const char *cn = child->name();
		std::size_t cnlen = child->name_size();
		TM_MARK("[TenviServer] MARK   xfd child[%d] type=%d cnlen=%d cn=%p\n", i, (int)child->type(), (int)cnlen, (void*)cn);
		i++;
		// [FIX] 用 name_size()+memcmp 比较, 不依赖 name() 以 null 结尾(防止 compare 内部 strlen 越界)
		if (cn && cnlen == name.size() && cnlen > 0 && memcmp(cn, name.c_str(), cnlen) == 0) {
			TM_MARK("[TenviServer] MARK   xfd FOUND\n");
			return child;
		}
	}
	TM_MARK("[TenviServer] MARK   xfd not found\n");
	return NULL;
}

bool TenviMap::LoadXML() {

	std::string mapid_str = (id < 10000) ? ("0" + std::to_string(id)) : std::to_string(id);
	std::string map_xml = tenvi_data.get_xml_path() + +"\\" + tenvi_data.get_region_str() + "\\map\\" + mapid_str + "_0.xml";
	OutputDebugStringA(("[Maple] xml = " + map_xml).c_str());
	TM_MARK("[TenviServer] MARK LoadXML(%s) before parse\n", map_xml.c_str());
	rapidxml::xml_document<> doc;
	// [FIX] xmlFile 必须活到 doc 用完。rapidxml 就地解析, doc 的节点指针全部指向
	//       xmlFile 的内部缓冲区; 若 xmlFile 先析构(原代码声明在 try 块内, try 结束即释放),
	//       之后 first_node/name/next_sibling 访问节点就是 use-after-free 崩溃(c0000005)。
	std::unique_ptr<rapidxml::file<>> xmlFile;

	try {
		xmlFile.reset(new rapidxml::file<>(map_xml.c_str()));
		doc.parse<0>(xmlFile->data());
	}
	catch (...) {
		TM_MARK("[TenviServer] MARK LoadXML parse threw\n");
		return false;
	}
	TM_MARK("[TenviServer] MARK LoadXML after parse\n");

	rapidxml::xml_node<>* root = doc.first_node();

	if (!root) {
		return false;
	}

	// spawn point
	rapidxml::xml_node<> *map_sp = xml_find_dir(root, "sp");
	TM_MARK("[TenviServer] MARK LoadXML after find sp=%p\n", (void*)map_sp);
	if (map_sp) {
		for (rapidxml::xml_node<>* child = map_sp->first_node(); child; child = child->next_sibling()) {
			TenviSpawnPoint spawn_point = {};
			spawn_point.id = data_spawn_point.size(); // all map have only 1 spawn point?
			spawn_point.x = (float)atoi(child->first_attribute("x")->value());
			spawn_point.y = (float)atoi(child->first_attribute("y")->value());
			AddSpawnPoint(spawn_point);
		}
	}
	TM_MARK("[TenviServer] MARK LoadXML after sp\n");

	// portal
	rapidxml::xml_node<> *map_portal = xml_find_dir(root, "portal");
	TM_MARK("[TenviServer] MARK LoadXML after find portal=%p\n", (void*)map_portal);
	if (map_portal) {
		for (rapidxml::xml_node<>* child = map_portal->first_node(); child; child = child->next_sibling()) {
			TenviPortal portal = {};
			portal.id = atoi(child->first_attribute("no")->value());
			portal.next_id = atoi(child->first_attribute("tno")->value());
			portal.next_mapid = atoi(child->first_attribute("tid")->value());
			portal.x = (float)atoi(child->first_attribute("x")->value());
			portal.y = (float)atoi(child->first_attribute("y")->value());
			AddPortal(portal);
		}
	}
	TM_MARK("[TenviServer] MARK LoadXML after portal, before LoadSubXML\n");

	LoadSubXML();
	TM_MARK("[TenviServer] MARK LoadXML after LoadSubXML\n");
	return true;
}

bool TenviMap::LoadSubXML() {
	std::string mapid_str = (id < 10000) ? ("0" + std::to_string(id)) : std::to_string(id);
	std::string region_str;

	switch (GetRegion()) {
	case TENVI_JP:
	case TENVI_CN:
	case TENVI_HK: {
		region_str = "KR";
		break;
	}
	default:
	{
		region_str = tenvi_data.get_region_str();
		break;
	}
	}

	std::string map_xml = tenvi_data.get_xml_path() + +"\\" + region_str + "\\npc\\regen\\" + mapid_str + "_0.xml";
	OutputDebugStringA(("[Maple] subxml = " + map_xml).c_str());
	TM_MARK("[TenviServer] MARK LoadSubXML(%s) entry\n", map_xml.c_str());
	rapidxml::xml_document<> doc;
	// [FIX] 同 LoadXML: xmlFile 必须活到 doc 用完, 防止 use-after-free
	std::unique_ptr<rapidxml::file<>> xmlFile;

	try {
		xmlFile.reset(new rapidxml::file<>(map_xml.c_str()));
		doc.parse<0>(xmlFile->data());
	}
	catch (...) {
		TM_MARK("[TenviServer] MARK LoadSubXML parse threw\n");
		return false;
	}
	TM_MARK("[TenviServer] MARK LoadSubXML after parse\n");

	rapidxml::xml_node<>* root = doc.first_node();

	if (!root) {
		return false;
	}

	// regen
	for (rapidxml::xml_node<>* node = root->first_node(); node; node = node->next_sibling()) {
		TM_MARK("[TenviServer] MARK LoadSubXML regen node\n");
		TenviRegen regen = {};
		regen.id = atoi(node->first_attribute("id")->value());
		regen.flip = atoi(node->first_attribute("flip")->value());
		regen.population = atoi(node->first_attribute("population")->value());

		for (rapidxml::xml_node<>* child = node->first_node(); child; child = child->next_sibling()) {
			if (strcmp("area", child->name()) == 0) {
				regen.area.left = (float)atoi(child->first_attribute("left")->value());
				regen.area.top = (float)atoi(child->first_attribute("top")->value());
				regen.area.right = (float)atoi(child->first_attribute("right")->value());
				regen.area.bottom = (float)atoi(child->first_attribute("bottom")->value());
				continue;
			}
			if (strcmp("id", child->name()) == 0) {
				regen.object.id = atoi(child->first_attribute("value")->value());
				//area.id = atoi(child->first_attribute("min")->value());
				//area.id = atoi(child->first_attribute("max")->value());

				OutputDebugStringA(("[Maple] npc = " + std::string(child->first_attribute("value")->value())).c_str());
				continue;
			}
		}

		AddRegen(regen);
	}

	return true;
}

DWORD TenviMap::GetID() {
	return id;
}

void TenviMap::AddSpawnPoint(TenviSpawnPoint &spawn_point) {
	data_spawn_point.push_back(spawn_point);
}

void TenviMap::AddPortal(TenviPortal &portal) {
	data_portal.push_back(portal);
}

void TenviMap::AddRegen(TenviRegen &regen) {
	data_regen.push_back(regen);
}

std::vector<TenviRegen>& TenviMap::GetRegen() {
	return data_regen;
}

TenviSpawnPoint TenviMap::FindSpawnPoint(DWORD id) {
	for (auto &spawn_point : data_spawn_point) {
		if (spawn_point.id == id) {
			return spawn_point;
		}
	}

	TenviSpawnPoint fake_spawn_point = {};
	return fake_spawn_point;
}

TenviPortal TenviMap::FindPortal(DWORD id) {
	for (auto &portal : data_portal) {
		if (portal.id == id) {
			return portal;
		}
	}

	TenviPortal fake_portal = {};
	return fake_portal;
}

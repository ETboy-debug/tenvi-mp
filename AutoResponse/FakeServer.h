#ifndef __FAKESERVER_H__
#define __FAKESERVER_H__

#include"ClientPacket.h"
#include"ServerPacket.h"
#include"TenviData.h"
#include"TemporaryData.h"

// FakeServer.cpp 使用的全局账号数据（定义在 TemporaryData.cpp）
// [MP] 每个连接线程一份独立会话状态, 这样多个玩家同时连服务端不会串号。
// 客户端 DLL 侧只有主线程访问, 行为不变。
extern thread_local TenviAccount TA;

#define MAPID_ITEM_SHOP 65535
#define MAPID_PARK 65534
#define MAPID_EVENT 62501

bool FakeServer(ClientPacket &cp);
// test
void WorldListPacket();
void CharacterSelectPacket();
void CharacterListPacket();
void CharacterListPacket_Test();

#endif
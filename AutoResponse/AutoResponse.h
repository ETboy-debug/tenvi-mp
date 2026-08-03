#ifndef __AUTORESPONSE_H__
#define __AUTORESPONSE_H__

#include"../Share/Simple/Simple.h"
#include"../Share/Hook/SimpleHook.h"
#include"TenviPacket.h"
#include"ClientPacket.h"
#include"ServerPacket.h"
#include"FakeServer.h"
#include"../EmuMainTenvi/ConfigTenvi.h"

bool AutoResponseHook();
void SendPacket(ServerPacket &sp);
void SendPacket2(ServerPacket &sp);
void DelaySendPacket(ServerPacket &sp);

// [FIX v18] VEH skip counter — defined in DllMain.cpp, read by AutoResponse.cpp for diag logging
// Use 'long' (not LONG) so the header is self-contained without windows.h.
extern volatile long g_veh_skip_count;

#endif

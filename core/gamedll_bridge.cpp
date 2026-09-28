/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * ======================================================
 * Metamod:Source
 * Copyright (C) 2004-2009 AlliedModders LLC and authors.
 * All rights reserved.
 * ======================================================
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from 
 * the use of this software.
 * 
 * Permission is granted to anyone to use this software for any purpose, 
 * including commercial applications, and to alter it and redistribute it 
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not 
 * claim that you wrote the original software. If you use this software in a 
 * product, an acknowledgment in the product documentation would be 
 * appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <cassert>

#include "metamod.h"
#include "metamod_plugins.h"
#include "metamod_util.h"
#include "loader_bridge.h"
#include "provider/provider_base.h"
#include "khook.hpp"

using namespace SourceMM;

class GameDllBridge : public IGameDllBridge
{
public:
	virtual bool DLLInit_Pre(const gamedll_bridge_info *info, char *buffer, size_t maxlength)
	{
		mm_LogInterfaceTrace("C.BRIDGE.PRE.ENTER",
			"engine_factory=%p server_factory=%p filesystem_factory=%p server=%p interface=%s version=%d",
			info->engineFactory,
			info->gsFactory,
			info->fsFactory,
			info->isgd,
			info->dllInterfaceName ? info->dllInterfaceName : "<null>",
			info->dllVersion);
		server = (IServerGameDLL *) info->isgd;
		g_Metamod.SetGameDLLInfo((CreateInterfaceFn)info->gsFactory,
			info->dllInterfaceName,
			info->dllVersion,
			true);
		g_Metamod.SetVSPListener(info->vsp_listener_path);
		mm_LogInterfaceTrace("C.GLOBALS.ENTER", "server=%p", server);
		mm_InitializeGlobals((CreateInterfaceFn) info->engineFactory,
			(CreateInterfaceFn) info->physicsFactory,
			(CreateInterfaceFn) info->fsFactory,
			(CGlobalVars*) info->pGlobals);
		mm_LogInterfaceTrace("C.GLOBALS.LEAVE", "globals=%p", gpGlobals);

		mm_LogInterfaceTrace("C.GAMEINFO.ENTER", "detect=1");
		if (!mm_DetectGameInformation())
		{
			mm_LogInterfaceTrace("C.GAMEINFO.LEAVE", "result=0");
			UTIL_Format(buffer, maxlength, "Metamod:Source failed to detect game paths; cannot load.");
			return false;
		}
		mm_LogInterfaceTrace("C.GAMEINFO.LEAVE", "result=1 path=%s", g_Metamod.GetBaseDir());

		mm_LogInterfaceTrace("C.INITLOAD.ENTER", "gs_factory=%p", info->gsFactory);
		mm_InitializeForLoad();
		mm_LogInterfaceTrace("C.INITLOAD.LEAVE", "");

		mm_LogInterfaceTrace("C.STARTUP.ENTER", "vsp_load=0");
		mm_StartupMetamod(false);
		mm_LogInterfaceTrace("C.STARTUP.LEAVE", "");

		mm_LogInterfaceTrace("C.BRIDGE.PRE.LEAVE", "result=1");

		return true;
	}
	virtual void DLLInit_Post(int *isgdUnload)
	{
		mm_LogInterfaceTrace("C.BRIDGE.POST.ENTER", "");
		SourceHook::MemFuncInfo mfi;

#ifdef META_IS_SOURCE2
		auto mfi = KHook::GetVtableIndex(&ISource2ServerConfig::Disconnect);
#else
		auto mfi = KHook::GetVtableIndex(&IServerGameDLL::DLLShutdown);
#endif
		assert(mfi != -1);
		*isgdUnload = mfi;
		mm_LogInterfaceTrace("C.BRIDGE.POST",
			"shutdown_slot=%d",
			*isgdUnload);

		mm_LogInterfaceTrace("C.PLUGINS.SETALLLOADED.ENTER", "plugins=%d", g_PluginMngr.GetPluginCount());
		g_PluginMngr.SetAllLoaded();
		mm_LogInterfaceTrace("C.PLUGINS.SETALLLOADED.LEAVE", "");
		mm_LogInterfaceTrace("C.BRIDGE.POST.LEAVE", "");
	}
	virtual void *QueryInterface(const char *iface, int *ret)
	{
		mm_LogInterfaceTrace("C.BRIDGE.QUERY.ENTER", "interface=%s", iface ? iface : "<null>");
		void *result = g_Metamod.GetServerFactory(true)(iface, ret);
		mm_LogInterfaceTrace("C.BRIDGE.QUERY.LEAVE",
			"interface=%s result=%p",
			iface ? iface : "<null>",
			result);
		return result;
	}
	virtual void Unload()
	{
		mm_LogInterfaceTrace("C.UNLOAD.ENTER", "plugins=%d", g_PluginMngr.GetPluginCount());
		mm_UnloadMetamod();
		mm_LogInterfaceTrace("C.UNLOAD.LEAVE", "complete=1");
	}
};

GameDllBridge mm16_gamedll_bridge;

SMM_API IGameDllBridge *
GetGameDllBridge()
{
	return &mm16_gamedll_bridge;
}


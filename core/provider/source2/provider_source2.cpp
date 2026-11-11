/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * ======================================================
 * Metamod:Source
 * Copyright (C) 2004-2023 AlliedModders LLC and authors.
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

#include "provider_source2.h"
#include "metamod.h"
#include "metamod_util.h"
#include "metamod_console.h"
#include "amtl/am-string.h"
#include "tier0/keyvalues.h"
#include "filesystem/ifilesystem.h"
#include "engine2/isource2serverconfig.h"
#include "engine2/networkgameserverbase.h"
#include "engine2/inetworkserverservice.h"


static Source2Provider g_Source2Provider;

IMetamodSourceProvider* provider = &g_Source2Provider;

CON_COMMAND_EXTERN(meta, LocalCommand_Meta, "Metamod:Source control options");

static ISource2ServerConfig* serverconfig = NULL;
INetworkServerService* netservice = NULL;
IEngineServiceMgr* enginesvcmgr = NULL;

#if defined MMS_INTERFACE_TRACE
static void *
QueryInterfaceWithTrace(const char *stage,
						const char *factory_name,
						CreateInterfaceFn factory,
						const char *interface_name)
{
	int return_code = -1;
	bool trace_enabled = mm_IsInterfaceTraceEnabled();
	mm_LogInterfaceTrace(stage,
		"QUERY ENTER factory=%s factory_ptr=%p interface=%s",
		factory_name,
		factory,
		interface_name);
	void *result = factory(interface_name, trace_enabled ? &return_code : NULL);
	mm_LogInterfaceTrace(stage,
		"QUERY LEAVE factory=%s interface=%s result=%p ret=%d",
		factory_name,
		interface_name,
		result,
		return_code);
	return result;
}

template <typename MethodPointer>
static void
TraceInterfaceMethod(const char *stage,
					 const char *interface_name,
					 const char *method_name,
					 void *object,
					 MethodPointer method)
{
	if (!mm_IsInterfaceTraceEnabled())
		return;

	mm_LogInterfaceVTable(stage,
		interface_name,
		method_name,
		object,
		KHook::GetVtableIndex(method));
}
#else
/* Tracing compiled out: a plain factory call, and no vtable inspection. */
#define QueryInterfaceWithTrace(stage, factory_name, factory, interface_name) \
	((factory)((interface_name), NULL))
#define TraceInterfaceMethod(...) ((void)0)
#endif

Source2Provider::Source2Provider() :
	m_RegisterLoopMode(&IEngineServiceMgr::RegisterLoopMode, this, &Source2Provider::Hook_RegisterLoopMode, nullptr),
	m_UnregisterLoopMode(&IEngineServiceMgr::UnregisterLoopMode, this, &Source2Provider::Hook_UnregisterLoopMode, nullptr),
	m_CreateLoopMode(&ILoopModeFactory::CreateLoopMode, this, nullptr, &Source2Provider::Hook_CreateLoopModePost),
	m_DestroyLoopMode(&ILoopModeFactory::DestroyLoopMode, this, nullptr, &Source2Provider::Hook_DestroyLoopMode),
	m_LoopInit(&ILoopMode::LoopInit, this, nullptr, &Source2Provider::Hook_LoopInitPost),
	m_LoopShutdown(&ILoopMode::LoopShutdown, this, nullptr, &Source2Provider::Hook_LoopShutdownPost),
	m_ClientCommand(&IServerGameClients::ClientCommand, this, &Source2Provider::Hook_ClientCommand, nullptr)
#ifdef SHOULD_OVERRIDE_ALLOWDEDICATED_SERVER
	,m_AllowDedicatedServers(&ISource2ServerConfig::AllowDedicatedServers, this, &Source2Provider::Hook_AllowDedicatedServers, nullptr)
#endif
{
}

void Source2Provider::Notify_DLLInit_Pre(CreateInterfaceFn engineFactory,
	CreateInterfaceFn serverFactory)
{
	mm_LogInterfaceTrace("C.PROVIDER.ENTER",
		"engine_factory=%p server_factory=%p",
		engineFactory,
		serverFactory);

	engine = (IVEngineServer*)QueryInterfaceWithTrace(
		"C0.ENGINE",
		"engine",
		engineFactory,
		INTERFACEVERSION_VENGINESERVER);
	if (!engine)
	{
		mm_LogInterfaceTrace("C0.ENGINE.FATAL",
			"interface=%s result=<null>",
			INTERFACEVERSION_VENGINESERVER);
		DisplayError("Could not find IVEngineServer! Metamod cannot load.");
		return;
	}

	TraceInterfaceMethod("C0.VTABLE",
		"IVEngineServer",
		"GetServerGlobals",
		engine,
		&IVEngineServer::GetServerGlobals);
	mm_LogInterfaceTrace("C0.CALL.ENTER", "method=IVEngineServer::GetServerGlobals object=%p", engine);
	gpGlobals = engine->GetServerGlobals();
	mm_LogInterfaceTrace("C0.CALL.LEAVE",
		"method=IVEngineServer::GetServerGlobals result=%p",
		gpGlobals);

	serverconfig = (ISource2ServerConfig*)QueryInterfaceWithTrace(
		"C1.SERVER_CONFIG",
		"server",
		serverFactory,
		INTERFACEVERSION_SERVERCONFIG);
#if defined MMS_INTERFACE_TRACE
	if (!serverconfig)
	{
		mm_LogInterfaceTrace("C1.SERVER_CONFIG.FATAL",
			"interface=%s result=<null>",
			INTERFACEVERSION_SERVERCONFIG);
	}
#endif
	netservice = (INetworkServerService*)QueryInterfaceWithTrace(
		"C2.NETWORK_SERVICE",
		"engine",
		engineFactory,
		NETWORKSERVERSERVICE_INTERFACE_VERSION);
#if defined MMS_INTERFACE_TRACE
	if (!netservice)
	{
		mm_LogInterfaceTrace("C2.NETWORK_SERVICE.OPTIONAL_MISSING",
			"interface=%s result=<null>",
			NETWORKSERVERSERVICE_INTERFACE_VERSION);
	}
#endif
	enginesvcmgr = (IEngineServiceMgr*)QueryInterfaceWithTrace(
		"C3.ENGINE_SERVICE_MGR",
		"engine",
		engineFactory,
		ENGINESERVICEMGR_INTERFACE_VERSION);
#if defined MMS_INTERFACE_TRACE
	if (!enginesvcmgr)
	{
		mm_LogInterfaceTrace("C3.ENGINE_SERVICE_MGR.FATAL",
			"interface=%s result=<null>",
			ENGINESERVICEMGR_INTERFACE_VERSION);
	}
#endif

	icvar = (ICvar*)QueryInterfaceWithTrace(
		"C4.CVAR",
		"engine",
		engineFactory,
		CVAR_INTERFACE_VERSION);
	if (!icvar)
	{
		mm_LogInterfaceTrace("C4.CVAR.FATAL",
			"interface=%s result=<null>",
			CVAR_INTERFACE_VERSION);
		DisplayError("Could not find ICvar! Metamod cannot load.");
		return;
	}

	gameclients = (IServerGameClients*)QueryInterfaceWithTrace(
		"C5.GAME_CLIENTS",
		"server",
		serverFactory,
		INTERFACEVERSION_SERVERGAMECLIENTS);
	g_pFullFileSystem = baseFs = (IFileSystem*)QueryInterfaceWithTrace(
		"C6.FILESYSTEM",
		"engine",
		engineFactory,
		FILESYSTEM_INTERFACE_VERSION);
	if (baseFs == NULL)
	{
		mm_LogInterfaceTrace("C6.FILESYSTEM.FATAL",
			"interface=%s result=<null>",
			FILESYSTEM_INTERFACE_VERSION);
		mm_LogMessage("Unable to find \"%s\": .vdf files will not be parsed", FILESYSTEM_INTERFACE_VERSION);
	}

	TraceInterfaceMethod("C6.VTABLE", "IFileSystem", "GetSearchPathsForPathID", baseFs, &IFileSystem::GetSearchPathsForPathID);
	TraceInterfaceMethod("C6.VTABLE", "IFileSystem", "RemoveSearchPath", baseFs, &IFileSystem::RemoveSearchPath);
	TraceInterfaceMethod("C6.VTABLE", "IFileSystem", "RemoveSearchPaths", baseFs, &IFileSystem::RemoveSearchPaths);
	TraceInterfaceMethod("C6.VTABLE", "IFileSystem", "GetSearchPath", baseFs, &IFileSystem::GetSearchPath);
	TraceInterfaceMethod("C6.VTABLE", "IFileSystem", "AddSearchPath", baseFs, &IFileSystem::AddSearchPath);

	// Since we have to be added as a Game path (cannot add GameBin directly), we
	// automatically get added to other paths as well, including having the MM:S
	// dir become the default write path for logs and more. We can fix some of these.
	
	// GAMMACASE: This deals with the search paths where metamod could get added to, but there are still
	// problems with this as console.log file for example would be created way earlier than we alter these search paths,
	// and will be created in the metamod folder still, unsure what the solution to that could be tho!

	// NOTE: baseFs->PrintSearchPaths(); could be used to print out search paths to debug them.

	const char *pathIds[] = {
		"ADDONS",
		"CONTENT",
		"CONTENTADDONS",
		"CONTENTROOT",
		"EXECUTABLE_PATH",
		"GAME",
		"GAMEBIN",
		"GAMEROOT",
		"MOD",
		"PLATFORM",
		"SHADER_SOURCE",
		"SHADER_SOURCE_MOD",
		"SHADER_SOURCE_ROOT"
	};

	for(size_t id = 0; id < (sizeof(pathIds) / sizeof(pathIds[0])); id++)
	{
		CUtlVector<CUtlString> searchPaths;
		mm_LogInterfaceTrace("C6.CALL.ENTER",
			"method=IFileSystem::GetSearchPathsForPathID path_id=%s",
			pathIds[id]);
		baseFs->GetSearchPathsForPathID(pathIds[id], (GetSearchPathTypes_t)0, searchPaths);
		mm_LogInterfaceTrace("C6.CALL.LEAVE",
			"method=IFileSystem::GetSearchPathsForPathID path_id=%s count=%d",
			pathIds[id],
			searchPaths.Count());

		FOR_EACH_VEC(searchPaths, i)
		{
			if(strstr(searchPaths[i].Get(), "metamod") != 0)
			{
				mm_LogInterfaceTrace("C6.CALL.ENTER",
					"method=IFileSystem::RemoveSearchPath path_id=%s path=%s",
					pathIds[id],
					searchPaths[i].Get());
				baseFs->RemoveSearchPath(searchPaths[i].Get(), pathIds[id]);
				mm_LogInterfaceTrace("C6.CALL.LEAVE",
					"method=IFileSystem::RemoveSearchPath path_id=%s",
					pathIds[id]);
			}
		}
	}

	mm_LogInterfaceTrace("C6.CALL.ENTER", "method=IFileSystem::RemoveSearchPaths path_id=DEFAULT_WRITE_PATH");
	baseFs->RemoveSearchPaths("DEFAULT_WRITE_PATH");
	mm_LogInterfaceTrace("C6.CALL.LEAVE", "method=IFileSystem::RemoveSearchPaths path_id=DEFAULT_WRITE_PATH");

	CBufferStringN<260> searchPath;
	mm_LogInterfaceTrace("C6.CALL.ENTER", "method=IFileSystem::GetSearchPath path_id=GAME");
	baseFs->GetSearchPath("GAME", (GetSearchPathTypes_t)0, searchPath, 1);
	mm_LogInterfaceTrace("C6.CALL.LEAVE",
		"method=IFileSystem::GetSearchPath path_id=GAME path=%s",
		searchPath.Get());
	mm_LogInterfaceTrace("C6.CALL.ENTER",
		"method=IFileSystem::AddSearchPath path_id=DEFAULT_WRITE_PATH path=%s",
		searchPath.Get());
	baseFs->AddSearchPath(searchPath.Get(), "DEFAULT_WRITE_PATH");
	mm_LogInterfaceTrace("C6.CALL.LEAVE", "method=IFileSystem::AddSearchPath path_id=DEFAULT_WRITE_PATH");

	g_pCVar = icvar;

	TraceInterfaceMethod("C4.VTABLE", "ICvar", "RegisterConCommand", icvar, &ICvar::RegisterConCommand);
	TraceInterfaceMethod("C4.VTABLE", "ICvar", "RegisterConVar", icvar, &ICvar::RegisterConVar);
	mm_LogInterfaceTrace("C4.CALL.ENTER", "method=ConVar_Register object=%p", icvar);
	ConVar_Register(FCVAR_RELEASE);
	mm_LogInterfaceTrace("C4.CALL.LEAVE", "method=ConVar_Register");

	if (gameclients)
	{
		TraceInterfaceMethod("C5.VTABLE", "IServerGameClients", "ClientCommand", gameclients, &IServerGameClients::ClientCommand);
		mm_LogInterfaceTrace("C5.HOOK.ENTER", "method=IServerGameClients::ClientCommand object=%p", gameclients);
		m_ClientCommand.Add(gameclients);
		mm_LogInterfaceTrace("C5.HOOK.LEAVE", "method=IServerGameClients::ClientCommand");
	}
#if defined MMS_INTERFACE_TRACE
	else
	{
		mm_LogInterfaceTrace("C5.HOOK.SKIP", "reason=interface-null");
	}
#endif

#ifdef SHOULD_OVERRIDE_ALLOWDEDICATED_SERVER
	TraceInterfaceMethod("C1.VTABLE", "ISource2ServerConfig", "AllowDedicatedServers", serverconfig, &ISource2ServerConfig::AllowDedicatedServers);
	mm_LogInterfaceTrace("C1.HOOK.ENTER", "method=ISource2ServerConfig::AllowDedicatedServers object=%p", serverconfig);
	m_AllowDedicatedServers.AddGlobal(serverconfig);
	mm_LogInterfaceTrace("C1.HOOK.LEAVE", "method=ISource2ServerConfig::AllowDedicatedServers");
#endif

	TraceInterfaceMethod("C3.VTABLE", "IEngineServiceMgr", "RegisterLoopMode", enginesvcmgr, &IEngineServiceMgr::RegisterLoopMode);
	TraceInterfaceMethod("C3.VTABLE", "IEngineServiceMgr", "UnregisterLoopMode", enginesvcmgr, &IEngineServiceMgr::UnregisterLoopMode);
	mm_LogInterfaceTrace("C3.HOOK.ENTER", "method=IEngineServiceMgr::RegisterLoopMode object=%p", enginesvcmgr);
	m_RegisterLoopMode.Add(enginesvcmgr);
	mm_LogInterfaceTrace("C3.HOOK.LEAVE", "method=IEngineServiceMgr::RegisterLoopMode");
	mm_LogInterfaceTrace("C3.HOOK.ENTER", "method=IEngineServiceMgr::UnregisterLoopMode object=%p", enginesvcmgr);
	m_UnregisterLoopMode.Add(enginesvcmgr);
	mm_LogInterfaceTrace("C3.HOOK.LEAVE", "method=IEngineServiceMgr::UnregisterLoopMode");
	mm_LogInterfaceTrace("C.PROVIDER.LEAVE", "result=1");
}

void Source2Provider::Notify_DLLShutdown_Pre()
{
	for(auto cvar : m_RegisteredConVars)
		delete cvar;

	ConVar_Unregister();

	m_RegisterLoopMode.Remove(enginesvcmgr);
	m_UnregisterLoopMode.Remove(enginesvcmgr);

	if (gameclients)
	{
		m_ClientCommand.Remove(gameclients);
	}
}

bool Source2Provider::ProcessVDF(const char* file, char path[], size_t path_len, char alias[], size_t alias_len)
{
	if (baseFs == NULL)
	{
		return false;
	}

	KeyValues* pValues;
	bool bKVLoaded = false;
	const char* plugin_file, * p_alias;

	pValues = new KeyValues("Metamod Plugin");

	bKVLoaded = pValues->LoadFromFile(baseFs, file);
	if (!bKVLoaded)
	{
		delete pValues;
		return false;
	}

	if ((plugin_file = pValues->GetString("file", NULL)) == NULL)
	{
		delete pValues;
		return false;
	}

	UTIL_Format(path, path_len, "%s", plugin_file);

	if ((p_alias = pValues->GetString("alias", NULL)) != NULL)
	{
		UTIL_Format(alias, alias_len, "%s", p_alias);
	}
	else
	{
		UTIL_Format(alias, alias_len, "");
	}

	delete pValues;

	return true;
}

int Source2Provider::DetermineSourceEngine()
{
#if SOURCE_ENGINE == SE_DOTA
	return SOURCE_ENGINE_DOTA;
#elif SOURCE_ENGINE == SE_CS2
	return SOURCE_ENGINE_CS2;
#elif SOURCE_ENGINE == SE_DEADLOCK
	return SOURCE_ENGINE_DEADLOCK;
#else
#error "SOURCE_ENGINE not defined to a known value"
#endif
}

const char* Source2Provider::GetEngineDescription() const
{
#if SOURCE_ENGINE == SE_DOTA
	return "Dota 2 (2013)";
#elif SOURCE_ENGINE == SE_CS2
	return "Counter-Strike 2 (2023)";
#elif SOURCE_ENGINE == SE_DEADLOCK
	return "Deadlock (2024)";
#else
#error "SOURCE_ENGINE not defined to a known value"
#endif
}

void Source2Provider::GetGamePath(char* pszBuffer, int len)
{
	CBufferStringN<MAX_PATH> buf;
	TraceInterfaceMethod("C0.VTABLE", "IVEngineServer", "GetGameDir", engine, &IVEngineServer::GetGameDir);
	mm_LogInterfaceTrace("C0.CALL.ENTER", "method=IVEngineServer::GetGameDir object=%p", engine);
	engine->GetGameDir(buf);
	mm_LogInterfaceTrace("C0.CALL.LEAVE",
		"method=IVEngineServer::GetGameDir path=%s",
		buf.Get());
	ke::SafeSprintf(pszBuffer, len, "%s", buf.Get());
}

const char* Source2Provider::GetGameDescription()
{
#if !defined MMS_INTERFACE_TRACE
	return serverconfig->GetGameDescription();
#else
	TraceInterfaceMethod("C1.VTABLE",
		"ISource2ServerConfig",
		"GetGameDescription",
		serverconfig,
		&ISource2ServerConfig::GetGameDescription);
	mm_LogInterfaceTrace("C1.CALL.ENTER",
		"method=ISource2ServerConfig::GetGameDescription object=%p",
		serverconfig);
	const char *description = serverconfig->GetGameDescription();
	mm_LogInterfaceTrace("C1.CALL.LEAVE",
		"method=ISource2ServerConfig::GetGameDescription result=%p",
		description);
	return description;
#endif
}

#ifdef SHOULD_OVERRIDE_ALLOWDEDICATED_SERVER
KHook::Return<bool> Source2Provider::Hook_AllowDedicatedServers(const ISource2ServerConfig*, EUniverse universe)
{
	return { KHook::Action::Supersede, true };
}
#endif

const char* Source2Provider::GetCommandLineValue(const char* key, const char* defval)
{
	return CommandLine()->ParmValue(key, defval);
}

void Source2Provider::ConsolePrint(const char* str)
{
	ConMsg("%s", str);
}

void Source2Provider::ClientConsolePrint(MMSPlayer_t client, const char* message)
{
#if defined MMS_INTERFACE_TRACE
	// Called per-message at runtime, so only record the vtable slot on first use.
	static bool traced = false;
	if (!traced)
	{
		traced = true;
		TraceInterfaceMethod("C0.VTABLE",
			"IVEngineServer",
			"ClientPrintf",
			engine,
			&IVEngineServer::ClientPrintf);
	}
#endif
	engine->ClientPrintf(client, message);
}

void Source2Provider::ServerCommand(const char* cmd)
{
#if defined MMS_INTERFACE_TRACE
	static bool traced = false;
	if (!traced)
	{
		traced = true;
		TraceInterfaceMethod("C0.VTABLE",
			"IVEngineServer",
			"ServerCommand",
			engine,
			&IVEngineServer::ServerCommand);
	}
#endif
	mm_LogInterfaceTrace("C0.CALL.ENTER",
		"method=IVEngineServer::ServerCommand cmd=%s",
		cmd ? cmd : "<null>");
	engine->ServerCommand(cmd);
	mm_LogInterfaceTrace("C0.CALL.LEAVE", "method=IVEngineServer::ServerCommand");
}

const char* Source2Provider::GetConVarString(MetamodSourceConVar *convar)
{
	if (convar == NULL)
	{
		return NULL;
	}

	auto &value = reinterpret_cast<CConVar<CUtlString> *>(convar)->Get();
	return !value.IsEmpty() ? value.Get() : nullptr;
}

void Source2Provider::SetConVarString(MetamodSourceConVar *convar, const char* str)
{
	reinterpret_cast<CConVar<CUtlString> *>(convar)->Set( str );
}

bool Source2Provider::IsConCommandBaseACommand(ConCommandBase* pCommand)
{
	// This method shouldn't ever be called on s2 titles
	return false;
}

bool Source2Provider::RegisterConCommand(ProviderConCommand* pCommand)
{
	// Registration is handled by the plugins
	
	// If command has FCVAR_DEVELOPMENTONLY and is registered through our system
	// assume that it's previously plugin registered command that was hidden (on plugin reloads for example),
	// so remove that flag to make it available again
	if(pCommand->IsFlagSet( FCVAR_DEVELOPMENTONLY ))
		pCommand->RemoveFlags( FCVAR_DEVELOPMENTONLY );

	return true;
}

bool Source2Provider::RegisterConVar(ProviderConVar *pVar)
{
	// Registration is handled by the plugins
	return true;
}

void Source2Provider::UnregisterConCommand(ProviderConCommand *pCommand)
{
#if defined MMS_INTERFACE_TRACE
	static bool traced = false;
	if (!traced)
	{
		traced = true;
		TraceInterfaceMethod("C4.VTABLE",
			"ICvar",
			"UnregisterConCommandCallbacks",
			icvar,
			&ICvar::UnregisterConCommandCallbacks);
	}
#endif
	icvar->UnregisterConCommandCallbacks(*pCommand);

	// Hide command from the search, as it would still be available otherwise
	pCommand->AddFlags( FCVAR_DEVELOPMENTONLY );
}

void Source2Provider::UnregisterConVar(ProviderConVar *pVar)
{
#if defined MMS_INTERFACE_TRACE
	static bool traced = false;
	if (!traced)
	{
		traced = true;
		TraceInterfaceMethod("C4.VTABLE",
			"ICvar",
			"UnregisterConVarCallbacks",
			icvar,
			&ICvar::UnregisterConVarCallbacks);
	}
#endif
	icvar->UnregisterConVarCallbacks(*pVar);

	// Invalidate cvar for future use
	// This makes it hidden from the search and lets future convar registrations of that 
	// name to take over its setup
	pVar->GetConVarData()->Invalidate();
}

MetamodSourceConVar* Source2Provider::CreateConVar(const char* name,
	const char* defval,
	const char* help,
	int flags)
{
	uint64 newflags = 0ll;
	if (flags & ConVarFlag_Notify)
	{
		newflags |= FCVAR_NOTIFY;
	}
	if (flags & ConVarFlag_SpOnly)
	{
		newflags |= FCVAR_SPONLY;
	}

	mm_LogInterfaceTrace("C4.CVAR.NEW.ENTER",
		"method=CConVar<CUtlString> name=%s default=%s flags=0x%llx",
		name ? name : "<null>",
		defval ? defval : "<null>",
		(unsigned long long)newflags);

	CConVar<CUtlString> *pVar = new CConVar<CUtlString>( name, newflags, help, defval );

	// A failed registration leaves the ref's access index invalid. The SDK aborts on
	// this internally, but log it either way so the trace shows the cause rather than
	// stopping at ENTER with no explanation.
	mm_LogInterfaceTrace("C4.CVAR.NEW.LEAVE",
		"method=CConVar<CUtlString> name=%s convar=%p valid_ref=%d",
		name ? name : "<null>",
		pVar,
		pVar ? (int)pVar->IsValidRef() : -1);
	
	m_RegisteredConVars.push_back( pVar );

	return reinterpret_cast<MetamodSourceConVar *>(pVar);
}

class GlobCommand : public IMetamodSourceCommandInfo
{
public:
	GlobCommand(const CCommand* cmd) : m_cmd(cmd)
	{
	}
public:
	unsigned int GetArgCount()
	{
		return m_cmd->ArgC() - 1;
	}

	const char* GetArg(unsigned int num)
	{
		return m_cmd->Arg(num);
	}

	const char* GetArgString()
	{
		return m_cmd->ArgS();
	}
private:
	const CCommand* m_cmd;
};

void LocalCommand_Meta(const CCommandContext &, const CCommand& args)
{
	if (nullptr != g_Source2Provider.m_pCallbacks)
	{
		GlobCommand cmd(&args);
		g_Source2Provider.m_pCallbacks->OnCommand_Meta(&cmd);
	}
}

KHook::Return<void> Source2Provider::Hook_RegisterLoopMode(IEngineServiceMgr*, const char *pszLoopModeName, ILoopModeFactory *pLoopModeFactory, void **ppGlobalPointer)
{
	mm_LogInterfaceTrace("C7.REGISTER_LOOP_MODE.ENTER",
		"name=%s factory=%p global_pointer=%p",
		pszLoopModeName ? pszLoopModeName : "<null>",
		pLoopModeFactory,
		ppGlobalPointer);
	if (!strcmp(pszLoopModeName, "game"))
	{
		m_CreateLoopMode.Add(pLoopModeFactory);
		m_DestroyLoopMode.Add(pLoopModeFactory);
		TraceInterfaceMethod("C7.VTABLE", "ILoopModeFactory", "CreateLoopMode", pLoopModeFactory, &ILoopModeFactory::CreateLoopMode);
		TraceInterfaceMethod("C7.VTABLE", "ILoopModeFactory", "DestroyLoopMode", pLoopModeFactory, &ILoopModeFactory::DestroyLoopMode);
		mm_LogInterfaceTrace("C7.HOOK.LEAVE", "method=ILoopModeFactory::CreateLoopMode+DestroyLoopMode object=%p", pLoopModeFactory);

		if (nullptr != m_pCallbacks)
		{
			mm_LogInterfaceTrace("C7.CALLBACK.ENTER", "method=OnGameInit");
			m_pCallbacks->OnGameInit();
			mm_LogInterfaceTrace("C7.CALLBACK.LEAVE", "method=OnGameInit");
		}
	}
	mm_LogInterfaceTrace("C7.REGISTER_LOOP_MODE.LEAVE",
		"name=%s",
		pszLoopModeName ? pszLoopModeName : "<null>");

	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2Provider::Hook_UnregisterLoopMode(IEngineServiceMgr*, const char* pszLoopModeName, ILoopModeFactory* pLoopModeFactory, void** ppGlobalPointer)
{
	mm_LogInterfaceTrace("C7.UNREGISTER_LOOP_MODE.ENTER",
		"name=%s factory=%p",
		pszLoopModeName ? pszLoopModeName : "<null>",
		pLoopModeFactory);
	if (!strcmp(pszLoopModeName, "game"))
	{
		m_CreateLoopMode.Remove(pLoopModeFactory);
		m_DestroyLoopMode.Remove(pLoopModeFactory);
	}

	mm_LogInterfaceTrace("C7.UNREGISTER_LOOP_MODE.LEAVE",
		"name=%s",
		pszLoopModeName ? pszLoopModeName : "<null>");
	return { KHook::Action::Ignore };
}

KHook::Return<ILoopMode*> Source2Provider::Hook_CreateLoopModePost(ILoopModeFactory*)
{
	auto pLoopMode = *(ILoopMode**)KHook::GetOriginalValuePtr();
	mm_LogInterfaceTrace("C8.CREATE_LOOP_MODE.ENTER", "loop_mode=%p", pLoopMode);
	TraceInterfaceMethod("C8.VTABLE", "ILoopMode", "LoopInit", pLoopMode, &ILoopMode::LoopInit);
	TraceInterfaceMethod("C8.VTABLE", "ILoopMode", "LoopShutdown", pLoopMode, &ILoopMode::LoopShutdown);
	mm_LogInterfaceTrace("C8.HOOK.ENTER", "method=ILoopMode::LoopInit+LoopShutdown object=%p", pLoopMode);
	m_LoopInit.Add(pLoopMode);
	m_LoopShutdown.Add(pLoopMode);
	mm_LogInterfaceTrace("C8.HOOK.LEAVE", "method=ILoopMode::LoopInit+LoopShutdown");
	mm_LogInterfaceTrace("C8.CREATE_LOOP_MODE.LEAVE", "loop_mode=%p", pLoopMode);

	// Post-hook. Ignored
	return { KHook::Action::Ignore, nullptr };
}

KHook::Return<void> Source2Provider::Hook_DestroyLoopMode(ILoopModeFactory*, ILoopMode* pLoopMode)
{
	mm_LogInterfaceTrace("C8.DESTROY_LOOP_MODE.ENTER", "loop_mode=%p", pLoopMode);
	m_LoopInit.Remove(pLoopMode);
	m_LoopShutdown.Remove(pLoopMode);
	mm_LogInterfaceTrace("C8.DESTROY_LOOP_MODE.LEAVE", "loop_mode=%p", pLoopMode);

	return { KHook::Action::Ignore };
}

KHook::Return<bool> Source2Provider::Hook_LoopInitPost(ILoopMode*, KeyValues* pKeyValues, ILoopModePrerequisiteRegistry *pRegistry)
{
	if (!m_bInLevelInit)
	{
		return { KHook::Action::Ignore };
	}
	m_bInLevelInit = false;
	mm_LogInterfaceTrace("C8.LOOP_INIT.ENTER",
		"keyvalues=%p registry=%p",
		pKeyValues,
		pRegistry);
	if (nullptr != m_pCallbacks)
	{
#if !defined MMS_INTERFACE_TRACE
		m_pCallbacks->OnLevelInit(
			pKeyValues->GetString("levelname"),
			"",
			pKeyValues->GetString("previouslevel"),
			pKeyValues->GetString("landmarkname"),
			pKeyValues->GetBool("loadmap"),
			false
		);
#else
		// Read the KeyValues up front, separately from the callback, so the trace
		// tells a bad KeyValues layout apart from a plugin failing in OnLevelInit.
		mm_LogInterfaceTrace("C8.LOOP_INIT.KV.ENTER", "keyvalues=%p", pKeyValues);
		const char *pszLevelName = pKeyValues->GetString("levelname");
		const char *pszPrevLevel = pKeyValues->GetString("previouslevel");
		const char *pszLandmark = pKeyValues->GetString("landmarkname");
		bool bLoadMap = pKeyValues->GetBool("loadmap");
		mm_LogInterfaceTrace("C8.LOOP_INIT.KV.LEAVE",
			"level=%s prev=%s landmark=%s loadmap=%d",
			pszLevelName ? pszLevelName : "<null>",
			pszPrevLevel ? pszPrevLevel : "<null>",
			pszLandmark ? pszLandmark : "<null>",
			(int)bLoadMap);

		mm_LogInterfaceTrace("C8.CALLBACK.ENTER", "method=OnLevelInit");
		m_pCallbacks->OnLevelInit(
			pszLevelName,
			"",
			pszPrevLevel,
			pszLandmark,
			bLoadMap,
			false
		);
		mm_LogInterfaceTrace("C8.CALLBACK.LEAVE", "method=OnLevelInit");
#endif
	}
	mm_LogInterfaceTrace("C8.LOOP_INIT.LEAVE", "complete=1");

	// Post-hook. Ignored
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2Provider::Hook_LoopShutdownPost(ILoopMode*)
{
	if (m_bInLevelInit)
	{
		return { KHook::Action::Ignore };
	}
	m_bInLevelInit = true;
	mm_LogInterfaceTrace("C8.LOOP_SHUTDOWN.ENTER", "complete=0");
	if (nullptr != m_pCallbacks)
	{
		mm_LogInterfaceTrace("C8.CALLBACK.ENTER", "method=OnLevelShutdown");
		m_pCallbacks->OnLevelShutdown();
		mm_LogInterfaceTrace("C8.CALLBACK.LEAVE", "method=OnLevelShutdown");
	}
	mm_LogInterfaceTrace("C8.LOOP_SHUTDOWN.LEAVE", "complete=1");

	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2Provider::Hook_ClientCommand(IServerGameClients*, CPlayerSlot nSlot, const CCommand& _cmd)
{
	GlobCommand cmd(&_cmd);

	if (strcmp(cmd.GetArg(0), "meta") == 0)
	{
		mm_LogInterfaceTrace("C5.CLIENT_COMMAND.ENTER",
			"argc=%u args=%s",
			cmd.GetArgCount(),
			cmd.GetArgString() ? cmd.GetArgString() : "<null>");
		if (nullptr != m_pCallbacks)
		{
			m_pCallbacks->OnCommand_ClientMeta(nSlot, &cmd);
		}
		mm_LogInterfaceTrace("C5.CLIENT_COMMAND.LEAVE", "");
		
		return { KHook::Action::Supersede };
	}

	return { KHook::Action::Ignore };
}

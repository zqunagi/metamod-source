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

#include <ctime>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <chrono>
#include <thread>

#include "loader.h"
#include "serverplugin.h"
#include "gamedll.h"
#include "utility.h"
#if defined __APPLE__
#include <crt_externs.h>
#endif

static HMODULE mm_library = NULL;
static char mm_fatal_logfile[PLATFORM_MAX_PATH] = "metamod-fatal.log";
static char mm_interface_logfile[PLATFORM_MAX_PATH] = "metamod-interface.log";
MetamodBackend mm_backend = MMBackend_UNKNOWN;

static void
mm_InitInterfaceLogPath()
{
	static bool initialized = false;
	if (initialized)
		return;

	initialized = true;

	char module_path[PLATFORM_MAX_PATH];
	if (!mm_GetFileOfAddress((void *)mm_LogInterfaceTrace, module_path, sizeof(module_path)))
		return;

	char *last_slash = strrchr(module_path, PATH_SEP_CHAR);
	if (!last_slash)
		return;

	last_slash[1] = '\0';
	mm_Format(mm_interface_logfile,
			  sizeof(mm_interface_logfile),
			  "%smetamod-interface.log",
			  module_path);
}

bool
mm_IsInterfaceTraceEnabled()
{
	static int enabled = -1;
	if (enabled != -1)
		return enabled == 1;

	const char *environment = std::getenv("MMS_INTERFACE_TRACE");
	enabled = (environment && environment[0] && strcmp(environment, "0") != 0) ? 1 : 0;

	if (!enabled && mm_GetCommandArgument("-interface-trace"))
	{
		enabled = 1;
#if defined _WIN32
		SetEnvironmentVariableA("MMS_INTERFACE_TRACE", "1");
#else
		setenv("MMS_INTERFACE_TRACE", "1", 1);
#endif
	}

	return enabled == 1;
}

void
mm_LogInterfaceTrace(const char *stage, const char *message, ...)
{
	if (!mm_IsInterfaceTraceEnabled())
		return;

	mm_InitInterfaceLogPath();

	FILE *fp = fopen(mm_interface_logfile, "at");
	if (!fp && (fp = fopen("metamod-interface.log", "at")) == NULL)
		return;

	time_t now = time(NULL);
	char timestamp[64];
	strftime(timestamp, sizeof(timestamp), "%m/%d/%Y - %H:%M:%S", localtime(&now));
	static bool session_started = false;
	if (!session_started)
	{
#if defined _WIN32
		unsigned long process_id = GetCurrentProcessId();
#else
		unsigned long process_id = static_cast<unsigned long>(getpid());
#endif
		fprintf(fp,
			"L %s: [loader] [TRACE.START] pid=%lu pointer_size=%u log=%s\n",
			timestamp,
			process_id,
			(unsigned int)sizeof(void *),
			mm_interface_logfile);
		session_started = true;
	}
	fprintf(fp, "L %s: [loader] [%s] ", timestamp, stage ? stage : "TRACE");

	va_list ap;
	va_start(ap, message);
	vfprintf(fp, message, ap);
	va_end(ap);

	fputc('\n', fp);
	fflush(fp);
	fclose(fp);
}

static bool
mm_GetInterfaceTraceModuleInfo(void *address,
							   char *module_path,
							   size_t module_path_length,
							   uintptr_t *module_base)
{
	if (!address)
		return false;

#if defined _WIN32
	MEMORY_BASIC_INFORMATION memory_info;
	if (!VirtualQuery(address, &memory_info, sizeof(memory_info)) || !memory_info.AllocationBase)
		return false;

	*module_base = reinterpret_cast<uintptr_t>(memory_info.AllocationBase);
	if (!GetModuleFileNameA((HMODULE)memory_info.AllocationBase,
						module_path,
						static_cast<DWORD>(module_path_length)))
	{
		module_path[0] = '\0';
	}
	else
	{
		module_path[module_path_length - 1] = '\0';
	}
#else
	Dl_info info;
	if (!dladdr(address, &info) || !info.dli_fbase)
		return false;

	*module_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
	mm_Format(module_path,
			  module_path_length,
			  "%s",
			  info.dli_fname ? info.dli_fname : "<unknown>");
#endif

	return true;
}

void
mm_LogInterfaceVTable(const char *stage,
					  const char *interface_name,
					  const char *method_name,
					  void *object,
					  int slot,
					  void *replacement)
{
	if (!mm_IsInterfaceTraceEnabled())
		return;

	if (!object || slot < 0)
	{
		mm_LogInterfaceTrace(stage,
			"interface=%s method=%s object=%p slot=%d target=<unavailable> replacement=%p",
			interface_name,
			method_name,
			object,
			slot,
			replacement);
		return;
	}

	void **vtable = *reinterpret_cast<void ***>(object);
	void *target = vtable ? vtable[slot] : NULL;
	char module_path[PLATFORM_MAX_PATH] = "<unknown>";
	uintptr_t module_base = 0;
	unsigned long long relative_address = 0;
	if (mm_GetInterfaceTraceModuleInfo(target, module_path, sizeof(module_path), &module_base))
	{
		relative_address = static_cast<unsigned long long>(
			reinterpret_cast<uintptr_t>(target) - module_base);
	}

	mm_LogInterfaceTrace(stage,
		"interface=%s method=%s object=%p vtable=%p slot=%d target=%p module=%s rva=0x%llx replacement=%p",
		interface_name,
		method_name,
		object,
		vtable,
		slot,
		target,
		module_path,
		relative_address,
		replacement);
}

extern void
mm_LogFatal(const char *message, ...)
{
	FILE *fp;
	time_t t;
	va_list ap;
	char header[256];

	printf("MMS: Fatal error: ");
	va_start(ap, message);
	vprintf(message, ap);
	va_end(ap);
	printf("\n");

	fp = fopen(mm_fatal_logfile, "at");
	if (!fp && (fp = fopen("metamod-fatal.log", "at")) == NULL)
		return;

	t = time(NULL);
	strftime(header, sizeof(header), "%m/%d/%Y - %H:%M:%S", localtime(&t));
	fprintf(fp, "L %s: ", header);
	
	va_start(ap, message);
	vfprintf(fp, message, ap);
	va_end(ap);

	fprintf(fp, "\n");

	fclose(fp);	
}

static const char *backend_names[] =
{
	"2.ep1",
	"2.darkm",
	"2.ep2",
	"2.bgt",
	"2.eye",
	"2.css",
	"2.ep2v",
	"2.l4d",
	"2.l4d2",
	"2.swarm",
	"2.portal2",
	"2.csgo",
	"2.dota",
	"2.hl2dm",
	"2.dods",	
	"2.tf2",
	"2.nd",
	"2.sdk2013",
	"2.blade",
	"2.insurgency",
	"2.contagion",
	"2.bms",
	"2.doi",
	"2.mock",
	"2.pvkii",
	"2.mcv",
	"2.cs2",
	"2.deadlock",
	"2.outbreak",
};

#if defined _WIN32
#define LIBRARY_EXT		".dll"
#define LIBRARY_MINEXT	".dll"
#elif defined __APPLE__
#define LIBRARY_EXT		".dylib"
#define LIBRARY_MINEXT	".dylib"
#elif defined __linux__
#define LIBRARY_EXT		LIB_SUFFIX
#define LIBRARY_MINEXT	".so"
#endif

bool
mm_LoadMetamodLibrary(MetamodBackend backend, char *buffer, size_t maxlength)
{
#ifdef KHOOK_DEBUG
	std::cout << "Pausing 10 seconds to allow debugger to attach" << std::endl;
	std::this_thread::sleep_for(std::chrono::seconds(10));
#endif

	size_t len, temp_len;
	char mm_path[PLATFORM_MAX_PATH * 2];

	/* Get our path */
	if (!mm_GetFileOfAddress((void*)mm_GetFileOfAddress, mm_path, sizeof(mm_path)))
		return false;

	len = strlen(mm_path);
	
	const char *pLastSlash = strrchr(mm_path, PATH_SEP_CHAR);
	if (!pLastSlash)
		return false;

	temp_len = strlen(&pLastSlash[1]);
	if (len < temp_len)
		return false;

	/* Build log file name */
	mm_path[len - temp_len] = '\0';
	mm_Format(mm_fatal_logfile,
			  sizeof(mm_fatal_logfile),
			  "%smetamod-fatal.log",
			  mm_path);

	/* Replace server.dll with the new binary we want */
	mm_Format(&mm_path[len - temp_len],
			  sizeof(mm_path) - (len - temp_len),
			  "metamod.%s" LIBRARY_MINEXT,
			  backend_names[backend]);

	mm_LogInterfaceTrace("L2.CORE.LOAD.ENTER",
		"backend=%d path=%s",
		backend,
		mm_path);
	mm_library = (HMODULE)mm_LoadLibrary(mm_path, buffer, maxlength);
	mm_LogInterfaceTrace("L2.CORE.LOAD.LEAVE",
		"backend=%d path=%s handle=%p error=%s",
		backend,
		mm_path,
		mm_library,
		mm_library ? "<none>" : buffer);

	return (mm_library != NULL);
}

void
mm_UnloadMetamodLibrary()
{
	mm_UnloadLibrary(mm_library);
	mm_library = NULL;
}

#if defined _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#elif defined __GNUC__
#define EXPORT extern "C" __attribute__ ((visibility("default")))
#endif

EXPORT void *
CreateInterface(const char *name, int *ret)
{
	mm_LogInterfaceTrace("L.API.QUERY.ENTER", "name=%s", name ? name : "<null>");

	/* If we've got a VSP bridge, do nothing. */
	if (vsp_bridge != NULL)
	{
		if (ret != NULL)
			*ret = 1;
		mm_LogInterfaceTrace("L.API.QUERY.LEAVE",
			"name=%s result=<null> ret=%d reason=vsp-bridge",
			name ? name : "<null>",
			ret ? *ret : -1);
		return NULL;
	}

	void *ptr;
	if (strncmp(name, "ISERVERPLUGINCALLBACKS", 22) == 0)
	{
		/* Either load as VSP or start VSP listener */
		ptr = mm_GetVspCallbacks(atoi(&name[22]));
	}
	else if (gamedll_bridge == NULL)
	{
		/* Load as gamedll */
		ptr = mm_GameDllRequest(name, ret);
	}
	else
	{
		/* If we've got a gamedll bridge, forward the request. */
		ptr = gamedll_bridge->QueryInterface(name, ret);
		mm_LogInterfaceTrace("L.API.QUERY.LEAVE",
			"name=%s result=%p ret=%d source=gamedll-bridge",
			name ? name : "<null>",
			ptr,
			ret ? *ret : -1);
		return ptr;
	}

	if (ret != NULL)
		*ret = (ptr != NULL) ? 0 : 1;
	mm_LogInterfaceTrace("L.API.QUERY.LEAVE",
		"name=%s result=%p ret=%d",
		name ? name : "<null>",
		ptr,
		ret ? *ret : -1);

	return ptr;
}

void *
mm_GetProcAddress(const char *name)
{
	return mm_GetLibAddress(mm_library, name);
}

typedef const char *(*GetGameInfoStringFn)(const char *pszKeyName, const char *pszDefaultValue, char *pszOut, uint64_t cbOut);

void
mm_GetGameName(char *buffer, size_t size)
{
	if (!mm_GetCommandArgument("-game", buffer, size))
	{
		// Source 2 doesn't ever use -game, so we'll hardcode by app id for now. This same approach
		// won't work for the few Source 1 games that don't require -game, as S1 initializes Steam
		// too late (although the env var still still be already set if their is a running Steam client
		// installed). We previously called GetGameInfoString exported from tier0 to look up the first Mod
		// dir defined. While that worked for CS2 and Dota 2, Deadlock does not define any Mod paths, solely
		// relying on Game paths. The function only returns the first path defined, and in the case of S2, where
		// we can't even set MM:S path with GameBin instead of Game, the first Game path will always be MM:S's
		// location, rather than the real Game dir
		char *pszAppId = std::getenv("SteamAppId");
		if (pszAppId)
		{
			switch (strtoul(pszAppId, nullptr, 10))
			{
				case 570ul:
					strncpy(buffer, "dota", size);
					break;
				case 730ul:
					strncpy(buffer, "csgo", size);
					break;
				case 1422450ul:
					strncpy(buffer, "citadel", size);
					break;
			}
		}
	}

	if (buffer[0] == 0)
	{
		strncpy(buffer, ".", size);
	}
}

MetamodBackend
mm_DetermineBackendS1(QueryValveInterface engineFactory, QueryValveInterface serverFactory, const char *game_name)
{
	if (engineFactory("VEngineServer023", NULL) != NULL)
	{
		if (engineFactory("EngineTraceServer004", NULL) == NULL)
		{
			goto TF2branch;
		}

		if (engineFactory("IEngineSoundServer004", NULL) != NULL)
		{
			void *lib = (void *)serverFactory;
			void *addr;
			if (strcmp(game_name, "doi") == 0
				|| (addr = mm_FindPattern(lib, "doi_gamerules_data", sizeof("doi_gamerules_data") - 1)))
			{
				return MMBackend_DOI;
			}
			
			return MMBackend_Insurgency;
		}
		
		if (serverFactory("ServerGameDLL010", NULL) != NULL)
		{
			return MMBackend_BMS;
		}

		if (serverFactory("VSERVERTOOLS003", NULL) != NULL)
		{
			if (engineFactory("VFileSystem018", NULL) != NULL)
			{
				return MMBackend_Outbreak;
			}

			return MMBackend_Blade;
		}

		if (strcmp(game_name, "vietnam") == 0)
		{
			return MMBackend_MCV;
		}
		
		return MMBackend_CSGO;
	}
	else if (engineFactory("VEngineServer022", NULL) != NULL &&
		engineFactory("VEngineCvar007", NULL) != NULL)
	{
		if (engineFactory("EngineTraceServer004", NULL) != NULL)
		{
			if (engineFactory("XboxSystemInterface001", NULL) != NULL)
			{
				return MMBackend_AlienSwarm;
			}
			
			void *lib = (void *)serverFactory;
			void *addr;
			if (strcmp(game_name, "portal2") == 0
				|| (addr = mm_FindPattern(lib, "baseportalcombatweapon", sizeof("baseportalcombatweapon") - 1)))
			{
				return MMBackend_Portal2;
			}

			return MMBackend_Blade; // Old Blade
		}
		else if (engineFactory("VPrecacheSystem001", NULL) != NULL)
		{
			if (serverFactory("ServerGameTags002", NULL) != NULL)
			{
				return MMBackend_NuclearDawn;
			}
			else
			{
				void *lib = (void *)serverFactory;
				void *addr;
				if (strcmp(game_name, "contagion") == 0
					|| (addr = mm_FindPattern(lib, "Contagion_Chat_All", sizeof("Contagion_Chat_All") - 1)))
				{
					return MMBackend_Contagion;
				}
				else
				{
					return MMBackend_Left4Dead2;
				}
			}
		}

		return MMBackend_Left4Dead;
	}
	else if (engineFactory("VEngineServer021", NULL) != NULL)
	{
		/* Check for OB */
		if (engineFactory("VEngineCvar004", NULL) != NULL)
		{
			if (engineFactory("VModelInfoServer002", NULL) != NULL)
			{
				/* BGT has same iface version numbers and libs as ep2 */
				void *lib = (void *)serverFactory;
				void *addr;
				if (strcmp(game_name, "pm") == 0
					|| (addr = mm_FindPattern(lib, "DT_PMPlayerResource", sizeof("DT_PMPlayerResource") - 1)))
				{
					return MMBackend_BloodyGoodTime;
				}
				else
				{
					return MMBackend_Episode2;
				}
			}
			else if (engineFactory("VModelInfoServer003", NULL) != NULL)
			{
				if (engineFactory("VFileSystem017", NULL) != NULL)
				{
					return MMBackend_EYE;
				}
				else
				{
	TF2branch:
					void *lib = (void *)serverFactory;
					void *addr;
					if (strcmp(game_name, "cstrike") == 0
						|| (addr = mm_FindPattern(lib, "DT_CSPlayerResource", sizeof("DT_CSPlayerResource") - 1)))
					{
						return MMBackend_CSS;
					}
					else if (strcmp(game_name, "tf") == 0
						|| (addr = mm_FindPattern(lib, "DT_TFPlayerResource", sizeof("DT_TFPlayerResource") - 1)))
					{
						return MMBackend_TF2;
					}
					else if (strcmp(game_name, "dod") == 0
						|| (addr = mm_FindPattern(lib, "DT_DODPlayerResource", sizeof("DT_DODPlayerResource") - 1)))
					{
						return MMBackend_DODS;
					}
					else if (strcmp(game_name, "hl2mp") == 0)
					{
						return MMBackend_HL2DM;
					}
					else if (strcmp(game_name, "pvkii") == 0)
					{
						return MMBackend_PVKII;
					}
					else if (strcmp(game_name, ".") == 0 && engineFactory("MOCK_ENGINE", NULL))
					{
						return MMBackend_Mock;
					}
					else if (serverFactory("ServerGameClients005", NULL) != nullptr)
					{
						// 2025 version of SDK 2013, or maybe hl1mp, or anything else shaped like those.
						// We may later make a separate SDK for this branch. For now, they match, we'll hack it
						return MMBackend_HL2DM;
					}
					else
					{
						return MMBackend_SDK2013;
					}
				}
			}
		}
		/* Check for Episode One/Old Engine */
		else if (engineFactory("VModelInfoServer001", NULL) != NULL &&
				 (engineFactory("VEngineCvar003", NULL) != NULL ||
				  engineFactory("VEngineCvar002", NULL) != NULL))
		{
			/* Check for Dark Messiah which has a weird directory structure */
			if (strcmp(game_name, ".") == 0)
			{
				return MMBackend_DarkMessiah;
			}
			return MMBackend_Episode1;
		}
	}

	return MMBackend_UNKNOWN;
}


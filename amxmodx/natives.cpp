/* AMX Mod X 
*
* by the AMX Mod X Development Team
*  originally developed by OLO
*
*  This program is free software; you can redistribute it and/or modify it
*  under the terms of the GNU General Public License as published by the
*  Free Software Foundation; either version 2 of the License, or (at
*  your option) any later version.
*
*  This program is distributed in the hope that it will be useful, but
*  WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
*  General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program; if not, write to the Free Software Foundation,
*  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*
*  In addition, as a special exception, the author gives permission to
*  link the code of this program with the Half-Life Game Engine ("HL
*  Engine") and Modified Game Libraries ("MODs") developed by Valve,
*  L.L.C ("Valve"). You must obey the GNU General Public License in all
*  respects for all of the code used other than the HL Engine and MODs
*  from Valve. If you modify this file, you may extend this exception
*  to your version of the file, but you are not obligated to do so. If
*  you do not wish to do so, delete this exception statement from your
*  version.
*/

#include "amxmodx.h"
#include "sh_stack.h"
#include "natives.h"
#include "debugger.h"
#include "libraries.h"
#include "format.h"

#ifdef __linux__
#include <malloc.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "sclinux.h"
#endif

//Written by David "BAILOPAN" Anderson
//With the exception for param_convert, which was written by
// Julien "dJeyL" Laurent

CStack<int> g_ErrorStk;
CVector<regnative *> g_RegNatives;
CStack<regnative *> g_NativeStack;
static char g_errorStr[512] = {0};
bool g_Initialized = false;

int amxx_DynaCallback(int idx, AMX *amx, cell *params)
{
	if (idx < 0 || idx >= (int)g_RegNatives.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Invalid dynamic native called");
		return 0;
	}

	regnative *pNative = g_RegNatives[idx];
	int numParams = params[0] / sizeof(cell);

	if (numParams > CALLFUNC_MAXPARAMS)
	{
		LogError(amx, AMX_ERR_NATIVE, "Called dynanative with too many parameters (%d)", CALLFUNC_MAXPARAMS);
		return 0;
	}

	CPluginMngr::CPlugin *pPlugin = g_plugins.findPluginFast(amx);
	CPluginMngr::CPlugin *pNativePlugin = g_plugins.findPluginFast(pNative->amx);

	if (!pNativePlugin->isExecutable(pNative->func))
	{
		LogError(amx, AMX_ERR_NATIVE, "Called dynanative into a paused plugin.");
		pPlugin->setStatus(ps_paused);
		return 0;
	}

	if (pNative->caller)
	{
		LogError(amx, AMX_ERR_NATIVE, "Bug caught! Please contact the AMX Mod X Dev Team.");
		return 0;
	}

	//parameter stack
	//NOTE: it is possible that recursive register native calling
	// could potentially be somehow damaged here.
	//so, a :TODO: - make the stack unique, rather than a known ptr
	pNative->caller = amx;

	int err = 0;
	cell ret = 0;
	g_ErrorStk.push(0);
	g_NativeStack.push(pNative);
	if (pNative->style == 0)
	{
		amx_Push(pNative->amx, numParams);
		amx_Push(pNative->amx, pPlugin->getId());
		for (int i=numParams; i>=0; i--)
			pNative->params[i] = params[i];
	} else if (pNative->style == 1) {
		//use dJeyL's system .. very clever!
		for (int i=numParams; i>=1; i--)
			amx_Push(pNative->amx, params[i]);
	}
	Debugger *pDebugger = (Debugger *)pNative->amx->userdata[UD_DEBUGGER];
	if (pDebugger)
		pDebugger->BeginExec();
	err=amx_Exec(pNative->amx, &ret, pNative->func);
	if (err != AMX_ERR_NONE)
	{
		if (pDebugger && pDebugger->ErrorExists())
		{
			//don't care
		} else if (err != -1) {
			//nothing logged the error
			LogError(pNative->amx, err, NULL);
		}
		pNative->amx->error = AMX_ERR_NONE;
		//furthermore, log an error in the parent plugin.
		LogError(amx, AMX_ERR_NATIVE, "Unhandled dynamic native error");
	} else if (g_ErrorStk.front()) {
		LogError(amx, g_ErrorStk.front(), g_errorStr);
	}
	if (pDebugger)
		pDebugger->EndExec();
	g_NativeStack.pop();
	g_ErrorStk.pop();

	pNative->caller = NULL;

	return ret;
}

AMX_NATIVE_INFO *BuildNativeTable()
{
	if (g_RegNatives.size() < 1)
		return NULL;

	AMX_NATIVE_INFO *pNatives = new AMX_NATIVE_INFO[g_RegNatives.size() + 1];

	AMX_NATIVE_INFO info;
	regnative *pNative;
	for (size_t i=0; i<g_RegNatives.size(); i++)
	{
		pNative = g_RegNatives[i];
		info.name = pNative->name.c_str();
		info.func = (AMX_NATIVE)((void *)(pNative->pfn));
		pNatives[i] = info;
	}
	pNatives[g_RegNatives.size()].name = NULL;
	pNatives[g_RegNatives.size()].func = NULL;

	//this needs to be deleted
	return pNatives;
}

static cell AMX_NATIVE_CALL log_error(AMX *amx, cell *params)
{
	int len;
	char *err = format_amxstring(amx, params, 2, len);

	_snprintf(g_errorStr, sizeof(g_errorStr), "%s", err);
	g_ErrorStk.pop();
	g_ErrorStk.push(params[1]);

	return 1;
}

//get_string(param, dest[], len)
static cell AMX_NATIVE_CALL get_string(AMX *amx, cell *params)
{
	if (!g_NativeStack.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Not currently in a dynamic native");
		return 0;
	}
	regnative *pNative = g_NativeStack.front();
	if (pNative->style)
	{
		LogError(amx, AMX_ERR_NATIVE, "Wrong style of dynamic native");
		return 0;
	}
	int p = params[1];

	int len;
	char *str = get_amxstring(pNative->caller, pNative->params[p], 0, len);
	return set_amxstring(amx, params[2], str, params[3]);
}

//set_string(param, source[], maxlen)
static cell AMX_NATIVE_CALL set_string(AMX *amx, cell *params)
{
	if (!g_NativeStack.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Not currently in a dynamic native");
		return 0;
	}
	regnative *pNative = g_NativeStack.front();
	if (pNative->style)
	{
		LogError(amx, AMX_ERR_NATIVE, "Wrong style of dynamic native");
		return 0;
	}
	int p = params[1];

	int len;
	char *str = get_amxstring(amx, params[2], 0, len);

	return set_amxstring(pNative->caller, pNative->params[p], str, params[3]);
}

//get a byvalue parameter
//get_param(num)
static cell AMX_NATIVE_CALL get_param(AMX *amx, cell *params)
{
	if (!g_NativeStack.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Not currently in a dynamic native");
		return 0;
	}
	regnative *pNative = g_NativeStack.front();
	if (pNative->style)
	{
		LogError(amx, AMX_ERR_NATIVE, "Wrong style of dynamic native");
		return 0;
	}
	int p = params[1];

	return pNative->params[p];
}

//get_param_byref(num)
static cell AMX_NATIVE_CALL get_param_byref(AMX *amx, cell *params)
{
	if (!g_NativeStack.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Not currently in a dynamic native");
		return 0;
	}
	regnative *pNative = g_NativeStack.front();
	if (pNative->style)
	{
		LogError(amx, AMX_ERR_NATIVE, "Wrong style of dynamic native");
		return 0;
	}
	int p = params[1];

	cell *addr = get_amxaddr(pNative->caller, pNative->params[p]);

	return addr[0];
}

//set_param_byref(num, val)
static cell AMX_NATIVE_CALL set_param_byref(AMX *amx, cell *params)
{
	if (!g_NativeStack.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Not currently in a dynamic native");
		return 0;
	}
	regnative *pNative = g_NativeStack.front();
	if (pNative->style)
	{
		LogError(amx, AMX_ERR_NATIVE, "Wrong style of dynamic native");
		return 0;
	}
	int p = params[1];

	cell *addr = get_amxaddr(pNative->caller, pNative->params[p]);

	addr[0] = params[2];

	return 1;
}

//get_array(param, dest[], size)
static cell AMX_NATIVE_CALL get_array(AMX *amx, cell *params)
{
	if (!g_NativeStack.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Not currently in a dynamic native");
		return 0;
	}
	regnative *pNative = g_NativeStack.front();
	if (pNative->style)
	{
		LogError(amx, AMX_ERR_NATIVE, "Wrong style of dynamic native");
		return 0;
	}
	int p = params[1];

	cell *source = get_amxaddr(pNative->caller, pNative->params[p]);
	cell *dest = get_amxaddr(amx, params[2]);

	int size = params[3];

	memcpy(dest, source, size * sizeof(cell));

	return 1;
}

//set_array(param, source[], size)
static cell AMX_NATIVE_CALL set_array(AMX *amx, cell *params)
{
	if (!g_NativeStack.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Not currently in a dynamic native");
		return 0;
	}
	regnative *pNative = g_NativeStack.front();
	if (pNative->style)
	{
		LogError(amx, AMX_ERR_NATIVE, "Wrong style of dynamic native");
		return 0;
	}
	int p = params[1];

	cell *dest = get_amxaddr(pNative->caller, pNative->params[p]);
	cell *source = get_amxaddr(amx, params[2]);

	int size = params[3];

	memcpy(dest, source, size * sizeof(cell));

	return 1;
}

static cell AMX_NATIVE_CALL vdformat(AMX *amx, cell *params)
{
	if (!g_NativeStack.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Not currently in a dynamic native");
		return 0;
	}

	regnative *pNative = g_NativeStack.front();

	if (pNative->style)
	{
		LogError(amx, AMX_ERR_NATIVE, "Wrong style of dynamic native");
		return 0;
	}

	int vargPos = static_cast<int>(params[4]);
	int fargPos = static_cast<int>(params[3]);

	/** get the parent parameter array */
	cell *local_params = pNative->params;

	cell max = local_params[0] / sizeof(cell);
	if (vargPos > (int)max + 1)
	{
		LogError(amx, AMX_ERR_NATIVE, "Invalid vararg parameter passed: %d", vargPos);
		return 0;
	}
	if (fargPos > (int)max + 1)
	{
		LogError(amx, AMX_ERR_NATIVE, "Invalid fmtarg parameter passed: %d", fargPos);
		return 0;
	}

	/* get destination info */
	cell *fmt;
	if (fargPos == 0)
	{
		if (params[0] / sizeof(cell) != 5)
		{
			LogError(amx, AMX_ERR_NATIVE, "Expected fmtarg as fifth parameter, found none");
			return 0;
		}
		fmt = get_amxaddr(amx, params[5]);
	} else {
		fmt = get_amxaddr(pNative->caller, pNative->params[fargPos]);
	}
	cell *realdest = get_amxaddr(amx, params[1]);
	size_t maxlen = static_cast<size_t>(params[2]);
	cell *dest = realdest;

	/* if this is necessary... */
	static cell cpbuf[4096];
	dest = cpbuf;

	/* perform format */
	size_t total = atcprintf(dest, maxlen, fmt, pNative->caller, local_params, &vargPos);

	/* copy back */
	memcpy(realdest, dest, (total+1) * sizeof(cell));

	return total;
}

//This is basically right from dJeyL's lib_convert function
//This awesome hack modifies the stack frame to have an address offset
// that will align to the other plugin's memory.
//I've no idea how he thought of this, but it's great.  No idea how well it works.
static cell AMX_NATIVE_CALL param_convert(AMX *amx, cell *params)
{
	if (!g_NativeStack.size())
	{
		LogError(amx, AMX_ERR_NATIVE, "Not currently in a dynamic native");
		return 0;
	}
	regnative *pNative = g_NativeStack.front();
	if (pNative->style != 1)
	{
		LogError(amx, AMX_ERR_NATIVE, "Wrong style of dynamic native");
		return 0;
	}
	cell p = params[1];

	AMX *caller = pNative->caller;

	unsigned char *data =amx->base+(int)((AMX_HEADER *)amx->base)->dat;
	unsigned char *realdata = caller->base+(int)((AMX_HEADER *)caller->base)->dat;

	* (cell *)(data+(int)amx->frm+(p+2)*sizeof(cell)) -= (cell)data-(cell)realdata;

	return 1;
}

static cell AMX_NATIVE_CALL register_library(AMX *amx, cell *params)
{
	int len;
	char *lib = get_amxstring(amx, params[1], 0, len);

	AddLibrary(lib, LibType_Library, LibSource_Plugin, g_plugins.findPluginFast(amx));

	return 1;
}

//register_native(const name[], const handler[])
static cell AMX_NATIVE_CALL register_native(AMX *amx, cell *params)
{
	if (!g_Initialized)
		amxx_DynaInit((void *)(amxx_DynaCallback));

	g_Initialized = true;

	int len;
	char *name = get_amxstring(amx, params[1], 0, len);
	char *func = get_amxstring(amx, params[2], 1, len);

	int idx, err;
	if ( (err=amx_FindPublic(amx, func, &idx)) != AMX_ERR_NONE)
	{
		LogError(amx, err, "Function \"%s\" was not found", func);
		return 0;
	}

	regnative *pNative = new regnative;
	pNative->amx = amx;
	pNative->func = idx;
	pNative->caller = NULL;
	
	//we'll apply a safety buffer too
	//make our function
	int size = amxx_DynaCodesize();
#ifndef __linux__
	DWORD temp;
	pNative->pfn = new char[size + 10];
	VirtualProtect(pNative->pfn, size+10, PAGE_EXECUTE_READWRITE, &temp);
#else
	pNative->pfn = (char *)memalign(sysconf(_SC_PAGESIZE), size+10);
	mprotect((void *)pNative->pfn, size+10, PROT_READ|PROT_WRITE|PROT_EXEC);
#endif

	int id = (int)g_RegNatives.size();
	
	amxx_DynaMake(pNative->pfn, id);
	pNative->func = idx;
	pNative->style = params[3];

	g_RegNatives.push_back(pNative);

	pNative->name.assign(name);

	return 1;
}

void ClearPluginLibraries()
{
	ClearLibraries(LibSource_Plugin);
	for (size_t i=0; i<g_RegNatives.size(); i++)
	{
		delete [] g_RegNatives[i]->pfn;
		delete g_RegNatives[i];
	}
	g_RegNatives.clear();
}

AMX_NATIVE_INFO g_NativeNatives[] = {
	{"register_native",	register_native},
	{"log_error",		log_error},
	{"register_library",register_library},
	{"get_string",		get_string},
	{"set_string",		set_string},
	{"get_param",		get_param},
	{"get_param_byref",	get_param_byref},
	{"set_param_byref",	set_param_byref},
	{"get_array",		get_array},
	{"set_array",		set_array},
	//these are dummy functions for floats ;p
	{"get_param_f",		get_param},
	{"get_float_byref",	get_param_byref},
	{"set_float_byref", set_param_byref},
	{"get_array_f",		get_array},
	{"set_array_f",		set_array},
	{"vdformat",		vdformat},
	{"param_convert",	param_convert},
	//////////////////////////
	{NULL,				NULL},
};

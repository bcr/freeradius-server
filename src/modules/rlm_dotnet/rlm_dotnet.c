/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * @file rlm_dotnet.c
 * @brief Translates requests between the server and .NET Core.
 *
 * @author Blake Ramsdell <blake.ramsdell@onelogin.com>
 *
 * @copyright 2019 OneLogin, Inc.
 * @copyright 1999-2013 The FreeRADIUS Server Project.
 */

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <dlfcn.h>
#include "coreclrhost.h"
#include "clrpath.h"

#define DEFAULT_CLR_LIBRARY	"libcoreclr.dylib"

/** Specifies the module.function to load for processing a section
 *
 */
typedef struct dotnet_func_def {
	void* function;

	char const	*assembly_name;		//!< String name of assembly.
	char const	*class_name;		//!< String name of class in assembly.
	char const	*function_name;		//!< String name of function in class.
} dotnet_func_def_t;

typedef struct rlm_dotnet_t {
	void *dylib;
	void *hostHandle;
	unsigned int domainId;
	coreclr_initialize_ptr coreclr_initialize;
	coreclr_create_delegate_ptr coreclr_create_delegate;
	coreclr_shutdown_2_ptr coreclr_shutdown_2;

	char const	*clr_library;		//!< Path to CLR library.

	dotnet_func_def_t
	instantiate,
	authorize,
	authenticate,
	preacct,
	accounting,
	checksimul,
	pre_proxy,
	post_proxy,
	post_auth,
#ifdef WITH_COA
	recv_coa,
	send_coa,
#endif
	detach;
} rlm_dotnet_t;

static const CONF_PARSER module_config[] = {
#define A(x) { "asm_" #x, FR_CONF_OFFSET(PW_TYPE_STRING, rlm_dotnet_t, x.assembly_name), "${.assembly}" }, \
	{ "class_" #x, FR_CONF_OFFSET(PW_TYPE_STRING, rlm_dotnet_t, x.class_name), "${.class}" }, \
	{ "func_" #x, FR_CONF_OFFSET(PW_TYPE_STRING, rlm_dotnet_t, x.function_name), NULL },

	A(instantiate)
	A(authorize)
	A(authenticate)
	A(preacct)
	A(accounting)
	A(checksimul)
	A(pre_proxy)
	A(post_proxy)
	A(post_auth)
#ifdef WITH_COA
	A(recv_coa)
	A(send_coa)
#endif
	A(detach)

#undef A

	{ "clr_library", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_dotnet_t, clr_library), DEFAULT_CLR_LIBRARY },

	CONF_PARSER_TERMINATOR
};

static int bind_dotnet(rlm_dotnet_t *inst)
{
	// Do dlopen
	inst->dylib = dlopen(inst->clr_library, RTLD_NOW | RTLD_GLOBAL);
	if (!inst->dylib)
	{
		ERROR("%s", dlerror());
		return 1;
	}

	// Find the relevant methods we want
#define A(x)	inst->x = dlsym(inst->dylib, #x); \
				if (!inst->x) ERROR("%s", dlerror());

	A(coreclr_initialize)
	A(coreclr_create_delegate)
	A(coreclr_shutdown_2)
#undef A

	return 0;
}

static int bind_one_method(rlm_dotnet_t *inst, dotnet_func_def_t *function_definition, char const *function_name)
{
	int rc = 0;
	if (function_definition->function_name)
	{
		DEBUG("binding %s to %s %s %s", function_name, function_definition->assembly_name, function_definition->class_name, function_definition->function_name);
		rc = inst->coreclr_create_delegate(
			inst->hostHandle,
			inst->domainId,
			function_definition->assembly_name,
			function_definition->class_name,
			function_definition->function_name,
			&function_definition->function
			);
		if (rc)
		{
			ERROR("Failure binding %s to %s %s %s, coreclr_create_delegate returned 0x%08X", function_name, function_definition->assembly_name, function_definition->class_name, function_definition->function_name, rc);
		}
		else
		{
			DEBUG("Bound it! Function is %p", function_definition->function);
		}
		
	}

	return rc;
}

/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 *
 */
static int mod_instantiate(CONF_SECTION *conf, void *instance)
{
	rlm_dotnet_t	*inst = instance;

	DEBUG("mod_instantiate");
	if (bind_dotnet(inst))
	{
		ERROR("Failed to load .NET core");
		return RLM_MODULE_FAIL;
	}

	const char* propertyKeys[] = {
		"TRUSTED_PLATFORM_ASSEMBLIES"
	};
    const char* propertyValues[] = {
        CLR_PATH
    };

	int hr = inst->coreclr_initialize(
		"/Users/blakeramsdell/Source/OpenSource/freeradius-server",
		"FreeRadius",
		sizeof(propertyKeys) / sizeof(char*),
		propertyKeys,
		propertyValues,
		&inst->hostHandle,
		&inst->domainId
		);

	// Check hr for failure
	if (hr == 0)
	{
		// Bind up all of our C# methods
#define A(x) bind_one_method(inst, &inst->x, #x);
		A(instantiate)
		A(authorize)
		A(authenticate)
		A(preacct)
		A(accounting)
		A(checksimul)
		A(pre_proxy)
		A(post_proxy)
		A(post_auth)
#ifdef WITH_COA
		A(recv_coa)
		A(send_coa)
#endif
		A(detach)

#undef A
	}
	else
	{
		ERROR("Failed coreclr_initialize hr = 0x%08X", hr);
	}
	return 0;
}

static int mod_detach(void *instance)
{
	rlm_dotnet_t *inst = instance;

	int latchedExitCode = 0;
	int hr = inst->coreclr_shutdown_2(inst->hostHandle, inst->domainId, &latchedExitCode);
	INFO("coreclr_shutdown_2 hr = 0x%08X latchedExitCode = 0x%08X", hr, latchedExitCode);
	return 0;
}

static rlm_rcode_t do_dotnet(rlm_dotnet_t *inst, REQUEST *request, void *pFunc, char const *funcname)
{
	return RLM_MODULE_NOOP;
}

#define MOD_FUNC(x) \
static rlm_rcode_t CC_HINT(nonnull) mod_##x(void *instance, REQUEST *request) { \
	return do_dotnet((rlm_dotnet_t *) instance, request, ((rlm_dotnet_t *)instance)->x.function, #x);\
}

MOD_FUNC(authenticate)
MOD_FUNC(authorize)
MOD_FUNC(preacct)
MOD_FUNC(accounting)
MOD_FUNC(checksimul)
MOD_FUNC(pre_proxy)
MOD_FUNC(post_proxy)
MOD_FUNC(post_auth)
#ifdef WITH_COA
MOD_FUNC(recv_coa)
MOD_FUNC(send_coa)
#endif

extern module_t rlm_dotnet;
module_t rlm_dotnet = {
	.magic		= RLM_MODULE_INIT,
	.name		= "dotnet",
	.type		= RLM_TYPE_THREAD_UNSAFE,
	.inst_size	= sizeof(rlm_dotnet_t),
	.config		= module_config,
	.instantiate	= mod_instantiate,
	.detach		= mod_detach,
	.methods = {
		[MOD_AUTHENTICATE]	= mod_authenticate,
		[MOD_AUTHORIZE]		= mod_authorize,
		[MOD_PREACCT]		= mod_preacct,
		[MOD_ACCOUNTING]	= mod_accounting,
		[MOD_SESSION]		= mod_checksimul,
		[MOD_PRE_PROXY]		= mod_pre_proxy,
		[MOD_POST_PROXY]	= mod_post_proxy,
		[MOD_POST_AUTH]		= mod_post_auth,
#ifdef WITH_COA
		[MOD_RECV_COA]		= mod_recv_coa,
		[MOD_SEND_COA]		= mod_send_coa
#endif
	}
};

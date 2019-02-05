/*
 * @mindmaze_header@
 */

#if defined (HAVE_CONFIG_H)
# include <config.h>
#endif

#include "mmpack-install.h"

#include <mmargparse.h>
#include <mmerrno.h>
#include <mmlib.h>
#include <mmsysio.h>
#include <string.h>
#include "context.h"
#include "package-utils.h"
#include "pkg-fs-utils.h"


static int is_yes_assumed = 0;

static char install_doc[] =
	"\"mmpack install\" downloads and installs given packages and "
	"their dependencies into the current prefix. If mmpack finds "
	"missing systems dependencies, then it will abort the installation "
	"and request said packages.";

static const struct mmarg_opt cmdline_optv[] = {
	{"y|assume-yes", MMOPT_NOVAL|MMOPT_INT, "1", {.iptr = &is_yes_assumed},
	 "assume \"yes\" as answer to all prompts and run non-interactively"},
};


static
void fill_pkgreq_from_cmdarg(struct pkg_request *req, const char* arg)
{
	const char* v;

	// Find the first occurrence of '='
	v = strchr(arg, '=');
	if (v != NULL) {
		// The package name is before the '=' character
		req->name = mmstr_malloc_copy(arg, v - arg);
		req->version = mmstr_malloc_from_cstr(v+1);
	} else {
		req->name = mmstr_malloc_from_cstr(arg);
		req->version = NULL;
	}
}


LOCAL_SYMBOL
int mmpack_install(struct mmpack_ctx * ctx, int argc, const char* argv[])
{
	struct pkg_request* reqlist = NULL;
	struct action_stack* act_stack = NULL;
	int i, nreq, arg_index, rv = -1;
	const char** req_args;
	struct mmarg_parser parser = {
		.doc = install_doc,
		.args_doc = INSTALL_SYNOPSIS,
		.optv = cmdline_optv,
		.num_opt = MM_NELEM(cmdline_optv),
		.execname = "mmpack",
	};

	arg_index = mmarg_parse(&parser, argc, (char**)argv);
	if (arg_index+1 > argc) {
		fprintf(stderr, "missing package list argument in command line\n"
		                "Run \"mmpack install --help\" to see usage\n");
		return -1;
	}

	nreq = argc - arg_index;
	req_args = argv + arg_index;

	// Load prefix configuration and caches
	if (mmpack_ctx_use_prefix(ctx, 0))
		goto exit;

	// Fill package requested to be installed from cmd arguments
	reqlist = mm_malloca(nreq * sizeof(*reqlist));
	memset(reqlist, 0, nreq * sizeof(*reqlist));
	for (i = 0; i < nreq; i++) {
		fill_pkgreq_from_cmdarg(&reqlist[i], req_args[i]);
		reqlist[i].next = (i == nreq-1) ? NULL : &reqlist[i+1];
	}

	// Determine the stack of actions to perform
	act_stack = mmpkg_get_install_list(ctx, reqlist);
	if (!act_stack)
		goto exit;

	if (!is_yes_assumed) {
		rv = confirm_action_stack_if_needed(nreq, act_stack);
		if (rv != 0)
			goto exit;
	}

	rv = apply_action_stack(ctx, act_stack);

exit:
	mmpack_action_stack_destroy(act_stack);
	for (i = 0; i < nreq && reqlist; i++) {
		mmstr_free(reqlist[i].name);
		mmstr_free(reqlist[i].version);
	}
	mm_freea(reqlist);
	return rv;
}
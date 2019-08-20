/*
 * @mindmaze_header@
 */

#if defined (HAVE_CONFIG_H)
# include <config.h>
#endif

#include "mmpack-download.h"

#include <mmargparse.h>
#include <mmerrno.h>
#include <mmlib.h>
#include <mmsysio.h>
#include <string.h>

#include "cmdline.h"
#include "context.h"
#include "package-utils.h"
#include "pkg-fs-utils.h"


static char download_doc[] =
	"\"mmpack download\" downloads and downloads given packages file "
	"the current prefix cache folder.";


static
struct mmpkg const* lookup_package(struct mmpack_ctx * ctx,
                                   mmstr const * name, mmstr const * version)
{
	struct mmpkg const * pkg;
	STATIC_CONST_MMSTR(any_version, "any");
	if (version == NULL)
		version = any_version;

	pkg = binindex_get_latest_pkg(&ctx->binindex, name, version);
	if (pkg == NULL
	    || pkg_version_compare(pkg->version, version) < 0) {
		return NULL;
	}

	return pkg;
}


/**
 * mmpack_download() - main function for the download command
 * @ctx: mmpack context
 * @argc: number of arguments
 * @argv: array of arguments
 *
 * downloads given packages into the current prefix.
 *
 * Return: 0 on success, -1 otherwise
 */
LOCAL_SYMBOL
int mmpack_download(struct mmpack_ctx * ctx, int argc, const char* argv[])
{
	int arg_index, rv = -1;
	const char * v;
	const char * arg;
	const mmstr * pkg_name;
	const mmstr * pkg_version;
	struct mmpkg const * pkg;
	struct mmarg_parser parser = {
		.flags = mmarg_is_completing() ? MMARG_PARSER_COMPLETION : 0,
		.doc = download_doc,
		.args_doc = DOWNLOAD_SYNOPSIS,
		.execname = "mmpack",
	};

	arg_index = mmarg_parse(&parser, argc, (char**)argv);
	if (mmarg_is_completing())
		return complete_pkgname(ctx, argv[argc-1], AVAILABLE_PKGS);

	if ((arg_index + 1) != argc) {
		fprintf(stderr,
		        "missing package list argument in command line\n"
		        "Run \"mmpack download --help\" to see usage\n");
		return -1;
	}

	/* Load prefix configuration and caches */
	if (mmpack_ctx_use_prefix(ctx, 0))
		return -1;

	arg = *(argv + arg_index);

	/* Find the first occurrence of '=' */
	v = strchr(arg, '=');
	if (v != NULL) {
		/* The package name is before the '=' character */
		pkg_name = mmstr_malloc_copy(arg, v - arg);
		pkg_version = mmstr_malloc_from_cstr(v+1);
	} else {
		pkg_name = mmstr_malloc_from_cstr(arg);
		pkg_version = NULL;
	}

	pkg = lookup_package(ctx, pkg_name, pkg_version);
	if (pkg != NULL)
		rv = download_package(ctx, pkg, pkg->filename);
	else
		error("No such package, or no such package version\n");

	mmstr_free(pkg_name);
	mmstr_free(pkg_version);
	return rv;
}

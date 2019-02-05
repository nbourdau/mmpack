/*
 * @mindmaze_header@
 */

#if defined (HAVE_CONFIG_H)
# include <config.h>
#endif

#include "mmpack-source.h"

#include <mmerrno.h>
#include <mmlib.h>
#include <mmsysio.h>
#include <string.h>

#include "context.h"
#include "download.h"
#include "package-utils.h"


static
int download_pkg_sources(struct mmpack_ctx * ctx, struct mmpkg const * pkg)
{
	int rv;

	mmstr * source_pkg_name;
	const mmstr* url;
	size_t source_pkg_name_len;

	/* source pkg name: name_version_src.tar.gz */
	source_pkg_name_len = strlen(pkg->source) + 1 + strlen(pkg->version)
	                      + 1 + sizeof("_src.tar.gz");
	source_pkg_name = mmstr_malloc(source_pkg_name_len);
	sprintf(source_pkg_name, "%s_%s_src.tar.gz", pkg->source, pkg->version);
	mmstr_setlen(source_pkg_name, source_pkg_name_len);
	url = settings_get_repo_url(&ctx->settings, pkg->repo_index);
	rv = download_from_repo(ctx, url, source_pkg_name, NULL, source_pkg_name);

	if (rv == 0)
		info("Downloaded: %s\n", source_pkg_name);
	else
		error("Failed to download: %s\n", source_pkg_name);

	mmstr_free(source_pkg_name);
	return rv < 0 ? rv : 1;
}


LOCAL_SYMBOL
int mmpack_source(struct mmpack_ctx * ctx, int argc, const char* argv[])
{
	struct mmpkg const * pkg;

	if (argc != 2
	    || STR_EQUAL(argv[1], strlen(argv[1]), "--help")
	    || STR_EQUAL(argv[1], strlen(argv[1]), "-h")) {
		fprintf(stderr, "missing package argument in command line\n"
		                "Usage:\n\tmmpack source "SOURCE_SYNOPSIS"\n");
		return argc != 2 ? -1 : 0;
	}

	/* Load prefix configuration and caches */
	if (mmpack_ctx_use_prefix(ctx, 0))
		return -1;

	pkg = binindex_get_latest_pkg(&ctx->binindex,
	                              mmstr_alloca_from_cstr(argv[1]), "any");
	if (pkg == NULL) {
		info("Could not find source package for: \"%s\"\n", argv[1]);
		return -1;
	}

	return download_pkg_sources(ctx, pkg);
}
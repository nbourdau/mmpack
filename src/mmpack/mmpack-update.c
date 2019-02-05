/*
 * @mindmaze_header@
 */

#if defined (HAVE_CONFIG_H)
# include <config.h>
#endif

#include "context.h"
#include "download.h"
#include "mmpack-update.h"
#include "mmstring.h"
#include "utils.h"


static
int download_repo_index(struct mmpack_ctx * ctx, int repo_index)
{
	STATIC_CONST_MMSTR(pkglist, "binary-index");
	const mmstr* cacheindex;
	const mmstr* url;

	url = settings_get_repo_url(&ctx->settings, repo_index);
	cacheindex = mmpack_ctx_get_cache_index(ctx, repo_index);

	if (download_from_repo(ctx, url, pkglist, NULL, cacheindex)) {
		error("Failed to download package list from %s\n", url);
		return -1;
	}

	info("Updated package list from repository: %s\n", url);
	return 0;
}


LOCAL_SYMBOL
int mmpack_update_all(struct mmpack_ctx * ctx, int argc, char const ** argv)
{
	int i, num_repo;

	if (argc == 2
	    && (STR_EQUAL(argv[1], strlen(argv[1]), "--help")
	        || STR_EQUAL(argv[1], strlen(argv[1]), "-h"))) {
		fprintf(stderr, "Usage:\n\tmmpack "UPDATE_SYNOPSIS"\n");
		return 0;
	}

	// Load prefix configuration
	if (mmpack_ctx_use_prefix(ctx, CTX_SKIP_PKGLIST))
		return -1;

	num_repo = settings_num_repo(&ctx->settings);
	if (num_repo == 0) {
		error("Repository URL unspecified\n");
		return -1;
	}

	for (i = 0; i < num_repo; i++)
		if (download_repo_index(ctx, i))
			return -1;

	return 0;
}
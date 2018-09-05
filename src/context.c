/*
 * @mindmaze_header@
 */

#if defined (HAVE_CONFIG_H)
# include <config.h>
#endif

#include <mmlib.h>
#include <mmsysio.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include <mmerrno.h>
#include <mmlib.h>

#include "binary-index.h"
#include "common.h"
#include "context.h"
#include "indextable.h"
#include "mmstring.h"
#include "package-utils.h"
#include "mmstring.h"


LOCAL_SYMBOL
int mmpack_ctx_init(struct mmpack_ctx * ctx, struct mmpack_opts* opts)
{
	const char* prefix;
	char* default_prefix = (char*)get_default_mmpack_prefix();

	memset(ctx, 0, sizeof(*ctx));

	if (!yaml_parser_initialize(&ctx->parser))
		return mm_raise_error(ENOMEM, "failed to init yaml parse");

	indextable_init(&ctx->binindex, -1, -1);
	indextable_init(&ctx->installed, -1, -1);

	prefix = opts->prefix;
	if (!prefix)
		prefix = mm_getenv("MMPACK_PREFIX", default_prefix);

	ctx->prefix = mmstr_malloc_from_cstr(prefix);

	free(default_prefix);
	return 0;
}


LOCAL_SYMBOL
void mmpack_ctx_deinit(struct mmpack_ctx * ctx)
{
	struct it_iterator iter;
	struct it_entry * entry;

	mmstr_free(ctx->prefix);

	if (ctx->curl != NULL) {
		curl_easy_cleanup(ctx->curl);
		ctx->curl = NULL;
	}

	yaml_parser_delete(&ctx->parser);

	entry = it_iter_first(&iter, &ctx->binindex);
	while (entry != NULL) {
		struct mmpkg * pkg = entry->value;
		mmpkg_destroy(pkg);
		entry = it_iter_next(&iter);
	}
	indextable_deinit(&ctx->binindex);

	entry = it_iter_first(&iter, &ctx->installed);
	while (entry != NULL) {
		struct mmpkg * pkg = entry->value;
		mmpkg_destroy(pkg);
		entry = it_iter_next(&iter);
	}
	indextable_deinit(&ctx->installed);
}

/*
 * @mindmaze_header@
 */

#ifndef CMDLINE_H
#define CMDLINE_H

#include <mmargparse.h>

#include "context.h"
#include "package-utils.h"

enum pkg_comp_type {
	AVAILABLE_PKGS,
	ONLY_INSTALLED,
};

typedef int (* subcmd_proc)(struct mmpack_ctx * ctx, int argc,
                            const char* argv[]);

struct subcmd {
	const char* name;
	subcmd_proc cb;
};


/**
 * struct subcmd_parser - subcmd and option parser configuration
 * @num_opt:    number of element in @optv.
 * @optv:       array of option supported. same as in struct mm_arg_parser
 * @args_doc:   lines of synopsis of program usage (excluding program name).
 *              This can support multiple line (like for different case of
 *              invocation). Can be NULL.
 * @doc:        document of the program. same as in struct mm_arg_parser
 * @execname:   name of executable. You are invited to set it to argv[0]. If
 *              NULL, "PROGRAM" will be used instead for synopsis
 * @num_subcmd: number of element in @subcmds array
 * @subcmds:    pointer to array of subcmd listing the possible command
 * @defcmd:     pointer to string interpreted as sub command if none is
 *              identified in arguments. If NULL, a missing subcommand will
 *              result in error of parsing.
 */
struct subcmd_parser {
	int num_opt;
	const struct mm_arg_opt* optv;
	const char* doc;
	const char* args_doc;
	const char* execname;
	int num_subcmd;
	const struct subcmd* subcmds;
	const char* defcmd;
};


/**
 * struct pkg_parser - structure containing a package parsed on the commandline
 * @cons: constraints asked by the user on the package he wants to
 *        retrieve/inspect...
 * @name: package name
 */
struct pkg_parser {
	struct constraints cons;
	mmstr * name;
	struct mmpkg * pkg;
};

void pkg_parser_init(struct pkg_parser * pp);
void pkg_parser_deinit(struct pkg_parser * pp);

const struct subcmd* subcmd_parse(const struct subcmd_parser* parser,
                                  int* p_argc, const char*** p_argv);
int parse_pkgreq(struct mmpack_ctx * ctx, const char* pkg_req,
                 struct pkg_parser * pp);
struct mmpkg const* parse_pkg(struct mmpack_ctx * ctx, const char* pkg_arg);
struct mmpkg const* find_package_by_sumsha(struct mmpack_ctx * ctx,
                                           const char* pkg_req);
int complete_pkgname(struct mmpack_ctx * ctx, const char* arg,
                     enum pkg_comp_type type);

#endif /* CMDLINE_H */

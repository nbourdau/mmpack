/*
 * @mindmaze_header@
 */

#if defined (HAVE_CONFIG_H)
# include <config.h>
#endif

#include <assert.h>
#include <archive.h>
#include <archive_entry.h>
#include <mmsysio.h>
#include <mmerrno.h>

#include "common.h"
#include "context.h"
#include "download.h"
#include "mmstring.h"
#include "package-utils.h"
#include "pkg-fs-utils.h"
#include "utils.h"
#include "sysdeps.h"

/**************************************************************************
 *                                                                        *
 *                      Packages files unpacking                          *
 *                                                                        *
 **************************************************************************/
#define READ_ARCHIVE_BLOCK      10240
#define READ_ARCHIVE_EOF        1
#define SKIP_UNPACK             1

/**
 * fullwrite() - write fully data buffer to a file
 * @fd:         file descriptor where to write data
 * @data:       data buffer to write
 * @size:       size of @data
 *
 * Return: 0 if @data has been fully written to @fd, -1 otherwise
 */
static
int fullwrite(int fd, const char* data, size_t size)
{
	ssize_t rsz;

	do {
		rsz = mm_write(fd, data, size);
		if (rsz < 0)
			return -1;

		size -= rsz;
		data += rsz;
	} while (size > 0);

	return 0;
}


/**
 * pkg_unpack_regfile() - extract a regular file from archive
 * @entry:      entry header of the file being extracted
 * @path:       path to which the file must be extracted
 * @a:          archive stream from which to read the file content
 *
 * Return: 0 in case of success, -1 otherwise
 */
static
int pkg_unpack_regfile(struct archive_entry *entry, const char* path,
                       struct archive *a)
{
	int r, rv, fd, mode;
	const void *buff;
	size_t size;
	int64_t offset;

	// If previous file exists, remove it first
	if (mm_check_access(path, F_OK) != ENOENT) {
		if (mm_unlink(path))
			return -1;
	}

	// Create the new file. This step should not fail because the caller
	// must have created parent dir
	mode = archive_entry_perm(entry);
	fd = mm_open(path, O_CREAT|O_EXCL|O_WRONLY, mode);
	if (fd < 0)
		return -1;

	// Resize file as reported by archive. If no file size is actually
	// set in archive, 0 will be reported which is harmless
	mm_ftruncate(fd, archive_entry_size(entry));

	rv = 0;
	while (rv == 0) {
		r = archive_read_data_block(a, &buff, &size, &offset);
		if (r == ARCHIVE_EOF)
			break;

		if (r != ARCHIVE_OK) {
			rv = mm_raise_from_errno("Unpacking %s failed", path);
			break;
		}

		if (mm_seek(fd, offset, SEEK_SET) == -1) {
			rv = -1;
			break;
		}

		rv = fullwrite(fd, buff, size);
	}

	mm_close(fd);
	return rv;
}


/**
 * pkg_unpack_symlink() - extract a symbolic link from archive
 * @entry:      entry header of the symlink being extracted
 * @path:       path to which the symlink must be extracted
 *
 * Return: 0 in case of success, -1 otherwise
 */
static
int pkg_unpack_symlink(struct archive_entry *entry, const char* path)
{
	const char* target;
	int rv;

	// If previous file exists, remove it first
	if (mm_check_access(path, F_OK) != ENOENT) {
		if (mm_unlink(path))
			return -1;
	}

	// Create a symlink (path -> target)
	target = archive_entry_symlink_utf8(entry);
	rv = mm_symlink(target, path);

	return rv;
}


/**
 * pkg_unpack_next_entry() - extract archive entry
 * @a:          archive stream from which to read the entry and file content
 * @entry:      archive entry to read
 * @path:       filename of package file being unpacked (it may be
 *              different from the one advertised in entry)
 *
 * Return: 0 on success, a negative value otherwise.
 */
static
int pkg_unpack_entry(struct archive *a, struct archive_entry* entry,
                     const mmstr* path)
{
	int type, rv;

	type = archive_entry_filetype(entry);
	switch (type) {
	case AE_IFDIR:
		rv = mm_mkdir(path, 0777, MM_RECURSIVE);
		break;

	case AE_IFREG:
		rv = pkg_unpack_regfile(entry, path, a);
		break;

	case AE_IFLNK:
		rv = pkg_unpack_symlink(entry, path);
		break;

	default:
		rv = mm_raise_error(MM_EBADFMT,
		                    "unexpected file type of %s", path);
		break;
	}

	return rv;
}


/**
 * is_mmpack_metadata() - given file is an internal mmpack metadata
 * @path: full path to the file
 *
 * Return: 1 if the file is an internal mmpack metadata, 0 otherwise
 */
LOCAL_SYMBOL
int is_mmpack_metadata(mmstr const * path)
{
	return STR_STARTS_WITH(path, (size_t) mmstrlen(path), "MMPACK");
}


/**
 * redirect_metadata() - modify path of inspected file if metadata
 * @pathname:   pointer to mmstr* holding the original path on input and
 *              modified path on output
 * @metadata_prefix: path to prefix to apply when metadata file
 *
 * This function inspect *@pathname of the file about to be inspected from
 * archive and identifies if it is a metadata or not. A file is a metadata
 * if it starts with "./MMPACK".
 *
 * Return: SKIP_UNPACK if the file entry of archive must be skipped, 0
 * otherwise.
 */
static
int redirect_metadata(mmstr** pathname, const mmstr* metadata_prefix)
{
	char tmp_data[64];
	mmstr* basename = mmstr_map_on_array(tmp_data);
	mmstr* path = *pathname;

	if (mmstrlen(path) == 0)
		return SKIP_UNPACK;

	// Keep path NOT starting with MMPACK untouched
	if (!is_mmpack_metadata(path))
		return 0;

	// MMPACK/info is not installed and MMPACK/ must not be created
	if (  mmstrequal(path, mmstr_alloca_from_cstr("MMPACK/info"))
	   || mmstrequal(path, mmstr_alloca_from_cstr("MMPACK/")))
		return SKIP_UNPACK;

	// Change destination
	mmstr_basename(basename, path);
	path = mmstrcpy_realloc(path, metadata_prefix);
	path = mmstrcat_realloc(path, basename);

	*pathname = path;

	return 0;
}


STATIC_CONST_MMSTR(metadata_dirpath, METADATA_RELPATH)

static
int pkg_unpack_files(const struct mmpkg* pkg, const char* mpk_filename)
{
	mmstr* metadata_prefix;
	const char* entry_path;
	struct archive_entry *entry;
	struct archive *a;
	int len, r, rv;
	mmstr* path = NULL;

	// Set the metadata prefix (var/lib/mmpack/metadata/<pkgname>.)
	len = mmstrlen(pkg->name) + mmstrlen(metadata_dirpath) + 2;
	metadata_prefix = mmstr_alloca(len);
	mmstr_join_path(metadata_prefix, metadata_dirpath, pkg->name);
	mmstrcat_cstr(metadata_prefix, ".");

	// Initialize an archive stream
	a = archive_read_new();
	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);

	// Open binary package in the archive stream
	if (archive_read_open_filename(a, mpk_filename, READ_ARCHIVE_BLOCK)) {
		mm_raise_error(archive_errno(a), "opening mpk %s failed: %s",
		               mpk_filename, archive_error_string(a));
		archive_read_free(a);
		return -1;
	}

	// Loop over each entry in the archive and process them
	rv = 0;
	while (rv == 0) {
		r = archive_read_next_header(a, &entry);
		if (r == ARCHIVE_EOF) {
			rv = READ_ARCHIVE_EOF;
			break;
		}

		if (r != ARCHIVE_OK) {
			mm_raise_error(archive_errno(a), "reading mpk %s failed: %s",
			               mpk_filename, archive_error_string(a));
			rv = -1;
			break;
		}

		// Obtain the pathname (with leading "./" stripped) of the
		// file being extracted and redirect to metadata folder if
		// it is a metadata file
		entry_path = archive_entry_pathname_utf8(entry);
		path = mmstrcpy_cstr_realloc(path, entry_path+2);
		if (redirect_metadata(&path, metadata_prefix) == SKIP_UNPACK)
			continue;

		rv = pkg_unpack_entry(a, entry, path);
	}
	mmstr_free(path);

	// Cleanup
	archive_read_close(a);
	archive_read_free(a);

	return (rv != READ_ARCHIVE_EOF) ? -1 : 0;
}


/**************************************************************************
 *                                                                        *
 *                          Packages files removal                        *
 *                                                                        *
 **************************************************************************/

#define UNPACK_MAXPATH  512

/**
 * pkg_list_rm_files() - list files of a package to be removed
 * @pkg:        package about to be removed
 * @files:      pointer to initialized strlist to populate
 *
 * Return: 0 in case of success, -1 otherwise with error
 */
static
int pkg_list_rm_files(const struct mmpkg* pkg, struct strlist* files)
{
	int len, rv = -1;
	FILE* fp;
	mmstr* path;
	mmstr* metadata_prefix;

	path = mmstr_malloc(UNPACK_MAXPATH);

	// Set the metadata prefix (var/lib/mmpack/metadata/<pkgname>.)
	len = mmstrlen(metadata_dirpath) + mmstrlen(pkg->name) + 2;
	metadata_prefix = mmstr_alloca(len);
	mmstr_join_path(metadata_prefix, metadata_dirpath, pkg->name);
	mmstrcat_cstr(metadata_prefix, ".");

	// Open package's sha256sums file to get file list
	mmstrcpy(path, metadata_prefix);
	mmstrcat_cstr(path, "sha256sums");
	fp = fopen(path, "rb");
	if (fp == NULL) {
		mm_raise_from_errno("Can't open %s", path);
		goto exit;
	}

	// Add immediately the path of sha256sums
	strlist_add(files, path);

	while (fscanf(fp, "%"MM_STRINGIFY(UNPACK_MAXPATH)"[^:]: %*s ", path) == 1) {
		mmstr_update_len_from_buffer(path);

		// Check the path has not been truncated
		if (mmstrlen(path) == UNPACK_MAXPATH) {
			mm_raise_error(ENAMETOOLONG, "Path of file"
			               " installed by %s has been truncated"
			               " (truncated path: %s)",
			               pkg->name, path);
			goto exit;
		}

		if (redirect_metadata(&path, metadata_prefix) == SKIP_UNPACK)
			continue;

		// Skip folder (path terminated with '/')
		if (is_path_separator(path[mmstrlen(path)-1]))
			continue;

		strlist_add(files, path);
	}

	fclose(fp);
	rv = 0;

exit:
	mmstr_free(path);
	return rv;
}


static
int rm_files_from_list(struct strlist* files)
{
	struct strlist_elt *elt;
	const mmstr* path;

	elt = files->head;

	while (elt) {
		path = elt->str.buf;

		if (mm_unlink(path)) {
			// If this has failed because the file is not found,
			// nothing prevent us to continue (maybe the user
			// has removed the file by mistake... but this
			// should not block). If something else, let's halt
			if (mm_get_lasterror_number() != ENOENT)
				return -1;
		}

		elt = elt->next;
	}

	return 0;
}


/**************************************************************************
 *                                                                        *
 *                            Packages retrieval                          *
 *                                                                        *
 **************************************************************************/
/**
 * check_file_pkg() - Check integrity of given file
 * @ref_sha: reference file sha256 to compare against
 * @parent: prefix directory to prepend to @filename to get the
 *          final path of the file to hash. This may be NULL
 * @filename: path of file whose hash must be computed
 *
 * Return: 0 if no issue has been found, -1 otherwise
 */
static
int check_file_pkg(const mmstr * ref_sha, const mmstr * parent,
                   const mmstr * filename)
{
	mmstr* sha = mmstr_alloca(SHA_HEXSTR_LEN);

	if (sha_compute(sha, filename, parent))
		return -1;

	if (!mmstrequal(sha, ref_sha)) {
		mm_raise_error(EBADMSG, "bad SHA-256 detected %s", filename);
		return -1;
	}

	return 0;
}


static
size_t get_map_size(int fd)
{
	struct mm_stat buf;

	if (mm_fstat(fd, &buf) != 0)
		return MM_PAGESZ;

	return next_pow2_u64(buf.size);
}


/**
 * check_pkg() - check integrity of installed package from its list of sha256
 * @parent: prefix directory to prepend to @filename to get the
 *          final path of the file to hash. This may be NULL
 * @sumsha: sha256sums file name
 *
 * Return: 0 if no issue has been found, -1 otherwise
 */
LOCAL_SYMBOL
int check_pkg(mmstr const * parent, mmstr const * sumsha)
{
	mmstr * filename;
	mmstr * ref_sha;
	char * line, * eol;
	size_t line_len, filename_len;
	int fd;
	void * map;

	fd = mm_open(sumsha, O_RDONLY, 0);
	if (fd == -1)
		return -1;

	map = mm_mapfile(fd, 0, get_map_size(fd), MM_MAP_READ|MM_MAP_SHARED);

	line = map;
	filename = ref_sha = NULL;
	while((eol = strchr(line, '\n')) != NULL) {
		line_len = eol - line;

		if (line_len < SHA_HEXSTR_LEN) {
			mm_raise_error(EBADMSG, "Error while parsing SHA-256 file");
			break;
		}
		ref_sha = mmstr_copy_realloc(ref_sha, &line[line_len - SHA_HEXSTR_LEN],
		                            SHA_HEXSTR_LEN);

		/* 2 is for len(': ') */
		if (line_len <= SHA_HEXSTR_LEN + 2) {
			mm_raise_error(EBADMSG, "Error while parsing SHA-256 file");
			break;;
		}
		filename_len = line_len - SHA_HEXSTR_LEN - 2;
		filename = mmstr_copy_realloc(filename, line, filename_len);
		if (is_mmpack_metadata(filename))
			goto check_continue;

		if (check_file_pkg(ref_sha, parent, filename) != 0)
			break;

check_continue:
		line = eol + 1;
	}
	mmstr_free(filename);
	mmstr_free(ref_sha);
	mm_unmap(map);
	mm_close(fd);

	return (eol == NULL) ? 0 : -1;
}


/**
 * fetch_pkgs() - download packages that are going to be installed
 * @ctx:        initialized mmpack context
 * @act_stak:   action stack to be applied
 *
 * NOTE: this function assumes current directory is the prefix path
 *
 * Return: 0 in case of success, -1 otherwise
 */
static
int fetch_pkgs(struct mmpack_ctx* ctx, struct action_stack* act_stk)
{
	char pkgbase_data[128];
	mmstr* pkgbase = mmstr_map_on_array(pkgbase_data);
	mmstr* mpkfile = NULL;
	const struct mmpkg* pkg;
	struct action* act;
	const mmstr* repo_url;
	const mmstr* cachedir = mmpack_ctx_get_pkgcachedir(ctx);
	int len, i, rv;

	rv = 0;
	for (i = 0; (i < act_stk->index) && (rv == 0); i++) {
		act = &act_stk->actions[i];
		pkg = act->pkg;
		if (act->action != INSTALL_PKG)
			continue;

		// Get filename of downloaded package and store the path in
		// a field of action structure being analyzed
		mmstr_basename(pkgbase, pkg->filename);
		len = mmstrlen(pkgbase) + mmstrlen(cachedir) + 1;
		mpkfile = mmstr_malloc(len);
		act->pathname = mmstr_join_path(mpkfile, cachedir, pkgbase);

		// Skip if there is a valid package already downloaded
		if (  mm_check_access(mpkfile, F_OK) == 0
		   && check_file_pkg(pkg->sha256, NULL, mpkfile) == 0) {
			mmlog_info("Going to install %s (%s) from cache",
			           pkg->name, pkg->version);
			continue;
		}

		info("Downloading %s (%s)... ", pkg->name, pkg->version);

		// Dowload package from repo and store it in prefix
		// package cachedir
		repo_url = settings_get_repo_url(&ctx->settings, pkg->repo_index);
		if (download_from_repo(ctx, repo_url, pkg->filename,
		                       NULL, mpkfile)) {
			error("Failed!\n");
			return -1;
		}

		// verify integrity of what has been downloaded
		if (check_file_pkg(pkg->sha256, NULL, mpkfile)) {
			error("Integrity check failed!\n");
			return -1;
		}

		info("OK\n");
	}

	return rv;
}


/**************************************************************************
 *                                                                        *
 *                           Packages manipulation                        *
 *                                                                        *
 **************************************************************************/

/**
 * install_package() - install a package in the prefix
 * @ctx:        mmpack context
 * @mpkfile:    filename of the downloaded package file
 *
 * This function install a package in a prefix hierarchy. The list of installed
 * package of context @ctx will be updated.
 *
 * NOTE: this function assumes current directory is the prefix path
 *
 * Return: 0 in case of success, -1 otherwise
 */
static
int install_package(struct mmpack_ctx* ctx,
                    const struct mmpkg* pkg, const mmstr* mpkfile)
{
	int rv;

	info("Installing package %s (%s)... ", pkg->name, pkg->version);
	rv = pkg_unpack_files(pkg, mpkfile);
	if (rv) {
		error("Failed!\n");
		return -1;
	}

	install_state_add_pkg(&ctx->installed, pkg);
	info("OK\n");
	return rv;
}


/**
 * remove_package() - remove a package from the prefix
 * @ctx:        mmpack context
 * @pkg:        package to remove
 *
 * This function removes a package from a prefix hierarchy. The list of
 * installed package of context @ctx will be updated.
 *
 * NOTE: this function assumes current directory is the prefix path
 *
 * Return: 0 in case of success, -1 otherwise
 */
static
int remove_package(struct mmpack_ctx* ctx, const struct mmpkg* pkg)
{
	int rv = 0;
	struct strlist files;

	info("Removing package %s ... ", pkg->name);

	strlist_init(&files);

	if (  pkg_list_rm_files(pkg, &files)
	   || rm_files_from_list(&files)) {
		rv = -1;
	}

	strlist_deinit(&files);

	install_state_rm_pkgname(&ctx->installed, pkg->name);

	if (rv)
		error("Failed!\n");
	else
		info("OK\n");

	return rv;
}


static
int apply_action(struct mmpack_ctx* ctx, struct action* act)
{
	int rv, type;

	type = act->action;

	switch(type) {
	case INSTALL_PKG:
		rv = install_package(ctx, act->pkg, act->pathname);
		break;

	case REMOVE_PKG:
		rv = remove_package(ctx, act->pkg);
		break;

	default:
		rv = mm_raise_error(EINVAL, "invalid action: %i", type);
		break;
	}

	return rv;
}


static
int check_new_sysdeps(struct action_stack* stack)
{
	int i, rv;
	struct strlist_elt* dep;
	struct strset sysdeps;

	strset_init(&sysdeps, STRSET_FOREIGN_STRINGS);

	// Add all system dependencies to the set if a package is installed
	for (i = 0; i < stack->index; i++) {
		if (stack->actions[i].action != INSTALL_PKG)
			continue;

		// Add all sysdeps if the package
		dep = stack->actions[i].pkg->sysdeps.head;
		while (dep) {
			strset_add(&sysdeps, dep->str.buf);
			dep = dep->next;
		}
	}

	rv = check_sysdeps_installed(&sysdeps);

	strset_deinit(&sysdeps);
	return rv;
}


/**
 * apply_action_stack() - execute the action listed in the stack
 * @ctx:        mmpack contect to use
 * @stack:      action stack to apply
 *
 * Return: 0 in case of success, -1 otherwise
 */
int apply_action_stack(struct mmpack_ctx* ctx, struct action_stack* stack)
{
	int i, rv;
	char old_currdir[512];

	if (check_new_sysdeps(stack) != DEPS_OK)
		return -1;

	// Change current directory to prefix... All the prefix relpath can
	// now be used directly.
	mm_getcwd(old_currdir, sizeof(old_currdir));
	if (  mm_chdir(ctx->prefix)
	   || mm_mkdir(METADATA_RELPATH, 0777, MM_RECURSIVE))
		return -1;

	// Fetch missing packages
	rv = fetch_pkgs(ctx, stack);

	// Apply individual action changes
	for (i = 0; (i < stack->index) && (rv == 0); i++)
		rv = apply_action(ctx, &stack->actions[i]);

	// Store the updated installed package list in prefix
	if (mmpack_ctx_save_installed_list(ctx))
		rv = -1;

	// Restore previous current directory
	mm_chdir(old_currdir);
	return rv;
}
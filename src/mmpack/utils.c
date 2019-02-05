/*
 * @mindmaze_header@
 */

#if defined (HAVE_CONFIG_H)
# include <config.h>
#endif

#include <mmlib.h>
#include <mmlog.h>
#include <mmsysio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <mmerrno.h>
#include <mmlib.h>

#include "common.h"
#include "mm-alloc.h"
#include "mmstring.h"
#include "sha256.h"
#include "utils.h"

#define MSG_MAXLEN              128
#define HASH_UPDATE_SIZE        512


/**************************************************************************
 *                                                                        *
 *                      Parse pathname components                         *
 *                                                                        *
 **************************************************************************/
static
const char* get_last_nonsep_ptr(const mmstr* path)
{
	const char* c = path + mmstrlen(path) - 1;

	// skip trailing path separators
	while (c > path && is_path_separator(*c))
		c--;

	return c;
}


static
const char* get_basename_ptr(const mmstr* path)
{
	const char *c, *lastptr;

	lastptr = get_last_nonsep_ptr(path);

	for (c = lastptr-1; c >= path; c--) {
		if (is_path_separator(*c))
			return (c == lastptr) ? c : c + 1;
	}

	return path;
}


LOCAL_SYMBOL
mmstr* mmstr_basename(mmstr* restrict basepath, const mmstr* restrict path)
{
	const char* baseptr;
	const char* lastptr;
	int len;

	baseptr = get_basename_ptr(path);
	lastptr = get_last_nonsep_ptr(path)+1;

	len = lastptr - baseptr;

	// if len == 0, path was "/" (or "//" or "///" or ...)
	if (len <= 0)
		len = 1;

	return mmstr_copy(basepath, baseptr, len);
}


LOCAL_SYMBOL
mmstr* mmstr_dirname(mmstr* restrict dirpath, const mmstr* restrict path)
{
	const char* baseptr;
	const char* lastptr;

	baseptr = get_basename_ptr(path);

	if (baseptr == path) {
		if (is_path_separator(*baseptr))
			return mmstrcpy_cstr(dirpath, "/");

		return mmstrcpy_cstr(dirpath, ".");
	}

	lastptr = baseptr-1;

	// Remove trailing separator
	while (lastptr > path && is_path_separator(*lastptr))
		lastptr--;

	return mmstr_copy(dirpath, path, lastptr - path + 1);
}


LOCAL_SYMBOL
mmstr* mmstr_join_path(mmstr* restrict dst,
                       const mmstr* restrict p1, const mmstr* restrict p2)
{
	int sep_end_p1, sep_start_p2;

	mmstrcpy(dst, p1);

	sep_end_p1 = is_path_separator(p1[mmstrlen(p1)-1]);
	sep_start_p2 = is_path_separator(p2[0]);

	// Remove one '/' if both path part provide a '/' at the junction
	if (sep_end_p1 && sep_start_p2)
		mmstr_setlen(dst, mmstrlen(dst)-1);

	// Add one '/' if neither p1 and p2 provide a '/' at the junction
	if (!sep_start_p2 && !sep_start_p2)
		mmstrcat_cstr(dst, "/");

	return mmstrcat(dst, p2);
}


/**************************************************************************
 *                                                                        *
 *                    File manipulation in prefix                         *
 *                                                                        *
 **************************************************************************/

/**
 * open_file_in_prefix() - open file in specified folder
 * @prefix:     folder from where to open the file (may be NULL)
 * @path:       path relative to @prefix of the file to open
 * @oflag:      control flags how to open the file (same as mm_open())
 *
 * This function opens a file descriptor for file located at @path
 * relatively to a folder specified by @prefix if not NULL. @oflag are the
 * same that can be passed to mm_open().
 *
 * If file may be created (ie @oflag contains O_CREAT), and the parent dir
 * do not exist, the parent dir will be created as well (and recursively)
 *
 * Return: a non-negative integer representing the file descriptor in case
 * of success. Otherwise -1 is returned with error state set accordingly.
 */
LOCAL_SYMBOL
int open_file_in_prefix(const mmstr* prefix, const mmstr* path, int oflag)
{
	int fd = -1;
	mmstr *tmp, *dirpath;

	if (prefix) {
		// Form path of file in prefix
		tmp = mmstr_alloca(mmstrlen(prefix) + mmstrlen(path) + 1);
		path = mmstr_join_path(tmp, prefix, path);
	}

	// If file may have to be created, try create parent dir if needed
	if (oflag & O_CREAT) {
		dirpath = mmstr_alloca(mmstrlen(path));
		mmstr_dirname(dirpath, path);
		if (mm_mkdir(dirpath, 0777, MM_RECURSIVE)) {
			fprintf(stderr, "Create parent dir of %s failed: %s\n",
		                path, mmstrerror(mm_get_lasterror_number()));
			return -1;
		}
	}

	// Create file
	fd = mm_open(path, oflag, 0666);
	if (fd < 0)
		fprintf(stderr, "Failed to open %s: %s\n",
		                path, mmstrerror(mm_get_lasterror_number()));

	return fd;
}


/**************************************************************************
 *                                                                        *
 *                            Host OS detection                           *
 *                                                                        *
 **************************************************************************/

#if defined(_WIN32)
LOCAL_SYMBOL
os_id get_os_id(void)
{
	return OS_ID_WINDOWS_10;
}
#elif defined( __linux)
#define OS_ID_CMD "grep '^ID=' /etc/os-release | cut -f2- -d= | sed -e 's/\"//g'"
LOCAL_SYMBOL
os_id get_os_id(void)
{
	FILE * stream;
	char * line = NULL;
	size_t len = 0;
	ssize_t nread;
	os_id id = OS_IS_UNKNOWN;

	stream = popen(OS_ID_CMD, "r");

	nread = getline(&line, &len, stream);
	if (nread == -1)
		goto exit;

	if (strncasecmp(line, "ubuntu", len)
			||  strncasecmp(line, "debian", len))
		id = OS_ID_DEBIAN;

exit:
	fclose(stream);
	free(line);
	return id;
}
#else /* !win32 && !linux */
LOCAL_SYMBOL
os_id get_os_id(void)
{
	return OS_IS_UNKNOWN;
}
#endif


/**************************************************************************
 *                                                                        *
 *                               Default paths                            *
 *                                                                        *
 **************************************************************************/
static
mmstr* get_default_path(enum mm_known_dir dirtype,
                        char const * default_filename)
{
	mmstr* filename;
	size_t filename_len;

	char const * xdg_home = mm_get_basedir(dirtype);
	if (xdg_home == NULL)
		return NULL;

	filename_len = strlen(xdg_home) + strlen(default_filename) + 1;
	filename = mmstr_malloc(filename_len);

	mmstrcat_cstr(filename, xdg_home);
	mmstrcat_cstr(filename, "/");
	mmstrcat_cstr(filename, default_filename);

	return filename;
}


LOCAL_SYMBOL
mmstr* get_config_filename(void)
{
	return get_default_path(MM_CONFIG_HOME, "mmpack-config.yaml");
}


LOCAL_SYMBOL
mmstr* get_default_mmpack_prefix(void)
{
	return get_default_path(MM_DATA_HOME, "mmpack");
}


/**************************************************************************
 *                                                                        *
 *                            SHA computation helper                      *
 *                                                                        *
 **************************************************************************/
/**
 * conv_to_hexstr() - convert byte array into hexadecimal string
 * @hexstr:     output string, must be (2*@len) long
 * @data:       byte array to convert
 * @len:        length of @data
 *
 * This function generates the hexadecimal string representation of a byte
 * array.
 *
 * Return: length of the string written in @hexstr
 */
static
int conv_to_hexstr(char* hexstr, const unsigned char* data, size_t len)
{
	const char hexlut[] = "0123456789abcdef";
	unsigned char d;
	size_t i;

	for (i = 0; i < len; i++) {
		d = data[i];
		hexstr[2*i + 0] = hexlut[(d >> 4) & 0x0F];
		hexstr[2*i + 1] = hexlut[(d >> 0) & 0x0F];
	}

	return 2*len;
}


/**
 * sha_fd_compute() - compute SHA256 hash of an open file
 * @hash:       mmstr* buffer receiving the hexadecimal form of hash. The
 *              pointed buffer must be SHA_HEXSTR_LEN long.
 * @fd:         file descriptor of a file opened for reading
 *
 * The computed hash is stored in hexadecimal as a mmstr* string in @hash whose
 * length must be at least SHA_HEXSTR_LEN long, as reported by mmstr_maxlen().
 *
 * Return: 0 in case of success, -1 if a problem of file reading has been
 * encountered.
 */
static
int sha_fd_compute(mmstr* hash, int fd)
{
	unsigned char md[SHA256_BLOCK_SIZE], data[HASH_UPDATE_SIZE];
	SHA256_CTX ctx;
	ssize_t rsz;
	int len, rv = 0;

	if (mmstr_maxlen(hash) < SHA_HEXSTR_LEN)
		return mm_raise_error(EOVERFLOW, "hash argument to short");

	sha256_init(&ctx);

	do {
		rsz = mm_read(fd, data, sizeof(data));
		if (rsz < 0) {
			rv = -1;
			break;
		}

		sha256_update(&ctx, data, rsz);
	} while (rsz > 0);

	sha256_final(&ctx, md);
	len = conv_to_hexstr(hash, md, sizeof(md));
	mmstr_setlen(hash, len);

	return rv;
}


static
int sha_regfile_compute(mmstr* hash, const mmstr* path)
{
	int fd;
	int rv = 0;

	if (  (fd = mm_open(path, O_RDONLY, 0)) < 0
	   || sha_fd_compute(hash, fd)) {
		rv = -1;
	}
	mm_close(fd);

	return rv;
}


static
int sha_symlink_compute(mmstr* hash, const mmstr* path, size_t target_size)
{
	unsigned char md[SHA256_BLOCK_SIZE];
	SHA256_CTX ctx;
	char* buff;
	int len;
	int rv = -1;

	buff = mm_malloca(target_size);
	if (mm_readlink(path, buff, target_size))
		goto exit;

	sha256_init(&ctx);
	sha256_update(&ctx, buff, target_size-1);
	sha256_final(&ctx, md);

	len = conv_to_hexstr(hash, md, sizeof(md));
	mmstr_setlen(hash, len);

	hash[0] = 's';
	hash[1] = 'y';
	hash[2] = 'm';

	rv = 0;

exit:
	mm_freea(buff);
	return rv;
}


/**
 * sha_compute() - compute SHA256 hash on specified file
 * @hash:       mmstr* buffer receiving the hexadecimal form of hash. The
 *              pointed string must be at least HASH_HEXSTR_LEN long.
 * @filename:   path of file whose hash must be computed
 * @parent:     prefix directory to prepend to @filename to get the
 *              final path of the file to hash. This may be NULL
 *
 * This function allows to compute the SHA256 hash of a file located at
 *  * @parent/@filename if @parent is non NULL
 *  * @filename if @parent is NULL
 *
 * The computed hash is stored in hexadecimal as a NULL-terminated string
 * in @hash string buffer which must be at least HASH_HEXSTR_SIZE long
 * (this include the NULL termination).
 *
 * Return: 0 in case of success, -1 if a problem of file reading has been
 * encountered.
 */
LOCAL_SYMBOL
int sha_compute(mmstr* hash, const mmstr* filename, const mmstr* parent)
{
	mmstr* fullpath = NULL;
	size_t len;
	int rv = 0;
	struct mm_stat st;

	if (parent != NULL) {
		len = mmstrlen(filename) + mmstrlen(parent) + 1;
		fullpath = mmstr_malloca(len);
		mmstr_join_path(fullpath, parent, filename);

		filename = fullpath;
	}

	if (mm_stat(filename, &st, MM_NOFOLLOW)) {
		rv = -1;
		goto exit;
	}

	if (S_ISREG(st.mode)) {
		rv = sha_regfile_compute(hash, filename);
	} else if (S_ISLNK(st.mode)) {
		rv = sha_symlink_compute(hash, filename, st.size);
	} else {
		rv = mm_raise_error(EINVAL, "%s is neither a regular file "
		                            "or symlink", filename);
	}

exit:
	mmstr_freea(fullpath);
	if (rv == -1)
		mmlog_error("Cannot compute SHA-256 of %s", filename);

	return rv;
}


/**************************************************************************
 *                                                                        *
 *                            user/log interaction                        *
 *                                                                        *
 **************************************************************************/

/**
 * prompt_user_confirm() - interactively ask user for confirmation
 *
 * Interpret invalid or empty answer as denial.
 *
 * Return: 0 on acceptance, 0 otherwise
 */
LOCAL_SYMBOL
int prompt_user_confirm(void)
{
	int rv;
	char line[64];
	char answer;

	printf("Do you want to proceed? [y/N] ");
	if (fgets(line, sizeof(line), stdin) == NULL)
		return -1;
	rv = sscanf(line, "%c\n", &answer);
	if (rv != 0 && rv != EOF && answer == 'y')
		return 0;

	return -1;
}


/**
 * report_user_and_log() - print message to user and log it
 * @mmlog_level:        mmlog level to set to the logged message
 * @fmt:                printf-like format string
 *
 * This function format a message duplicates it both to standard output for
 * the user and in the log (through mmlog which will decorate the message and
 * send it to standard error).
 *
 * The message may or may not be terminated by a linefeed character. If
 * there is one, the message sent to log (mmlog facility) will be anyway
 * stripped from it terminating LF. This behavior allows sending sending
 * multipart message to user, while the log contains only self-contained
 * one-liners.
 */
LOCAL_SYMBOL
void report_user_and_log(int mmlog_level, const char* fmt, ...)
{
	char msg[MSG_MAXLEN+2];
	int lastchar_idx, msglen, has_lf;
	va_list ap;

	va_start(ap, fmt);
	msglen = vsnprintf(msg, MSG_MAXLEN+1, fmt, ap);
	va_end(ap);

	// Ensure msglen represents the length of buffer in msg even in
	// case of truncation or error
	msglen = MAX(msglen, 0);
	msglen = MIN(msglen, MSG_MAXLEN);

	// See if there was a trailing linefeed and remove it temporary if
	// so (because mmlog assume one-liner be added in log)
	lastchar_idx = (msglen == 0) ? 0 : msglen - 1;
	has_lf = (msg[lastchar_idx] == '\n');
	if (has_lf)
		msg[lastchar_idx] = '\0';

	// log message with mmlog (to STDERR)
	mmlog_log(mmlog_level, PACKAGE_NAME, msg);

	// restore trailing linefeed if there was one
	if (has_lf)
		msg[lastchar_idx] = '\n';

	// Write message to standard output
	fwrite(msg, 1, msglen, stdout);
}


/**************************************************************************
 *                                                                        *
 *                            string list                                 *
 *                                                                        *
 **************************************************************************/

LOCAL_SYMBOL
void strlist_init(struct strlist* list)
{
	*list = (struct strlist) {0};
}


LOCAL_SYMBOL
void strlist_deinit(struct strlist* list)
{
	struct strlist_elt *elt, *next;

	elt = list->head;

	while (elt) {
		next = elt->next;
		free(elt);
		elt = next;
	}
}


LOCAL_SYMBOL
int strlist_add(struct strlist* list, const char* str)
{
	struct strlist_elt* elt;
	int len;

	// Create the new element
	len = strlen(str);
	elt = mm_malloc(sizeof(*elt) + len + 1);
	elt->str.max = len;
	elt->str.len = len;
	memcpy(elt->str.buf, str, len + 1);
	elt->next = NULL;

	// Set as new head if list is empty
	if (list->head == NULL) {
		list->head = elt;
		list->last = elt;
		return 0;
	}

	// Add new element at the end of list
	list->last->next = elt;
	list->last = elt;
	return 0;
}


/**************************************************************************
 *                                                                        *
 *                               buffer                                   *
 *                                                                        *
 **************************************************************************/

LOCAL_SYMBOL
void buffer_init(struct buffer* buf)
{
	*buf = (struct buffer) {0};
}


LOCAL_SYMBOL
void buffer_deinit(struct buffer* buf)
{
	free(buf->base);
	*buf = (struct buffer) {0};
}


/**
 * buffer_reserve_data() - ensure data buffer can welcome a new amount
 * @buff:       struct buffer to modify
 * @need_size:  data size to accomodate in addition to the current size
 *
 * Return: pointer to data allocated next to content previously added to
 * @buff. You are guaranteed that up to @need_size can be written to the
 * returned data.
 */
LOCAL_SYMBOL
void* buffer_reserve_data(struct buffer* buff, size_t need_size)
{
	if (buff->size + need_size > buff->max_size) {
		buff->max_size = next_pow2_u64(buff->size + need_size);
		buff->base = mm_realloc(buff->base, buff->max_size);
	}

	return (char*)buff->base + buff->size;
}


/**
 * buffer_inc_size() - increase memory use in the buffer
 * @buff:       struct buffer to modify
 * @size:       number of byte to add to @buff->size
 *
 * Return: the new size used by @buff.
 */
LOCAL_SYMBOL
size_t buffer_inc_size(struct buffer* buff, size_t size)
{
	buff->size += size;
	return buff->size;
}


/**
 * buffer_dec_size() - reduce size used of a buffer
 * @buff:       struct buffer to modify
 * @size:       number of byte to reduce
 *
 * This function is the exact opposite of buffer_inc_size(). It reduces the
 * size "used" by the buffer by @size number of bytes. The memory
 * corresponding to the size reduction memory is still allocated by @buff
 * and can be accessed until overwritten explicitely.
 *
 * Return: the pointer to new top of buffer.
 */
LOCAL_SYMBOL
void* buffer_dec_size(struct buffer* buf, size_t sz)
{
	buf->size -= sz;
	return (char*)buf->base + buf->size;
}


LOCAL_SYMBOL
void buffer_push(struct buffer* buf, const void* data, size_t sz)
{
	memcpy(buffer_reserve_data(buf, sz), data, sz);
	buffer_inc_size(buf, sz);
}


LOCAL_SYMBOL
void buffer_pop(struct buffer* buf, void* data, size_t sz)
{
	memcpy(data, buffer_dec_size(buf, sz), sz);
}


/**
 * buffer_take_data_ownership() - steal ownership of the internal data
 * @buff:       struct buffer to modify
 *
 * This function steal ownership of the internal data buffer of @buff.
 * After this call, @buff will not touch this buffer anymore, later call to
 * buffer_*() will operate on a new internal data buffer.
 *
 * Return: the pointer to the internal buffer. Call free() on it once you
 * are done with it.
 */
LOCAL_SYMBOL
void* buffer_take_data_ownership(struct buffer* buff)
{
	void* data = buff->base;

	buffer_init(buff);

	return data;
}

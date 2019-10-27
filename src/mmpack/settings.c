/*
 * @mindmaze_header@
 */
#if defined (HAVE_CONFIG_H)
# include <config.h>
#endif

#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <yaml.h>

#include <mmerrno.h>
#include <mmlib.h>
#include <mmsysio.h>

#include "mmstring.h"
#include "utils.h"


enum {
	UNKNOWN_FIELD = -1,
	REPOSITORIES,
	DEFAULT_PREFIX,
};


/**
 * repolist_init() - init repolist structure
 * @list: repolist structure to initialize
 *
 * To be cleansed by calling repolist_deinit()
 */
LOCAL_SYMBOL
void repolist_init(struct repolist* list)
{
	*list = (struct repolist) {0};
}


/**
 * repolist_deinit() - cleanup repolist structure
 * @list: repolist structure to cleanse
 */
LOCAL_SYMBOL
void repolist_deinit(struct repolist* list)
{
	struct repolist_elt * elt, * next;

	elt = list->head;

	while (elt) {
		next = elt->next;
		mmstr_free(elt->url);
		mmstr_free(elt->name);
		free(elt);
		elt = next;
	}
}


/**
 * repolist_add() - add a repository to the list
 * @list: initialized repolist structure
 * @url: the url of the repository from which packages can be retrieved
 * @name: the short name referencing the url
 *
 * Return: always return 0
 */
LOCAL_SYMBOL
int repolist_add(struct repolist* list, const char* url, const char* name)
{
	struct repolist_elt* elt;

	/* Should not be possible. Maybe on malformed yaml files ?
	 * Handle anyway to silence scan-build. */
	if (name == NULL) {
		return mm_raise_error(MM_EBADFMT,
		                      "url %s must have a short name", url);
	}


	elt = mm_malloc(sizeof(*elt));
	elt->name = NULL;
	elt->url = NULL;

	elt->url = mmstr_malloc_from_cstr(url);
	elt->name = mmstr_malloc_from_cstr(name);

	elt->next = NULL;

	// Set as new head if list is empty
	if (list->head == NULL) {
		list->head = elt;
		return 0;
	}

	// Add new element at the end of list
	elt->next = list->head;
	list->head = elt;
	return 0;
}


static
int get_field_type(const char* name, int len)
{
	if (STR_EQUAL(name, len, "repositories"))
		return REPOSITORIES;
	else if (STR_EQUAL(name, len, "default-prefix"))
		return DEFAULT_PREFIX;
	else
		return UNKNOWN_FIELD;
}


static
int set_settings_field(struct settings* s, int field_type,
                       const char* data, int len)
{
	switch (field_type) {
	case DEFAULT_PREFIX:
		s->default_prefix = mmstr_copy_realloc(s->default_prefix,
		                                       data,
		                                       len);
		break;

	default:
		// Unknown field are silently ignored
		break;
	}

	return 0;
}


static
int fill_repositories(yaml_parser_t* parser, struct settings* settings)
{
	yaml_token_t token;
	struct repolist repo_list;
	int type = -1;
	int cpt = 1; // counter to know when the list of repositories ends
	int rv = -1;
	char* name = NULL;
	char* url = NULL;

	repolist_init(&repo_list);
	while (1) {
		if (!yaml_parser_scan(parser, &token))
			goto exit;

		switch (token.type) {

		case YAML_FLOW_SEQUENCE_END_TOKEN:
			goto exit;

		case YAML_STREAM_END_TOKEN:
		case YAML_BLOCK_END_TOKEN:
			cpt--;
			if (cpt == 0)
				goto exit;

			break;

		case YAML_VALUE_TOKEN:
			type = YAML_VALUE_TOKEN;
			break;

		case YAML_KEY_TOKEN:
			type = YAML_KEY_TOKEN;
			cpt++;
			break;

		case YAML_SCALAR_TOKEN:
			if (type == YAML_KEY_TOKEN) {
				name = mm_malloc(token.data.scalar.length + 1);
				memcpy(name, token.data.scalar.value,
				       token.data.scalar.length + 1);
				type = -1;
			} else if (type == YAML_VALUE_TOKEN) {
				url = mm_malloc(token.data.scalar.length + 1);
				memcpy(url, token.data.scalar.value,
				       token.data.scalar.length + 1);
				repolist_add(&repo_list, url, name);

				free(name);
				name = NULL;
				free(url);
				url = NULL;
				type = -1;
			} else {
				/* if the yaml repository list has no server
				 * name and only contains urls, then yaml does
				 * not present a YAML_VALUE_TOKEN and directly
				 * jumps to the YAML_SCALAR_TOKEN */
				mm_raise_error(MM_EBADFMT,
				               "url %s must have a short name",
				               token.data.scalar.value);
				goto error;
			}

			break;

		default:
			// silently ignore error
			break;
		}

		yaml_token_delete(&token);
	}

exit:
	/* Replace repo list in settings */
	repolist_deinit(&settings->repo_list);
	settings->repo_list = repo_list;
	rv = 0;

error:
	free(name);
	free(url);
	yaml_token_delete(&token);

	return rv;
}


static
int parse_config(yaml_parser_t* parser, struct settings* settings)
{
	yaml_token_t token;
	const char* data;
	int rv, type, field_type, datalen;

	rv = -1;
	data = NULL;
	type = -1;
	field_type = UNKNOWN_FIELD;
	while (1) {
		if (!yaml_parser_scan(parser, &token)) {
			rv = 0;
			goto exit;
		}

		switch (token.type) {
		case YAML_STREAM_END_TOKEN:
			rv = 0;
			goto exit;
		case YAML_KEY_TOKEN:
			type = YAML_KEY_TOKEN;
			break;
		case YAML_VALUE_TOKEN:
			type = YAML_VALUE_TOKEN;
			break;
		case YAML_FLOW_SEQUENCE_START_TOKEN:
		case YAML_BLOCK_SEQUENCE_START_TOKEN:
			if (type == YAML_VALUE_TOKEN
			    && field_type == REPOSITORIES) {
				if (fill_repositories(parser, settings)) {
					return -1;
				}
			}

			break;
		case YAML_SCALAR_TOKEN:
			data = (const char*)token.data.scalar.value;
			datalen = token.data.scalar.length;
			if (type == YAML_KEY_TOKEN) {
				field_type = get_field_type(data, datalen);
			} else if (type == YAML_VALUE_TOKEN) {
				if (set_settings_field(settings, field_type,
				                       data, datalen))
					goto exit;
			}

			break;
		default:         // ignore
			break;
		}

		yaml_token_delete(&token);
	}

exit:
	yaml_token_delete(&token);
	return rv;
}


/**
 * settings_load() - read config file and update settings
 * @settings:   initialized settings structure to update
 * @filename:   configuration file to open
 *
 * Read and parse specified configuration file and set or update fields of
 * @settings accordingly.
 *
 * If @filename does not exists, no update will be done and the function
 * will succeed. However, if @filename exists but is not readable, the
 * function will fail.
 *
 * Return: 0 in case of success, -1 otherwise.
 */
LOCAL_SYMBOL
int settings_load(struct settings* settings, const char* filename)
{
	int rv = -1;
	FILE* fh = NULL;
	yaml_parser_t parser;

	if (mm_check_access(filename, F_OK))
		return 0;

	fh = fopen(filename, "rb");
	if (fh == NULL) {
		mm_raise_from_errno("Cannot open %s", filename);
		return -1;
	}

	if (!yaml_parser_initialize(&parser)) {
		mm_raise_error(ENOMEM, "failed to init yaml parse");
		goto exit;
	}

	yaml_parser_set_input_file(&parser, fh);
	rv = parse_config(&parser, settings);
	yaml_parser_delete(&parser);

exit:
	fclose(fh);
	return rv;
}


/**
 * settings_init() - init settings structure
 * @settings: the settings structure to initialize
 *
 * Should be cleansed by calling settings_deinit()
 */
LOCAL_SYMBOL
void settings_init(struct settings* settings)
{
	*settings = (struct settings) {
		.default_prefix = get_default_mmpack_prefix(),
	};

	repolist_init(&settings->repo_list);
}


/**
 * settings_deinit() - cleanse settings structure
 * @settings: the settings structure to clean
 */
LOCAL_SYMBOL
void settings_deinit(struct settings* settings)
{
	repolist_deinit(&settings->repo_list);
	mmstr_free(settings->default_prefix);

	*settings = (struct settings) {0};
}


/**
 * settings_reset() - reset settings structure
 * @settings: the settings structure to clean
 */
LOCAL_SYMBOL
void settings_reset(struct settings* settings)
{
	settings_deinit(settings);
	settings_init(settings);
}


/**
 * settings_num_repo() - count the number of repositories in the configuration
 * @settings: an initialized settings structure
 *
 * Returns: the number of repositories
 */
LOCAL_SYMBOL
int settings_num_repo(const struct settings* settings)
{
	struct repolist_elt* elt;
	int num;

	num = 0;
	for (elt = settings->repo_list.head; elt; elt = elt->next)
		num++;

	return num;
}


/**
 * settings_get_repo_url() - pick one url from the settings
 * @settings: an initialized settings structure
 * @index: index of the url to get
 *
 * Return: a pointer to a mmstr structure describing the url on success
 *         NULL otherwise
 */
LOCAL_SYMBOL
const mmstr* settings_get_repo_url(const struct settings* settings, int index)
{
	struct repolist_elt* repo = settings_get_repo(settings, index);

	return repo->url;
}


/**
 * settings_get_repo() - pick one repository from the settings
 * @settings: an initialized settings structure
 * @index: index of the repository to get
 *
 * Return: a pointer to a struct repolist_elt describing the repository on
 * success NULL otherwise
 */
LOCAL_SYMBOL
struct repolist_elt* settings_get_repo(const struct settings* settings,
                                       int index)
{
	struct repolist_elt* elt = settings->repo_list.head;
	int i;

	for (i = 0; i < index; i++) {
		if (!elt)
			return NULL;

		elt = elt->next;
	}

	return elt;
}

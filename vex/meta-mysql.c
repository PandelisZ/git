#include "vex/meta-mysql.h"

#ifdef VEX_META_MYSQL_STORE
#include "git-compat-util.h"
#include "trace.h"
#include "vex/store-internal.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_value(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int vex_meta_mysql_repo_id_parse(const char *repo_id, unsigned char out[16])
{
	size_t i;
	size_t out_at = 0;
	size_t len;
	int high = -1;
	int hyphenated;

	if (!repo_id || !out)
		return VEX_META_MYSQL_ERR_INVALID;
	len = strlen(repo_id);
	hyphenated = len == 36;
	if (len != VEX_META_MYSQL_REPO_ID_HEXSZ && !hyphenated)
		return VEX_META_MYSQL_ERR_INVALID;

	for (i = 0; i < len; i++) {
		int value;

		if (hyphenated && (i == 8 || i == 13 || i == 18 || i == 23)) {
			if (repo_id[i] != '-')
				return VEX_META_MYSQL_ERR_INVALID;
			continue;
		}
		value = hex_value(repo_id[i]);
		if (value < 0)
			return VEX_META_MYSQL_ERR_INVALID;
		if (high < 0) {
			high = value;
			continue;
		}
		out[out_at++] = (high << 4) | value;
		high = -1;
	}

	return out_at == 16 && high < 0 ? VEX_META_MYSQL_OK :
		VEX_META_MYSQL_ERR_INVALID;
}

#ifndef VEX_HAVE_MYSQL_CLIENT

/* Keep the source safe to compile in a regular Git build without libmysql. */
static int mysql_unavailable(void)
{
	return VEX_META_MYSQL_ERR_CONNECT;
}

int vex_meta_mysql_open(struct vex_meta_mysql **out,
			const struct vex_meta_mysql_options *opts)
{
	(void)opts;
	if (out)
		*out = NULL;
	return mysql_unavailable();
}

void vex_meta_mysql_close(struct vex_meta_mysql *db)
{
	(void)db;
}

const char *vex_meta_mysql_error(const struct vex_meta_mysql *db)
{
	(void)db;
	return "Vex was built without a MySQL client; use make vex-mysql-metadata-probe";
}

int vex_meta_mysql_ping(struct vex_meta_mysql *db)
{
	(void)db;
	return mysql_unavailable();
}

int vex_meta_mysql_schema_check(struct vex_meta_mysql *db,
				unsigned int *table_count)
{
	(void)db;
	if (table_count)
		*table_count = 0;
	return mysql_unavailable();
}

int vex_meta_mysql_ensure_repository(struct vex_meta_mysql *db,
				     const char *repo_id,
				     const char *hash_algo,
				     const char *name)
{
	(void)db;
	(void)repo_id;
	(void)hash_algo;
	(void)name;
	return mysql_unavailable();
}

int vex_meta_mysql_epoch_begin(struct vex_meta_mysql *db,
			       const char *repo_id, const char *push_id,
			       uint64_t *generation)
{
	(void)db;
	(void)repo_id;
	(void)push_id;
	if (generation)
		*generation = 0;
	return mysql_unavailable();
}

int vex_meta_mysql_epoch_get_state(struct vex_meta_mysql *db,
				   const char *repo_id, uint64_t generation,
				   enum vex_meta_mysql_epoch_state *state)
{
	(void)db;
	(void)repo_id;
	(void)generation;
	if (state)
		*state = 0;
	return mysql_unavailable();
}

int vex_meta_mysql_epoch_abort(struct vex_meta_mysql *db,
			       const char *repo_id, uint64_t generation)
{
	(void)db;
	(void)repo_id;
	(void)generation;
	return mysql_unavailable();
}

int vex_meta_mysql_stage_object(struct vex_meta_mysql *db,
			 const char *repo_id, uint64_t generation,
			 const unsigned char *oid, size_t oid_len,
			 unsigned int object_type, uint64_t size,
			 const char *blob_key)
{
	(void)db;
	(void)repo_id;
	(void)generation;
	(void)oid;
	(void)oid_len;
	(void)object_type;
	(void)size;
	(void)blob_key;
	return mysql_unavailable();
}

int vex_meta_mysql_object_get_visible(struct vex_meta_mysql *db,
				      const char *repo_id,
				      const unsigned char *oid, size_t oid_len,
				      uint64_t visible_generation,
				      struct vex_meta_mysql_object *object)
{
	(void)db;
	(void)repo_id;
	(void)oid;
	(void)oid_len;
	(void)visible_generation;
	if (object)
		memset(object, 0, sizeof(*object));
	return mysql_unavailable();
}

void vex_meta_mysql_object_release(struct vex_meta_mysql_object *object)
{
	if (!object)
		return;
	free(object->blob_key);
	memset(object, 0, sizeof(*object));
}

int vex_meta_mysql_add_commit_parent(struct vex_meta_mysql *db,
			     const char *repo_id,
			     const unsigned char *commit_oid, size_t commit_oid_len,
			     unsigned int parent_pos,
			     const unsigned char *parent_oid, size_t parent_oid_len)
{
	(void)db;
	(void)repo_id;
	(void)commit_oid;
	(void)commit_oid_len;
	(void)parent_pos;
	(void)parent_oid;
	(void)parent_oid_len;
	return mysql_unavailable();
}

int vex_meta_mysql_epoch_seal(struct vex_meta_mysql *db,
			      const char *repo_id, uint64_t generation)
{
	(void)db;
	(void)repo_id;
	(void)generation;
	return mysql_unavailable();
}

int vex_meta_mysql_ref_cas_direct(struct vex_meta_mysql *db,
				  const char *repo_id, const char *refname,
				  uint64_t expected_version,
				  const unsigned char *new_oid, size_t new_oid_len,
				  const char *reason, const char *message,
				  uint64_t *new_version)
{
	(void)db;
	(void)repo_id;
	(void)refname;
	(void)expected_version;
	(void)new_oid;
	(void)new_oid_len;
	(void)reason;
	(void)message;
	if (new_version)
		*new_version = 0;
	return mysql_unavailable();
}

int vex_meta_mysql_pin_object(struct vex_meta_mysql *db,
			       const char *repo_id,
			       const unsigned char *oid, size_t oid_len,
			       const char *reason)
{
	(void)db;
	(void)repo_id;
	(void)oid;
	(void)oid_len;
	(void)reason;
	return mysql_unavailable();
}

int vex_meta_mysql_unpin_object(struct vex_meta_mysql *db,
				 const char *repo_id,
				 const unsigned char *oid, size_t oid_len,
				 const char *reason)
{
	(void)db;
	(void)repo_id;
	(void)oid;
	(void)oid_len;
	(void)reason;
	return mysql_unavailable();
}

int vex_meta_mysql_for_each_active_pin(struct vex_meta_mysql *db,
					const char *repo_id,
					vex_meta_mysql_pin_iter_fn fn,
					void *cb_data)
{
	(void)db;
	(void)repo_id;
	(void)fn;
	(void)cb_data;
	return mysql_unavailable();
}

int vex_meta_mysql_state_lock(struct vex_meta_mysql *db, const char *repo_id)
{
	(void)db;
	(void)repo_id;
	return mysql_unavailable();
}

void vex_meta_mysql_state_unlock(struct vex_meta_mysql *db)
{
	(void)db;
}

int vex_meta_mysql_state_load(struct vex_meta_mysql *db, const char *repo_id,
			       const struct git_hash_algo *hash_algo,
			       struct vex_meta_state *state)
{
	(void)db;
	(void)repo_id;
	(void)hash_algo;
	(void)state;
	return mysql_unavailable();
}

int vex_meta_mysql_state_save_locked(struct vex_meta_mysql *db,
				      const char *repo_id,
				      const struct git_hash_algo *hash_algo,
				      const struct vex_meta_state *state)
{
	(void)db;
	(void)repo_id;
	(void)hash_algo;
	(void)state;
	return mysql_unavailable();
}

#else /* VEX_HAVE_MYSQL_CLIENT */

/* mysql.h exports an unrelated list_add symbol; Git already owns that name. */
#define list_add vex_mysql_client_list_add
#include <mysql.h>
#undef list_add

#define VEX_META_MYSQL_ERROR_SIZE 512

struct vex_meta_mysql {
	MYSQL *mysql;
	char error[VEX_META_MYSQL_ERROR_SIZE];
	/* A central-store write owns this transaction until save or unlock. */
	int state_transaction;
};

static const char hex[] = "0123456789abcdef";

static int set_error(struct vex_meta_mysql *db, int result,
		     const char *fmt, ...)
{
	va_list args;

	if (!db)
		return result;
	va_start(args, fmt);
	vsnprintf(db->error, sizeof(db->error), fmt, args);
	va_end(args);
	return result;
}

static int set_mysql_error(struct vex_meta_mysql *db, const char *context)
{
	return set_error(db, VEX_META_MYSQL_ERR, "%s: %s", context,
			 mysql_error(db->mysql));
}

static char *vex_strdup(const char *value)
{
	size_t len;
	char *copy;

	if (!value)
		return NULL;
	len = strlen(value);
	copy = malloc(len + 1);
	if (!copy)
		return NULL;
	memcpy(copy, value, len + 1);
	return copy;
}

static char *vex_format(const char *fmt, ...)
{
	va_list args;
	va_list copy;
	char *result;
	int len;

	va_start(args, fmt);
	va_copy(copy, args);
	len = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (len < 0) {
		va_end(args);
		return NULL;
	}
	result = malloc((size_t)len + 1);
	if (!result) {
		va_end(args);
		return NULL;
	}
	vsnprintf(result, (size_t)len + 1, fmt, args);
	va_end(args);
	return result;
}

static void bytes_to_hex(const unsigned char *bytes, size_t nr, char *out)
{
	size_t i;

	for (i = 0; i < nr; i++) {
		out[i * 2] = hex[bytes[i] >> 4];
		out[i * 2 + 1] = hex[bytes[i] & 0x0f];
	}
	out[nr * 2] = '\0';
}

static int normalize_oid(const unsigned char *oid, size_t oid_len,
			 unsigned char out[VEX_META_MYSQL_OID_BYTES])
{
	if (!oid || (oid_len != 20 && oid_len != VEX_META_MYSQL_OID_BYTES))
		return VEX_META_MYSQL_ERR_INVALID;
	memset(out, 0, VEX_META_MYSQL_OID_BYTES);
	memcpy(out, oid, oid_len);
	return VEX_META_MYSQL_OK;
}

static char *sql_hex_literal(const unsigned char *bytes, size_t nr)
{
	char *literal;

	if (nr > (SIZE_MAX - 4) / 2)
		return NULL;
	literal = malloc(nr * 2 + 4);
	if (!literal)
		return NULL;
	literal[0] = 'X';
	literal[1] = '\'';
	bytes_to_hex(bytes, nr, literal + 2);
	literal[nr * 2 + 2] = '\'';
	literal[nr * 2 + 3] = '\0';
	return literal;
}

static char *sql_repo_literal(const char *repo_id, char repo_hex[33])
{
	unsigned char bytes[16];

	if (vex_meta_mysql_repo_id_parse(repo_id, bytes))
		return NULL;
	if (repo_hex)
		bytes_to_hex(bytes, sizeof(bytes), repo_hex);
	return sql_hex_literal(bytes, sizeof(bytes));
}

static char *sql_uuid_literal(const char *uuid)
{
	unsigned char bytes[16];

	if (!uuid)
		return vex_strdup("NULL");
	if (vex_meta_mysql_repo_id_parse(uuid, bytes))
		return NULL;
	return sql_hex_literal(bytes, sizeof(bytes));
}

static char *sql_oid_literal(const unsigned char *oid, size_t oid_len)
{
	unsigned char padded[VEX_META_MYSQL_OID_BYTES];

	if (normalize_oid(oid, oid_len, padded))
		return NULL;
	return sql_hex_literal(padded, sizeof(padded));
}

static char *sql_quote(struct vex_meta_mysql *db, const char *value)
{
	char *quoted;
	size_t len;
	unsigned long escaped;

	if (!value)
		return vex_strdup("NULL");
	len = strlen(value);
	if (len > (SIZE_MAX - 3) / 2 || len > ULONG_MAX)
		return NULL;
	quoted = malloc(len * 2 + 3);
	if (!quoted)
		return NULL;
	quoted[0] = '\'';
	escaped = mysql_real_escape_string(db->mysql, quoted + 1, value,
					     (unsigned long)len);
	quoted[escaped + 1] = '\'';
	quoted[escaped + 2] = '\0';
	return quoted;
}

static int mysql_exec(struct vex_meta_mysql *db, const char *sql)
{
	MYSQL_RES *res;

	if (mysql_query(db->mysql, sql))
		return set_mysql_error(db, "MySQL query failed");
	if (!mysql_field_count(db->mysql))
		return VEX_META_MYSQL_OK;
	res = mysql_store_result(db->mysql);
	if (!res)
		return set_mysql_error(db, "MySQL result retrieval failed");
	mysql_free_result(res);
	return VEX_META_MYSQL_OK;
}

static int parse_u64(const char *value, uint64_t *out)
{
	char *end;
	unsigned long long parsed;

	if (!value || !*value)
		return -1;
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno || *end)
		return -1;
	*out = parsed;
	return 0;
}

static int query_one_u64(struct vex_meta_mysql *db, const char *sql,
			 uint64_t *out, int *found)
{
	MYSQL_RES *res;
	MYSQL_ROW row;
	uint64_t value;

	*found = 0;
	if (mysql_query(db->mysql, sql))
		return set_mysql_error(db, "MySQL query failed");
	res = mysql_store_result(db->mysql);
	if (!res)
		return set_mysql_error(db, "MySQL result retrieval failed");
	row = mysql_fetch_row(res);
	if (row) {
		if (parse_u64(row[0], &value)) {
			mysql_free_result(res);
			return set_error(db, VEX_META_MYSQL_ERR,
					 "MySQL returned an invalid unsigned integer");
		}
		*out = value;
		*found = 1;
	}
	mysql_free_result(res);
	return VEX_META_MYSQL_OK;
}

static int query_one_text(struct vex_meta_mysql *db, const char *sql,
			  char *out, size_t out_size, int *found)
{
	MYSQL_RES *res;
	MYSQL_ROW row;
	unsigned long *lengths;

	*found = 0;
	if (mysql_query(db->mysql, sql))
		return set_mysql_error(db, "MySQL query failed");
	res = mysql_store_result(db->mysql);
	if (!res)
		return set_mysql_error(db, "MySQL result retrieval failed");
	row = mysql_fetch_row(res);
	if (row) {
		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !lengths || lengths[0] >= out_size) {
			mysql_free_result(res);
			return set_error(db, VEX_META_MYSQL_ERR,
					 "MySQL returned an invalid text value");
		}
		memcpy(out, row[0], lengths[0]);
		out[lengths[0]] = '\0';
		*found = 1;
	}
	mysql_free_result(res);
	return VEX_META_MYSQL_OK;
}

static int parse_epoch_state(const char *value,
			     enum vex_meta_mysql_epoch_state *state)
{
	if (!strcmp(value, "open"))
		*state = VEX_META_MYSQL_EPOCH_OPEN;
	else if (!strcmp(value, "sealed"))
		*state = VEX_META_MYSQL_EPOCH_SEALED;
	else if (!strcmp(value, "sweepable"))
		*state = VEX_META_MYSQL_EPOCH_SWEEPABLE;
	else if (!strcmp(value, "swept"))
		*state = VEX_META_MYSQL_EPOCH_SWEPT;
	else if (!strcmp(value, "aborted"))
		*state = VEX_META_MYSQL_EPOCH_ABORTED;
	else
		return -1;
	return 0;
}

static int parse_object_state(const char *value,
			      enum vex_meta_mysql_object_state *state)
{
	if (!strcmp(value, "quarantine"))
		*state = VEX_META_MYSQL_OBJECT_QUARANTINE;
	else if (!strcmp(value, "published"))
		*state = VEX_META_MYSQL_OBJECT_PUBLISHED;
	else
		return -1;
	return 0;
}

static int require_published_object_locked(struct vex_meta_mysql *db,
					   const char *repo,
					   const char *object)
{
	char *sql;
	char state[16];
	int found;
	int ret;

	sql = vex_format("SELECT state FROM objects WHERE repo_id = %s "
			 "AND oid = %s FOR UPDATE", repo, object);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate object publication query");
	ret = query_one_text(db, sql, state, sizeof(state), &found);
	free(sql);
	if (ret)
		return ret;
	if (!found)
		return set_error(db, VEX_META_MYSQL_ERR_NOT_FOUND,
				 "Vex ref target object does not exist");
	if (strcmp(state, "published"))
		return set_error(db, VEX_META_MYSQL_ERR_STATE,
				 "Vex ref target object is not published");
	return VEX_META_MYSQL_OK;
}

static int mysql_begin(struct vex_meta_mysql *db)
{
	return mysql_exec(db, "START TRANSACTION");
}

static void vex_mysql_rollback(struct vex_meta_mysql *db)
{
	(void)mysql_query(db->mysql, "ROLLBACK");
}

static int vex_mysql_commit(struct vex_meta_mysql *db)
{
	return mysql_exec(db, "COMMIT");
}

/*
 * Pin changes and a state-store write share this repository-row boundary.
 * Without it, a pin could be installed while state replacement is between
 * deleting and reinserting object rows.
 */
static int require_repository_locked(struct vex_meta_mysql *db,
				     const char *repo)
{
	char hash_algo[32];
	char *sql;
	int found;
	int ret;

	sql = vex_format("SELECT hash_algo FROM repositories WHERE repo_id = %s "
			 "FOR UPDATE", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate Vex repository lock query");
	ret = query_one_text(db, sql, hash_algo, sizeof(hash_algo), &found);
	free(sql);
	if (ret)
		return ret;
	if (!found)
		return set_error(db, VEX_META_MYSQL_ERR_NOT_FOUND,
				 "Vex repository does not exist");
	return VEX_META_MYSQL_OK;
}

static int acquire_generation_lock(struct vex_meta_mysql *db,
				   const char *repo_hex)
{
	char lock_name[64];
	char *quoted = NULL;
	char *sql = NULL;
	uint64_t locked;
	int found;
	int ret;

	snprintf(lock_name, sizeof(lock_name), "vex-generation-%s", repo_hex);
	quoted = sql_quote(db, lock_name);
	if (!quoted)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate MySQL advisory lock name");
	sql = vex_format("SELECT GET_LOCK(%s, 5)", quoted);
	free(quoted);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate MySQL advisory lock query");
	ret = query_one_u64(db, sql, &locked, &found);
	free(sql);
	if (ret)
		return ret;
	if (!found || locked != 1)
		return set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
				 "timed out acquiring Vex generation lock");
	return VEX_META_MYSQL_OK;
}

static int release_generation_lock(struct vex_meta_mysql *db,
				   const char *repo_hex)
{
	char lock_name[64];
	char *quoted = NULL;
	char *sql = NULL;
	uint64_t released;
	int found;
	int ret;

	snprintf(lock_name, sizeof(lock_name), "vex-generation-%s", repo_hex);
	quoted = sql_quote(db, lock_name);
	if (!quoted)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate MySQL advisory lock name");
	sql = vex_format("SELECT RELEASE_LOCK(%s)", quoted);
	free(quoted);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate MySQL advisory lock query");
	ret = query_one_u64(db, sql, &released, &found);
	free(sql);
	if (ret)
		return ret;
	if (!found || released != 1)
		return set_error(db, VEX_META_MYSQL_ERR_STATE,
				 "lost Vex generation advisory lock");
	return VEX_META_MYSQL_OK;
}

int vex_meta_mysql_open(struct vex_meta_mysql **out,
			const struct vex_meta_mysql_options *opts)
{
	struct vex_meta_mysql *db;

	if (!out || !opts || !opts->user || !opts->database)
		return VEX_META_MYSQL_ERR_INVALID;
	*out = NULL;
	db = calloc(1, sizeof(*db));
	if (!db)
		return VEX_META_MYSQL_ERR_NOMEM;
	db->mysql = mysql_init(NULL);
	if (!db->mysql) {
		free(db);
		return VEX_META_MYSQL_ERR_NOMEM;
	}
	/* Keep one explicit connection per metadata transaction; do not reconnect
	 * underneath an in-flight epoch or ref CAS. */
	if (!mysql_real_connect(db->mysql, opts->host ? opts->host : "127.0.0.1",
				opts->user, opts->password, opts->database,
				opts->port ? opts->port : 3306, opts->unix_socket, 0)) {
		set_error(db, VEX_META_MYSQL_ERR_CONNECT, "MySQL connect failed: %s",
			  mysql_error(db->mysql));
#ifdef VEX_META_MYSQL_STORE
		trace_printf("vex: MySQL connection to %s:%u/%s failed: %s",
			     opts->host ? opts->host : "127.0.0.1",
			     opts->port ? opts->port : 3306, opts->database,
			     mysql_error(db->mysql));
#endif
		mysql_close(db->mysql);
		free(db);
		return VEX_META_MYSQL_ERR_CONNECT;
	}
	if (mysql_set_character_set(db->mysql, "utf8mb4")) {
		set_mysql_error(db, "MySQL character set setup failed");
#ifdef VEX_META_MYSQL_STORE
		trace_printf("vex: MySQL character-set setup for %s:%u/%s failed: %s",
			     opts->host ? opts->host : "127.0.0.1",
			     opts->port ? opts->port : 3306, opts->database,
			     mysql_error(db->mysql));
#endif
		mysql_close(db->mysql);
		free(db);
		return VEX_META_MYSQL_ERR_CONNECT;
	}
	*out = db;
	return VEX_META_MYSQL_OK;
}

void vex_meta_mysql_close(struct vex_meta_mysql *db)
{
	if (!db)
		return;
	if (db->mysql)
		mysql_close(db->mysql);
	free(db);
}

const char *vex_meta_mysql_error(const struct vex_meta_mysql *db)
{
	if (!db || !db->error[0])
		return "no MySQL error recorded";
	return db->error;
}

int vex_meta_mysql_ping(struct vex_meta_mysql *db)
{
	if (!db || !db->mysql)
		return VEX_META_MYSQL_ERR_INVALID;
	if (mysql_ping(db->mysql))
		return set_mysql_error(db, "MySQL ping failed");
	return VEX_META_MYSQL_OK;
}

/*
 * ref_updates.id was originally made globally unique even though every Vex
 * state transaction locks only its repository row.  That made simultaneous
 * first pushes to two repositories race for the same manually assigned audit
 * id.  IDs are an audit sequence within a repository, so retain a leading
 * non-unique index for AUTO_INCREMENT callers instead.  The advisory lock
 * makes this upgrade safe when several smart-HTTP children open at once.
 */
static int ensure_ref_updates_id_index(struct vex_meta_mysql *db)
{
	static const char lock_sql[] =
		"SELECT GET_LOCK('vex-schema-ref-updates-id-v2', 5)";
	static const char unlock_sql[] =
		"SELECT RELEASE_LOCK('vex-schema-ref-updates-id-v2')";
	static const char index_sql[] =
		"SELECT non_unique FROM information_schema.statistics "
		"WHERE table_schema = DATABASE() AND table_name = 'ref_updates' "
		"AND index_name = 'ref_updates_id' LIMIT 1";
	uint64_t locked;
	uint64_t non_unique;
	uint64_t released;
	int found;
	int lock_found;
	int release_found;
	int ret;
	int release_ret;

	ret = query_one_u64(db, lock_sql, &locked, &lock_found);
	if (ret)
		return ret;
	if (!lock_found || locked != 1)
		return set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
				 "timed out acquiring Vex schema migration lock");

	ret = query_one_u64(db, index_sql, &non_unique, &found);
	if (!ret && found && non_unique != 0 && non_unique != 1)
		ret = set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				"ref_updates.id index has invalid uniqueness metadata");
	if (!ret && !found)
		ret = mysql_exec(db, "ALTER TABLE ref_updates ADD KEY ref_updates_id (id)");
	else if (!ret && found && !non_unique)
		ret = mysql_exec(db, "ALTER TABLE ref_updates DROP INDEX ref_updates_id, "
				 "ADD KEY ref_updates_id (id)");
	release_ret = query_one_u64(db, unlock_sql, &released, &release_found);
	if (!ret && (release_ret || !release_found || released != 1)) {
		if (release_ret)
			ret = release_ret;
		else
			ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
					"lost Vex schema migration lock");
	}
	return ret;
}

int vex_meta_mysql_schema_check(struct vex_meta_mysql *db,
				unsigned int *table_count)
{
	static const char table_sql[] =
		"SELECT COUNT(*) FROM information_schema.tables "
		"WHERE table_schema = DATABASE() AND table_name IN "
		"('repositories','generations','objects','generation_objects',"
		"'commit_parents','refs','ref_updates','object_pins',"
		"'pack_envelopes','pack_objects')";
	static const char blob_key_sql[] =
		"SELECT COUNT(*) FROM information_schema.columns "
		"WHERE table_schema = DATABASE() AND table_name = 'objects' "
		"AND column_name = 'blob_key' AND data_type = 'varbinary'";
	uint64_t tables;
	uint64_t blob_key;
	int found;
	int ret;

	if (!db || !db->mysql)
		return VEX_META_MYSQL_ERR_INVALID;
	ret = query_one_u64(db, table_sql, &tables, &found);
	if (ret)
		return ret;
	if (!found || tables != 10) {
		if (table_count)
			*table_count = found && tables <= UINT_MAX ? tables : 0;
		return set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				 "expected 10 Vex metadata tables, found %" PRIu64,
				 found ? tables : 0);
	}
	ret = query_one_u64(db, blob_key_sql, &blob_key, &found);
	if (ret)
		return ret;
	if (!found || blob_key != 1)
		return set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				 "objects.blob_key is not VARBINARY metadata");
	ret = ensure_ref_updates_id_index(db);
	if (ret)
		return ret;
	if (table_count)
		*table_count = (unsigned int)tables;
	return VEX_META_MYSQL_OK;
}

int vex_meta_mysql_ensure_repository(struct vex_meta_mysql *db,
				     const char *repo_id,
				     const char *hash_algo,
				     const char *name)
{
	char *repo = NULL;
	char *quoted_name = NULL;
	char *quoted_algo = NULL;
	char *sql = NULL;
	char existing_algo[32];
	int found;
	int ret;

	if (!db || !db->mysql || !hash_algo ||
	    (strcmp(hash_algo, "sha1") && strcmp(hash_algo, "sha256")) ||
	    (name && strlen(name) > 255))
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	quoted_name = sql_quote(db, name);
	quoted_algo = sql_quote(db, hash_algo);
	if (!repo || !quoted_name || !quoted_algo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate repository metadata query");
		goto out;
	}
	sql = vex_format("INSERT IGNORE INTO repositories "
			 "(repo_id, name, hash_algo) VALUES (%s, %s, %s)",
			 repo, quoted_name, quoted_algo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate repository insert query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	sql = vex_format("SELECT hash_algo FROM repositories WHERE repo_id = %s",
			 repo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate repository read query");
		goto out;
	}
	ret = query_one_text(db, sql, existing_algo, sizeof(existing_algo), &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (!found || strcmp(existing_algo, hash_algo)) {
		ret = set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
				"repository identity already has a different hash algorithm");
		goto out;
	}
	if (name) {
		sql = vex_format("UPDATE repositories SET name = %s WHERE repo_id = %s",
				 quoted_name, repo);
		if (!sql) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not allocate repository update query");
			goto out;
		}
		ret = mysql_exec(db, sql);
	}

out:
	free(sql);
	free(quoted_algo);
	free(quoted_name);
	free(repo);
	return ret;
}
int vex_meta_mysql_epoch_begin(struct vex_meta_mysql *db,
			       const char *repo_id, const char *push_id,
			       uint64_t *generation)
{
	char repo_hex[33];
	char *repo = NULL;
	char *push = NULL;
	char *sql = NULL;
	uint64_t previous;
	uint64_t next;
	int found;
	int locked = 0;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || !generation)
		return VEX_META_MYSQL_ERR_INVALID;
	*generation = 0;
	repo = sql_repo_literal(repo_id, repo_hex);
	push = sql_uuid_literal(push_id);
	if (!repo || !push) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid repository or push UUID");
		goto out;
	}
	ret = acquire_generation_lock(db, repo_hex);
	if (ret)
		goto out;
	locked = 1;
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	sql = vex_format("SELECT generation FROM generations WHERE repo_id = %s "
			 "ORDER BY generation DESC LIMIT 1 FOR UPDATE", repo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate generation query");
		goto out;
	}
	ret = query_one_u64(db, sql, &previous, &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (found && previous == UINT64_MAX) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"Vex generation counter exhausted");
		goto out;
	}
	next = found ? previous + 1 : 1;
	sql = vex_format("INSERT INTO generations "
			 "(repo_id, generation, state, push_id) VALUES "
			 "(%s, %" PRIu64 ", 'open', %s)", repo, next, push);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate generation insert query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	ret = vex_mysql_commit(db);
	if (ret)
		goto out;
	in_transaction = 0;
	*generation = next;

out:
	free(sql);
	if (in_transaction)
		vex_mysql_rollback(db);
	if (locked) {
		int unlock_ret = release_generation_lock(db, repo_hex);

		if (!ret)
			ret = unlock_ret;
	}
	free(push);
	free(repo);
	return ret;
}

int vex_meta_mysql_epoch_get_state(struct vex_meta_mysql *db,
				   const char *repo_id, uint64_t generation,
				   enum vex_meta_mysql_epoch_state *state)
{
	char *repo = NULL;
	char *sql = NULL;
	char value[16];
	int found;
	int ret;

	if (!db || !db->mysql || !generation || !state)
		return VEX_META_MYSQL_ERR_INVALID;
	*state = 0;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "invalid Vex repository UUID");
	sql = vex_format("SELECT state FROM generations WHERE repo_id = %s "
			 "AND generation = %" PRIu64, repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate epoch state query");
		goto out;
	}
	ret = query_one_text(db, sql, value, sizeof(value), &found);
	if (ret)
		goto out;
	if (!found) {
		ret = VEX_META_MYSQL_ERR_NOT_FOUND;
		goto out;
	}
	if (parse_epoch_state(value, state)) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned an invalid Vex epoch state");
		goto out;
	}
	ret = VEX_META_MYSQL_OK;

out:
	free(sql);
	free(repo);
	return ret;
}

int vex_meta_mysql_epoch_abort(struct vex_meta_mysql *db,
			       const char *repo_id, uint64_t generation)
{
	char *repo = NULL;
	char *sql = NULL;
	char value[16];
	int found;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || !generation)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "invalid Vex repository UUID");
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	sql = vex_format("SELECT state FROM generations WHERE repo_id = %s "
			 "AND generation = %" PRIu64 " FOR UPDATE", repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate epoch abort state query");
		goto out;
	}
	ret = query_one_text(db, sql, value, sizeof(value), &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (!found) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOT_FOUND,
				"cannot abort a missing Vex generation");
		goto out;
	}
	if (strcmp(value, "open")) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"cannot abort a non-open Vex generation");
		goto out;
	}

	/*
	 * First remove graph rows for objects which are solely claimed by this
	 * generation.  The matching object/claim deletes below preserve an object
	 * shared with another open epoch, or a pre-existing published object.
	 */
	sql = vex_format("DELETE cp FROM commit_parents AS cp "
			 "INNER JOIN objects AS o ON o.repo_id = cp.repo_id "
			 "AND o.oid = cp.commit_oid "
			 "INNER JOIN generation_objects AS own ON own.repo_id = o.repo_id "
			 "AND own.oid = o.oid "
			 "LEFT JOIN generation_objects AS other ON other.repo_id = o.repo_id "
			 "AND other.oid = o.oid AND other.generation <> %" PRIu64 " "
			 "WHERE own.repo_id = %s AND own.generation = %" PRIu64 " "
			 "AND o.state = 'quarantine' AND other.oid IS NULL",
			 generation, repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate commit-parent compensation query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;

	/* Drop this epoch's unique quarantine claims before deleting their rows. */
	sql = vex_format("DELETE own FROM generation_objects AS own "
			 "INNER JOIN objects AS o ON o.repo_id = own.repo_id "
			 "AND o.oid = own.oid "
			 "LEFT JOIN generation_objects AS other ON other.repo_id = own.repo_id "
			 "AND other.oid = own.oid AND other.generation <> %" PRIu64 " "
			 "WHERE own.repo_id = %s AND own.generation = %" PRIu64 " "
			 "AND o.state = 'quarantine' AND other.oid IS NULL",
			 generation, repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate generation-claim compensation query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;

	/* A normally staged object is owned by the generation that introduced it. */
	sql = vex_format("DELETE o FROM objects AS o "
			 "LEFT JOIN generation_objects AS remaining "
			 "ON remaining.repo_id = o.repo_id AND remaining.oid = o.oid "
			 "WHERE o.repo_id = %s AND o.generation = %" PRIu64 " "
			 "AND o.state = 'quarantine' AND remaining.oid IS NULL",
			 repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate object compensation query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;

	/* Remaining claims are shared quarantine or published-object claims. */
	sql = vex_format("DELETE FROM generation_objects WHERE repo_id = %s "
			 "AND generation = %" PRIu64, repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate final generation-claim cleanup query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;

	sql = vex_format("UPDATE generations SET state = 'aborted' "
			 "WHERE repo_id = %s AND generation = %" PRIu64 " "
			 "AND state = 'open'", repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate epoch abort update query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"Vex generation changed while being aborted");
		goto out;
	}
	ret = vex_mysql_commit(db);
	if (ret)
		goto out;
	in_transaction = 0;

out:
	free(sql);
	if (in_transaction)
		vex_mysql_rollback(db);
	free(repo);
	return ret;
}

int vex_meta_mysql_stage_object(struct vex_meta_mysql *db,
			 const char *repo_id, uint64_t generation,
			 const unsigned char *oid, size_t oid_len,
			 unsigned int object_type, uint64_t size,
			 const char *blob_key)
{
	char *repo = NULL;
	char *object = NULL;
	char *quoted_key = NULL;
	char *sql = NULL;
	uint64_t checked_generation;
	int found;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || !generation || object_type < 1 || object_type > 4 ||
	    !blob_key || strlen(blob_key) > 512)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	object = sql_oid_literal(oid, oid_len);
	quoted_key = sql_quote(db, blob_key);
	if (!repo || !object || !quoted_key) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex object metadata");
		goto out;
	}
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	sql = vex_format("SELECT generation FROM generations WHERE repo_id = %s "
			 "AND generation = %" PRIu64 " AND state = 'open' FOR UPDATE",
			 repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate epoch state query");
		goto out;
	}
	ret = query_one_u64(db, sql, &checked_generation, &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (!found || checked_generation != generation) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"cannot stage metadata in a non-open Vex generation");
		goto out;
	}
	/* blob_key is a location; this query deliberately has no byte payload. */
	sql = vex_format("INSERT IGNORE INTO objects "
			 "(repo_id, oid, obj_type, size, blob_key, generation, state) VALUES "
			 "(%s, %s, %u, %" PRIu64 ", %s, %" PRIu64 ", 'quarantine')",
			 repo, object, object_type, size, quoted_key, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate object metadata insert query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	sql = vex_format("INSERT IGNORE INTO generation_objects "
			 "(repo_id, generation, oid) VALUES (%s, %" PRIu64 ", %s)",
			 repo, generation, object);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate generation object insert query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	ret = vex_mysql_commit(db);
	if (ret)
		goto out;
	in_transaction = 0;

out:
	free(sql);
	if (in_transaction)
		vex_mysql_rollback(db);
	free(quoted_key);
	free(object);
	free(repo);
	return ret;
}

int vex_meta_mysql_object_get_visible(struct vex_meta_mysql *db,
				      const char *repo_id,
				      const unsigned char *oid, size_t oid_len,
				      uint64_t visible_generation,
				      struct vex_meta_mysql_object *object)
{
	char *repo = NULL;
	char *object_id = NULL;
	char *sql = NULL;
	char *blob_key = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	char state_value[16];
	uint64_t object_type;
	uint64_t size;
	uint64_t generation;
	enum vex_meta_mysql_object_state state;
	int ret = VEX_META_MYSQL_ERR;

	if (!db || !db->mysql || !object)
		return VEX_META_MYSQL_ERR_INVALID;
	memset(object, 0, sizeof(*object));
	repo = sql_repo_literal(repo_id, NULL);
	object_id = sql_oid_literal(oid, oid_len);
	if (!repo || !object_id) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex object visibility lookup");
		goto out;
	}
	if (visible_generation) {
		sql = vex_format("SELECT o.obj_type, o.size, o.blob_key, o.generation, "
				 "o.state FROM objects AS o WHERE o.repo_id = %s "
				 "AND o.oid = %s AND (o.state = 'published' OR "
				 "(o.state = 'quarantine' AND EXISTS (SELECT 1 "
				 "FROM generation_objects AS go INNER JOIN generations AS g "
				 "ON g.repo_id = go.repo_id AND g.generation = go.generation "
				 "WHERE go.repo_id = o.repo_id AND go.oid = o.oid "
				 "AND go.generation = %" PRIu64 " AND g.state = 'open')))",
				 repo, object_id, visible_generation);
	} else {
		sql = vex_format("SELECT o.obj_type, o.size, o.blob_key, o.generation, "
				 "o.state FROM objects AS o WHERE o.repo_id = %s "
				 "AND o.oid = %s AND o.state = 'published'", repo,
				 object_id);
	}
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate object visibility query");
		goto out;
	}
	if (mysql_query(db->mysql, sql)) {
		ret = set_mysql_error(db, "MySQL object visibility read failed");
		goto out;
	}
	res = mysql_store_result(db->mysql);
	if (!res) {
		ret = set_mysql_error(db, "MySQL object visibility result retrieval failed");
		goto out;
	}
	if (mysql_num_fields(res) != 5) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned an invalid Vex object visibility result");
		goto out;
	}
	row = mysql_fetch_row(res);
	if (!row) {
		ret = VEX_META_MYSQL_ERR_NOT_FOUND;
		goto out;
	}
	lengths = mysql_fetch_lengths(res);
	if (!row[0] || !row[1] || !row[2] || !row[3] || !row[4] || !lengths ||
	    parse_u64(row[0], &object_type) || object_type < 1 || object_type > 4 ||
	    parse_u64(row[1], &size) || parse_u64(row[3], &generation) ||
	    !lengths[2] || lengths[2] > 512 || memchr(row[2], '\0', lengths[2])) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid Vex object metadata");
		goto out;
	}
	if (lengths[4] >= sizeof(state_value)) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned an invalid Vex object state");
		goto out;
	}
	memcpy(state_value, row[4], lengths[4]);
	state_value[lengths[4]] = '\0';
	if (parse_object_state(state_value, &state)) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned an invalid Vex object state");
		goto out;
	}
	blob_key = malloc(lengths[2] + 1);
	if (!blob_key) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex blob key metadata");
		goto out;
	}
	memcpy(blob_key, row[2], lengths[2]);
	blob_key[lengths[2]] = '\0';
	object->object_type = (unsigned int)object_type;
	object->size = size;
	object->generation = generation;
	object->state = state;
	object->blob_key = blob_key;
	blob_key = NULL;
	ret = VEX_META_MYSQL_OK;

out:
	free(blob_key);
	if (res)
		mysql_free_result(res);
	free(sql);
	free(object_id);
	free(repo);
	return ret;
}

void vex_meta_mysql_object_release(struct vex_meta_mysql_object *object)
{
	if (!object)
		return;
	free(object->blob_key);
	memset(object, 0, sizeof(*object));
}

int vex_meta_mysql_add_commit_parent(struct vex_meta_mysql *db,
			     const char *repo_id,
			     const unsigned char *commit_oid, size_t commit_oid_len,
			     unsigned int parent_pos,
			     const unsigned char *parent_oid, size_t parent_oid_len)
{
	char *repo = NULL;
	char *commit = NULL;
	char *parent = NULL;
	char *sql = NULL;
	int ret;

	if (!db || !db->mysql)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	commit = sql_oid_literal(commit_oid, commit_oid_len);
	parent = sql_oid_literal(parent_oid, parent_oid_len);
	if (!repo || !commit || !parent) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex commit parent metadata");
		goto out;
	}
	sql = vex_format("INSERT INTO commit_parents "
			 "(repo_id, commit_oid, parent_pos, parent_oid) VALUES "
			 "(%s, %s, %u, %s)", repo, commit, parent_pos, parent);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate commit parent metadata query");
		goto out;
	}
	ret = mysql_exec(db, sql);

out:
	free(sql);
	free(parent);
	free(commit);
	free(repo);
	return ret;
}

int vex_meta_mysql_epoch_seal(struct vex_meta_mysql *db,
			      const char *repo_id, uint64_t generation)
{
	char *repo = NULL;
	char *sql = NULL;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || !generation)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "invalid Vex repository UUID");
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	sql = vex_format("UPDATE generations SET state = 'sealed', "
			 "sealed_at = CURRENT_TIMESTAMP(6) WHERE repo_id = %s "
			 "AND generation = %" PRIu64 " AND state = 'open'", repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate generation seal query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"cannot seal a missing or non-open Vex generation");
		goto out;
	}
	sql = vex_format("UPDATE objects AS o INNER JOIN generation_objects AS go "
			 "ON go.repo_id = o.repo_id AND go.oid = o.oid "
			 "SET o.state = 'published', "
			 "o.published_at = COALESCE(o.published_at, CURRENT_TIMESTAMP(6)) "
			 "WHERE go.repo_id = %s AND go.generation = %" PRIu64,
			 repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate object publication query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	ret = vex_mysql_commit(db);
	if (ret)
		goto out;
	in_transaction = 0;

out:
	free(sql);
	if (in_transaction)
		vex_mysql_rollback(db);
	free(repo);
	return ret;
}

static int query_locked_ref(struct vex_meta_mysql *db, const char *repo,
			    const char *quoted_refname, uint64_t *version,
			    char old_oid_hex[65], int *found)
{
	char *sql;
	MYSQL_RES *res;
	MYSQL_ROW row;
	unsigned long *lengths;

	*found = 0;
	sql = vex_format("SELECT version, HEX(target_oid) FROM refs "
			 "WHERE repo_id = %s AND refname = %s FOR UPDATE", repo,
			 quoted_refname);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate ref read query");
	if (mysql_query(db->mysql, sql)) {
		free(sql);
		return set_mysql_error(db, "MySQL ref read failed");
	}
	free(sql);
	res = mysql_store_result(db->mysql);
	if (!res)
		return set_mysql_error(db, "MySQL ref result retrieval failed");
	row = mysql_fetch_row(res);
	if (!row) {
		mysql_free_result(res);
		return VEX_META_MYSQL_OK;
	}
	lengths = mysql_fetch_lengths(res);
	if (!row[0] || !row[1] || !lengths || lengths[1] != 64 ||
	    parse_u64(row[0], version)) {
		mysql_free_result(res);
		return set_error(db, VEX_META_MYSQL_ERR,
				 "MySQL returned invalid Vex ref metadata");
	}
	memcpy(old_oid_hex, row[1], 64);
	old_oid_hex[64] = '\0';
	*found = 1;
	mysql_free_result(res);
	return VEX_META_MYSQL_OK;
}

int vex_meta_mysql_ref_cas_direct(struct vex_meta_mysql *db,
				  const char *repo_id, const char *refname,
				  uint64_t expected_version,
				  const unsigned char *new_oid, size_t new_oid_len,
				  const char *reason, const char *message,
				  uint64_t *new_version)
{
	char *repo = NULL;
	char *quoted_refname = NULL;
	char *new_object = NULL;
	char *quoted_reason = NULL;
	char *quoted_message = NULL;
	char *old_object = NULL;
	char *sql = NULL;
	char old_oid_hex[65];
	uint64_t current_version = 0;
	uint64_t next_version;
	int found;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || !refname || !*refname ||
	    strlen(refname) > 1024 || (reason && strlen(reason) > 32) ||
	    (message && strlen(message) > 1024))
		return VEX_META_MYSQL_ERR_INVALID;
	if (new_version)
		*new_version = 0;
	repo = sql_repo_literal(repo_id, NULL);
	quoted_refname = sql_quote(db, refname);
	new_object = sql_oid_literal(new_oid, new_oid_len);
	quoted_reason = sql_quote(db, reason ? reason : "update");
	quoted_message = sql_quote(db, message);
	if (!repo || !quoted_refname || !new_object || !quoted_reason ||
	    !quoted_message) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex direct ref metadata");
		goto out;
	}
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	ret = require_published_object_locked(db, repo, new_object);
	if (ret)
		goto out;
	ret = query_locked_ref(db, repo, quoted_refname, &current_version,
			       old_oid_hex, &found);
	if (ret)
		goto out;
	if (!found) {
		if (expected_version != VEX_META_MYSQL_VERSION_ABSENT) {
			ret = set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
					"Vex ref does not exist for requested CAS");
			goto out;
		}
		next_version = 1;
		old_object = vex_strdup("NULL");
	} else {
		if (expected_version == VEX_META_MYSQL_VERSION_ABSENT ||
		    expected_version != current_version) {
			ret = set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
					"Vex ref version compare-and-swap failed");
			goto out;
		}
		if (current_version == UINT64_MAX) {
			ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
					"Vex ref version counter exhausted");
			goto out;
		}
		next_version = current_version + 1;
		old_object = vex_format("X'%s'", old_oid_hex);
	}
	if (!old_object) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate previous Vex ref object ID");
		goto out;
	}
	sql = vex_format("INSERT INTO refs "
			 "(repo_id, refname, is_symbolic, target_oid, symref_target, "
			 "peeled_oid, version) VALUES (%s, %s, FALSE, %s, NULL, NULL, "
			 "%" PRIu64 ") ON DUPLICATE KEY UPDATE is_symbolic = FALSE, "
			 "target_oid = %s, symref_target = NULL, peeled_oid = NULL, "
			 "version = %" PRIu64,
			 repo, quoted_refname, new_object, next_version, new_object,
			 next_version);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex ref write query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	sql = vex_format("INSERT INTO ref_updates "
			 "(repo_id, refname, old_oid, new_oid, old_target, new_target, "
			 "is_ff, actor, push_id, reason, message) VALUES "
			 "(%s, %s, %s, %s, NULL, NULL, NULL, NULL, NULL, %s, %s)",
			 repo, quoted_refname, old_object, new_object, quoted_reason,
			 quoted_message);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex ref audit query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	ret = vex_mysql_commit(db);
	if (ret)
		goto out;
	in_transaction = 0;
	if (new_version)
		*new_version = next_version;

out:
	free(sql);
	if (in_transaction)
		vex_mysql_rollback(db);
	free(old_object);
	free(quoted_message);
	free(quoted_reason);
	free(new_object);
	free(quoted_refname);
	free(repo);
	return ret;
}

int vex_meta_mysql_pin_object(struct vex_meta_mysql *db,
			       const char *repo_id,
			       const unsigned char *oid, size_t oid_len,
			       const char *reason)
{
	char *repo = NULL;
	char *object = NULL;
	char *quoted_reason = NULL;
	char *sql = NULL;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || db->state_transaction || !reason || !*reason ||
	    strlen(reason) > 255)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	object = sql_oid_literal(oid, oid_len);
	quoted_reason = sql_quote(db, reason);
	if (!repo || !object || !quoted_reason) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex object pin metadata");
		goto out;
	}
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	if ((ret = require_repository_locked(db, repo)) ||
	    (ret = require_published_object_locked(db, repo, object)))
		goto out;
	/*
	 * Pins are idempotent roots.  Reasserting a retained pin intentionally
	 * clears any expiry a future retention job may have assigned.
	 */
	sql = vex_format("INSERT INTO object_pins "
			 "(repo_id, oid, reason, expires_at) VALUES (%s, %s, %s, NULL) "
			 "ON DUPLICATE KEY UPDATE expires_at = NULL",
			 repo, object, quoted_reason);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex object pin query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	ret = vex_mysql_commit(db);
	if (ret)
		goto out;
	in_transaction = 0;

out:
	free(sql);
	if (in_transaction)
		vex_mysql_rollback(db);
	free(quoted_reason);
	free(object);
	free(repo);
	return ret;
}

int vex_meta_mysql_unpin_object(struct vex_meta_mysql *db,
				 const char *repo_id,
				 const unsigned char *oid, size_t oid_len,
				 const char *reason)
{
	char *repo = NULL;
	char *object = NULL;
	char *quoted_reason = NULL;
	char *sql = NULL;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || db->state_transaction || !reason || !*reason ||
	    strlen(reason) > 255)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	object = sql_oid_literal(oid, oid_len);
	quoted_reason = sql_quote(db, reason);
	if (!repo || !object || !quoted_reason) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex object pin metadata");
		goto out;
	}
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	if ((ret = require_repository_locked(db, repo)))
		goto out;
	sql = vex_format("DELETE FROM object_pins WHERE repo_id = %s "
			 "AND oid = %s AND reason = %s", repo, object, quoted_reason);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex object unpin query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOT_FOUND,
				"Vex object pin does not exist");
		goto out;
	}
	ret = vex_mysql_commit(db);
	if (ret)
		goto out;
	in_transaction = 0;

out:
	free(sql);
	if (in_transaction)
		vex_mysql_rollback(db);
	free(quoted_reason);
	free(object);
	free(repo);
	return ret;
}

int vex_meta_mysql_for_each_active_pin(struct vex_meta_mysql *db,
					const char *repo_id,
					vex_meta_mysql_pin_iter_fn fn,
					void *cb_data)
{
	char *repo = NULL;
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret = VEX_META_MYSQL_OK;

	if (!db || !db->mysql || !fn)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID");
		goto out;
	}
	sql = vex_format("SELECT oid FROM object_pins WHERE repo_id = %s "
			 "AND (expires_at IS NULL OR expires_at > CURRENT_TIMESTAMP(6)) "
			 "ORDER BY oid, reason", repo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex active-pin query");
		goto out;
	}
	if (mysql_query(db->mysql, sql)) {
		ret = set_mysql_error(db, "MySQL active-pin read failed");
		goto out;
	}
	res = mysql_store_result(db->mysql);
	if (!res) {
		ret = set_mysql_error(db, "MySQL active-pin result retrieval failed");
		goto out;
	}
	if (mysql_num_fields(res) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid Vex active-pin metadata");
		goto out;
	}
	while ((row = mysql_fetch_row(res))) {
		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !lengths ||
		    lengths[0] != VEX_META_MYSQL_OID_BYTES) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex active pin object ID");
			goto out;
		}
		ret = fn((const unsigned char *)row[0], cb_data);
		if (ret)
			goto out;
	}

out:
	if (res)
		mysql_free_result(res);
	free(sql);
	free(repo);
	return ret;
}
#ifdef VEX_META_MYSQL_STORE

static int state_oid_from_binary(const char *value, unsigned long length,
				 const struct git_hash_algo *hash_algo,
				 struct object_id *oid)
{
	unsigned int i;

	if (!value || length != VEX_META_MYSQL_OID_BYTES ||
	    !hash_algo || (hash_algo->rawsz != 20 && hash_algo->rawsz != 32))
		return -1;
	for (i = hash_algo->rawsz; i < VEX_META_MYSQL_OID_BYTES; i++)
		if ((unsigned char)value[i])
			return -1;
	memset(oid, 0, sizeof(*oid));
	memcpy(oid->hash, value, hash_algo->rawsz);
	return 0;
}

static char *state_oid_literal(const struct object_id *oid,
				       const struct git_hash_algo *hash_algo)
{
	if (!oid || !hash_algo)
		return NULL;
	return sql_oid_literal(oid->hash, hash_algo->rawsz);
}

static char *state_row_text(const char *value, unsigned long length)
{
	char *result;

	if (!value || memchr(value, '\0', length))
		return NULL;
	result = xmallocz(length);
	memcpy(result, value, length);
	return result;
}

static char *state_row_uuid(const char *value, unsigned long length)
{
	char *result;

	if (!value)
		return NULL;
	if (length != 16)
		return NULL;
	result = xmallocz(VEX_META_MYSQL_REPO_ID_HEXSZ);
	bytes_to_hex((const unsigned char *)value, length, result);
	return result;
}

static char *state_uuid_literal_or_null(const char *value)
{
	char *literal;

	if (!value)
		return vex_strdup("NULL");
	literal = sql_uuid_literal(value);
	/* Internal one-off object publication uses a descriptive non-UUID id. */
	return literal ? literal : vex_strdup("NULL");
}

static int state_query_result(struct vex_meta_mysql *db, const char *sql,
			      MYSQL_RES **result)
{
	if (mysql_query(db->mysql, sql))
		return set_mysql_error(db, "MySQL Vex state query failed");
	*result = mysql_store_result(db->mysql);
	if (!*result)
		return set_mysql_error(db, "MySQL Vex state result retrieval failed");
	return VEX_META_MYSQL_OK;
}

static int state_verify_repository(struct vex_meta_mysql *db, const char *repo,
				   const struct git_hash_algo *hash_algo)
{
	char *sql = NULL;
	char value[32];
	int found;
	int ret;

	sql = vex_format("SELECT hash_algo FROM repositories WHERE repo_id = %s", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate repository state query");
	ret = query_one_text(db, sql, value, sizeof(value), &found);
	free(sql);
	if (ret)
		return ret;
	if (!found)
		return VEX_META_MYSQL_ERR_NOT_FOUND;
	if (strcmp(value, hash_algo->name))
		return set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
				 "Vex repository hash algorithm does not match store");
	return VEX_META_MYSQL_OK;
}

int vex_meta_mysql_state_lock(struct vex_meta_mysql *db, const char *repo_id)
{
	char *repo = NULL;
	char *sql = NULL;
	char value[32];
	int found;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || db->state_transaction)
		return VEX_META_MYSQL_ERR_STATE;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "invalid Vex repository UUID");
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	sql = vex_format("SELECT hash_algo FROM repositories WHERE repo_id = %s "
			 "FOR UPDATE", repo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex state lock query");
		goto out;
	}
	ret = query_one_text(db, sql, value, sizeof(value), &found);
	if (ret)
		goto out;
	if (!found) {
		ret = VEX_META_MYSQL_ERR_NOT_FOUND;
		goto out;
	}
	db->state_transaction = 1;
	in_transaction = 0;
	ret = VEX_META_MYSQL_OK;

out:
	free(sql);
	if (in_transaction)
		vex_mysql_rollback(db);
	free(repo);
	return ret;
}

void vex_meta_mysql_state_unlock(struct vex_meta_mysql *db)
{
	if (!db || !db->mysql || !db->state_transaction)
		return;
	vex_mysql_rollback(db);
	db->state_transaction = 0;
}

static int state_load_epochs(struct vex_meta_mysql *db, const char *repo,
			     struct vex_meta_state *state)
{
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret;

	sql = vex_format("SELECT generation, state, push_id FROM generations "
			 "WHERE repo_id = %s ORDER BY generation", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate generation state query");
	ret = state_query_result(db, sql, &res);
	free(sql);
	if (ret)
		return ret;
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_epoch *epoch;
		uint64_t id;
		enum vex_meta_mysql_epoch_state epoch_state;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !lengths ||
		    parse_u64(row[0], &id) || !id ||
		    parse_epoch_state(row[1], &epoch_state) ||
		    (row[2] && lengths[2] != 16) || id == UINT64_MAX) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex generation metadata");
			goto out;
		}
		ALLOC_GROW(state->epochs, state->epochs_nr + 1, state->epochs_alloc);
		epoch = &state->epochs[state->epochs_nr++];
		epoch->id = id;
		epoch->state = (enum vex_epoch_state)epoch_state;
		epoch->push_id = state_row_uuid(row[2], row[2] ? lengths[2] : 0);
		if (row[2] && !epoch->push_id) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not allocate Vex generation push id");
			goto out;
		}
		if (id >= state->next_epoch)
			state->next_epoch = id + 1;
	}
	ret = VEX_META_MYSQL_OK;
out:
	mysql_free_result(res);
	return ret;
}

static int state_load_objects(struct vex_meta_mysql *db, const char *repo,
			      const struct git_hash_algo *hash_algo,
			      struct vex_meta_state *state)
{
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret;

	sql = vex_format("SELECT oid, obj_type, size, generation, state FROM objects "
			 "WHERE repo_id = %s ORDER BY oid", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate object state query");
	ret = state_query_result(db, sql, &res);
	free(sql);
	if (ret)
		return ret;
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_object *object;
		struct object_id parsed_oid;
		uint64_t object_type;
		uint64_t size;
		uint64_t generation;
		enum vex_meta_mysql_object_state object_state;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !row[2] || !row[3] || !row[4] || !lengths ||
		    state_oid_from_binary(row[0], lengths[0], hash_algo,
					  &parsed_oid) ||
		    parse_u64(row[1], &object_type) || object_type < OBJ_COMMIT ||
		    object_type > OBJ_TAG || parse_u64(row[2], &size) ||
		    size > ULONG_MAX || parse_u64(row[3], &generation) || !generation ||
		    parse_object_state(row[4], &object_state)) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex object metadata");
			goto out;
		}
		ALLOC_GROW(state->objects, state->objects_nr + 1, state->objects_alloc);
		object = &state->objects[state->objects_nr++];
		oidcpy(&object->oid, &parsed_oid);
		object->type = (enum object_type)object_type;
		object->size = (unsigned long)size;
		object->epoch = generation;
		object->state = (enum vex_object_state)object_state;
	}
	ret = VEX_META_MYSQL_OK;
out:
	mysql_free_result(res);
	return ret;
}

static int state_load_generation_objects(struct vex_meta_mysql *db,
					 const char *repo,
					 const struct git_hash_algo *hash_algo,
					 struct vex_meta_state *state)
{
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret;

	sql = vex_format("SELECT generation, oid FROM generation_objects "
			 "WHERE repo_id = %s ORDER BY generation, oid", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate generation-object state query");
	ret = state_query_result(db, sql, &res);
	free(sql);
	if (ret)
		return ret;
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_epoch_object *entry;
		uint64_t generation;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !lengths ||
		    parse_u64(row[0], &generation) || !generation) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex generation-object metadata");
			goto out;
		}
		ALLOC_GROW(state->epoch_objects, state->epoch_objects_nr + 1,
			   state->epoch_objects_alloc);
		entry = &state->epoch_objects[state->epoch_objects_nr++];
		entry->epoch = generation;
		if (state_oid_from_binary(row[1], lengths[1], hash_algo, &entry->oid)) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned an invalid Vex generation object id");
			goto out;
		}
	}
	ret = VEX_META_MYSQL_OK;
out:
	mysql_free_result(res);
	return ret;
}

static int state_target_from_row(const char *oid, unsigned long oid_length,
				 const char *symref, unsigned long symref_length,
				 int symbolic, const struct git_hash_algo *hash_algo,
				 int allow_none, struct vex_ref_target *target)
{
	memset(target, 0, sizeof(*target));
	if (oid && symref)
		return -1;
	if (symbolic) {
		if (oid || !symref || !symref_length)
			return -1;
		target->type = VEX_REF_TARGET_SYMREF;
		target->symref = state_row_text(symref, symref_length);
		return target->symref ? 0 : -1;
	}
	if (symref)
		return -1;
	if (!oid) {
		if (!allow_none)
			return -1;
		target->type = VEX_REF_TARGET_NONE;
		return 0;
	}
	target->type = VEX_REF_TARGET_OID;
	return state_oid_from_binary(oid, oid_length, hash_algo, &target->oid);
}

static int state_load_refs(struct vex_meta_mysql *db, const char *repo,
			   const struct git_hash_algo *hash_algo,
			   struct vex_meta_state *state)
{
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret;

	sql = vex_format("SELECT refname, is_symbolic, target_oid, symref_target, "
			 "version FROM refs WHERE repo_id = %s ORDER BY refname", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate ref state query");
	ret = state_query_result(db, sql, &res);
	free(sql);
	if (ret)
		return ret;
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_ref *ref;
		uint64_t symbolic;
		uint64_t version;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !row[4] || !lengths || !lengths[0] ||
		    parse_u64(row[1], &symbolic) || symbolic > 1 ||
		    parse_u64(row[4], &version) || !version) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex ref metadata");
			goto out;
		}
		ALLOC_GROW(state->refs, state->refs_nr + 1, state->refs_alloc);
		ref = &state->refs[state->refs_nr++];
		memset(ref, 0, sizeof(*ref));
		ref->name = state_row_text(row[0], lengths[0]);
		if (!ref->name || state_target_from_row(row[2], row[2] ? lengths[2] : 0,
							 row[3], row[3] ? lengths[3] : 0,
							 (int)symbolic, hash_algo, 0,
							 &ref->target)) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned an invalid Vex ref target");
			goto out;
		}
		ref->version = version;
	}
	ret = VEX_META_MYSQL_OK;
out:
	mysql_free_result(res);
	return ret;
}

static int state_update_target_from_row(const char *oid,
					unsigned long oid_length,
					const char *symref,
					unsigned long symref_length,
					const struct git_hash_algo *hash_algo,
					struct vex_ref_target *target)
{
	return state_target_from_row(oid, oid_length, symref, symref_length,
				     symref != NULL, hash_algo, 1, target);
}

static int state_load_ref_updates(struct vex_meta_mysql *db, const char *repo,
				  const struct git_hash_algo *hash_algo,
				  struct vex_meta_state *state)
{
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret;

	sql = vex_format("SELECT id, refname, old_oid, new_oid, old_target, "
			 "new_target, is_ff, actor, push_id, reason, message FROM ref_updates "
			 "WHERE repo_id = %s ORDER BY id", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate ref audit state query");
	ret = state_query_result(db, sql, &res);
	free(sql);
	if (ret)
		return ret;
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_ref_update *entry;
		struct vex_ref_update *update;
		uint64_t id;
		uint64_t is_ff;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !row[9] || !lengths || !lengths[1] ||
		    parse_u64(row[0], &id) || !id ||
		    (row[6] && (parse_u64(row[6], &is_ff) || is_ff > 1)) ||
		    (row[8] && lengths[8] != 16)) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex ref audit metadata");
			goto out;
		}
		ALLOC_GROW(state->ref_updates, state->ref_updates_nr + 1,
			   state->ref_updates_alloc);
		entry = &state->ref_updates[state->ref_updates_nr++];
		memset(entry, 0, sizeof(*entry));
		entry->id = id;
		update = &entry->update;
		update->is_fast_forward = row[6] ? (int)is_ff : -1;
		update->refname = state_row_text(row[1], lengths[1]);
		update->actor = row[7] ? state_row_text(row[7], lengths[7]) : NULL;
		update->push_id = row[8] ? state_row_uuid(row[8], lengths[8]) : NULL;
		update->reason = state_row_text(row[9], lengths[9]);
		update->message = row[10] ? state_row_text(row[10], lengths[10]) : NULL;
		if (!update->refname || !update->reason ||
		    (row[7] && !update->actor) || (row[8] && !update->push_id) ||
		    (row[10] && !update->message) ||
		    state_update_target_from_row(row[2], row[2] ? lengths[2] : 0,
						 row[4], row[4] ? lengths[4] : 0,
						 hash_algo, &update->old_target) ||
		    state_update_target_from_row(row[3], row[3] ? lengths[3] : 0,
						 row[5], row[5] ? lengths[5] : 0,
						 hash_algo, &update->new_target)) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned an invalid Vex ref audit target");
			goto out;
		}
	}
	ret = VEX_META_MYSQL_OK;
out:
	mysql_free_result(res);
	return ret;
}

static int state_load_commit_parents(struct vex_meta_mysql *db, const char *repo,
				     const struct git_hash_algo *hash_algo,
				     struct vex_meta_state *state)
{
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret;

	sql = vex_format("SELECT commit_oid, parent_pos, parent_oid FROM commit_parents "
			 "WHERE repo_id = %s ORDER BY commit_oid, parent_pos", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate commit-parent state query");
	ret = state_query_result(db, sql, &res);
	free(sql);
	if (ret)
		return ret;
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_commit_parent *entry;
		uint64_t position;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !row[2] || !lengths ||
		    parse_u64(row[1], &position) || position > UINT_MAX) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex commit-parent metadata");
			goto out;
		}
		ALLOC_GROW(state->commit_parents, state->commit_parents_nr + 1,
			   state->commit_parents_alloc);
		entry = &state->commit_parents[state->commit_parents_nr++];
		if (state_oid_from_binary(row[0], lengths[0], hash_algo,
					  &entry->commit_oid) ||
		    state_oid_from_binary(row[2], lengths[2], hash_algo,
					  &entry->parent_oid)) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex commit-parent object ids");
			goto out;
		}
		entry->position = (unsigned int)position;
	}
	ret = VEX_META_MYSQL_OK;
out:
	mysql_free_result(res);
	return ret;
}

static int state_load_next_update(struct vex_meta_mysql *db, const char *repo,
				  struct vex_meta_state *state)
{
	char *sql;
	uint64_t max_id;
	int found;
	int ret;

	sql = vex_format("SELECT COALESCE(MAX(id), 0) FROM ref_updates "
			 "WHERE repo_id = %s", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate Vex ref audit sequence query");
	ret = query_one_u64(db, sql, &max_id, &found);
	free(sql);
	if (ret)
		return ret;
	if (!found || max_id == UINT64_MAX)
		return set_error(db, VEX_META_MYSQL_ERR_STATE,
				 "Vex ref audit identifier counter exhausted");
	state->next_update = max_id + 1;
	return VEX_META_MYSQL_OK;
}

int vex_meta_mysql_state_load(struct vex_meta_mysql *db, const char *repo_id,
			       const struct git_hash_algo *hash_algo,
			       struct vex_meta_state *state)
{
	char *repo = NULL;
	int own_transaction = 0;
	int ret;

	if (!db || !db->mysql || !repo_id || !hash_algo || !state)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "invalid Vex repository UUID");
	vex_meta_state_init(state);
	if (!db->state_transaction) {
		ret = mysql_begin(db);
		if (ret)
			goto out;
		own_transaction = 1;
	}
	if ((ret = state_verify_repository(db, repo, hash_algo)) ||
	    (ret = state_load_epochs(db, repo, state)) ||
	    (ret = state_load_objects(db, repo, hash_algo, state)) ||
	    (ret = state_load_generation_objects(db, repo, hash_algo, state)) ||
	    (ret = state_load_refs(db, repo, hash_algo, state)) ||
	    (ret = state_load_ref_updates(db, repo, hash_algo, state)) ||
	    (ret = state_load_commit_parents(db, repo, hash_algo, state)) ||
	    (ret = state_load_next_update(db, repo, state)))
		goto out;
	if (own_transaction) {
		ret = vex_mysql_commit(db);
		if (ret)
			goto out;
		own_transaction = 0;
	}
	ret = VEX_META_MYSQL_OK;
out:
	if (own_transaction)
		vex_mysql_rollback(db);
	if (ret)
		vex_meta_state_release(state);
	free(repo);
	return ret;
}
struct vex_meta_mysql_saved_pin {
	unsigned char oid[VEX_META_MYSQL_OID_BYTES];
	char *reason;
	char *created_at;
	char *expires_at;
};

struct vex_meta_mysql_saved_pins {
	struct vex_meta_mysql_saved_pin *items;
	size_t nr;
	size_t alloc;
};

/*
 * Derived packs are immutable S3 artifacts.  The current state adapter still
 * replaces the ordinary per-repository metadata under one lock, so retain the
 * pack rows exactly through that replacement rather than making packs block
 * subsequent receives.  A future deletion path must obsolete a pack before it
 * deletes an object referenced by that pack.
 */
struct vex_meta_mysql_saved_pack {
	unsigned char pack_id[VEX_META_MYSQL_OID_BYTES];
	unsigned char *pack_key;
	size_t pack_key_len;
	unsigned char *idx_key;
	size_t idx_key_len;
	unsigned char *bitmap_key;
	size_t bitmap_key_len;
	uint64_t covering_generation;
	char *state;
	char *created_at;
	char *sealed_at;
	char *obsolete_at;
};

struct vex_meta_mysql_saved_pack_object {
	unsigned char pack_id[VEX_META_MYSQL_OID_BYTES];
	unsigned char oid[VEX_META_MYSQL_OID_BYTES];
};

struct vex_meta_mysql_saved_packs {
	struct vex_meta_mysql_saved_pack *items;
	size_t nr;
	size_t alloc;
	struct vex_meta_mysql_saved_pack_object *objects;
	size_t objects_nr;
	size_t objects_alloc;
};

static void state_saved_pins_release(struct vex_meta_mysql_saved_pins *pins)
{
	size_t i;

	for (i = 0; i < pins->nr; i++) {
		free(pins->items[i].reason);
		free(pins->items[i].created_at);
		free(pins->items[i].expires_at);
	}
	free(pins->items);
	memset(pins, 0, sizeof(*pins));
}

static unsigned char *state_row_bytes(const char *value, unsigned long length)
{
	unsigned char *result;

	if (!value)
		return NULL;
	result = malloc(length ? length : 1);
	if (!result)
		return NULL;
	if (length)
		memcpy(result, value, length);
	return result;
}

static void state_saved_packs_release(struct vex_meta_mysql_saved_packs *packs)
{
	size_t i;

	for (i = 0; i < packs->nr; i++) {
		free(packs->items[i].pack_key);
		free(packs->items[i].idx_key);
		free(packs->items[i].bitmap_key);
		free(packs->items[i].state);
		free(packs->items[i].created_at);
		free(packs->items[i].sealed_at);
		free(packs->items[i].obsolete_at);
	}
	free(packs->items);
	free(packs->objects);
	memset(packs, 0, sizeof(*packs));
}

/*
 * The current MySQL state adapter rewrites ordinary store rows under one
 * repository lock.  Pins are independent durable roots, so preserve them
 * exactly across that rewrite instead of forbidding a repository from using
 * pins at all.
 */
static int state_load_pins(struct vex_meta_mysql *db, const char *repo,
			   struct vex_meta_mysql_saved_pins *pins)
{
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret;

	sql = vex_format("SELECT oid, reason, "
			 "DATE_FORMAT(created_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f'), "
			 "DATE_FORMAT(expires_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f') "
			 "FROM object_pins WHERE repo_id = %s ORDER BY oid, reason", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate Vex pin preservation query");
	ret = state_query_result(db, sql, &res);
	free(sql);
	if (ret)
		return ret;
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_mysql_saved_pin *pin;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !row[2] || !lengths ||
		    lengths[0] != VEX_META_MYSQL_OID_BYTES || !lengths[1] ||
		    lengths[1] > 255 || lengths[2] > 32 ||
		    (row[3] && lengths[3] > 32)) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex object pin metadata");
			goto out;
		}
		ALLOC_GROW(pins->items, pins->nr + 1, pins->alloc);
		pin = &pins->items[pins->nr++];
		memset(pin, 0, sizeof(*pin));
		memcpy(pin->oid, row[0], sizeof(pin->oid));
		pin->reason = state_row_text(row[1], lengths[1]);
		pin->created_at = state_row_text(row[2], lengths[2]);
		pin->expires_at = row[3] ? state_row_text(row[3], lengths[3]) : NULL;
		if (!pin->reason || !pin->created_at ||
		    (row[3] && !pin->expires_at)) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not preserve Vex object pin metadata");
			goto out;
		}
	}
	ret = VEX_META_MYSQL_OK;
out:
	mysql_free_result(res);
	return ret;
}

static int state_insert_pins(struct vex_meta_mysql *db, const char *repo,
			     const struct vex_meta_mysql_saved_pins *pins)
{
	size_t i;

	for (i = 0; i < pins->nr; i++) {
		const struct vex_meta_mysql_saved_pin *pin = &pins->items[i];
		char *oid = sql_hex_literal(pin->oid, sizeof(pin->oid));
		char *reason = sql_quote(db, pin->reason);
		char *created_at = sql_quote(db, pin->created_at);
		char *expires_at = pin->expires_at ? sql_quote(db, pin->expires_at) :
			vex_strdup("NULL");
		char *sql;
		int ret;

		if (!oid || !reason || !created_at || !expires_at) {
			free(expires_at);
			free(created_at);
			free(reason);
			free(oid);
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not encode preserved Vex object pin");
		}
		sql = vex_format("INSERT INTO object_pins "
				 "(repo_id, oid, reason, expires_at, created_at) "
				 "VALUES (%s, %s, %s, %s, %s)",
				 repo, oid, reason, expires_at, created_at);
		free(expires_at);
		free(created_at);
		free(reason);
		free(oid);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex object pin restore query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (ret)
			return ret;
	}
	return VEX_META_MYSQL_OK;
}

static int state_load_packs(struct vex_meta_mysql *db, const char *repo,
				    struct vex_meta_mysql_saved_packs *packs)
{
	char *sql;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret;

	sql = vex_format("SELECT pack_id, pack_key, idx_key, bitmap_key, "
			 "covering_generation, state, "
			 "DATE_FORMAT(created_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f'), "
			 "DATE_FORMAT(sealed_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f'), "
			 "DATE_FORMAT(obsolete_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f') "
			 "FROM pack_envelopes WHERE repo_id = %s ORDER BY pack_id", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate Vex pack preservation query");
	ret = state_query_result(db, sql, &res);
	free(sql);
	if (ret)
		return ret;
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_mysql_saved_pack *pack;
		uint64_t covering_generation;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !row[4] || !row[5] || !row[6] || !lengths ||
		    lengths[0] != VEX_META_MYSQL_OID_BYTES || lengths[1] > 512 ||
		    (row[2] && lengths[2] > 512) ||
		    (row[3] && lengths[3] > 512) || lengths[5] == 0 ||
		    lengths[5] > 16 || lengths[6] > 32 ||
		    (row[7] && lengths[7] > 32) ||
		    (row[8] && lengths[8] > 32) ||
		    parse_u64(row[4], &covering_generation)) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex pack metadata");
			goto out;
		}
		ALLOC_GROW(packs->items, packs->nr + 1, packs->alloc);
		pack = &packs->items[packs->nr++];
		memset(pack, 0, sizeof(*pack));
		memcpy(pack->pack_id, row[0], sizeof(pack->pack_id));
		pack->pack_key = state_row_bytes(row[1], lengths[1]);
		pack->pack_key_len = lengths[1];
		if (row[2]) {
			pack->idx_key = state_row_bytes(row[2], lengths[2]);
			pack->idx_key_len = lengths[2];
		}
		if (row[3]) {
			pack->bitmap_key = state_row_bytes(row[3], lengths[3]);
			pack->bitmap_key_len = lengths[3];
		}
		pack->covering_generation = covering_generation;
		pack->state = state_row_text(row[5], lengths[5]);
		pack->created_at = state_row_text(row[6], lengths[6]);
		pack->sealed_at = row[7] ? state_row_text(row[7], lengths[7]) : NULL;
		pack->obsolete_at = row[8] ? state_row_text(row[8], lengths[8]) : NULL;
		if (!pack->pack_key || !pack->state || !pack->created_at ||
		    (row[2] && !pack->idx_key) ||
		    (row[3] && !pack->bitmap_key) ||
		    (row[7] && !pack->sealed_at) ||
		    (row[8] && !pack->obsolete_at)) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not preserve Vex pack metadata");
			goto out;
		}
	}
	mysql_free_result(res);
	res = NULL;

	sql = vex_format("SELECT pack_id, oid FROM pack_objects "
			 "WHERE repo_id = %s ORDER BY pack_id, oid", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate Vex pack-object preservation query");
	ret = state_query_result(db, sql, &res);
	free(sql);
	if (ret)
		return ret;
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_mysql_saved_pack_object *object;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !lengths ||
		    lengths[0] != VEX_META_MYSQL_OID_BYTES ||
		    lengths[1] != VEX_META_MYSQL_OID_BYTES) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex pack object metadata");
			goto out;
		}
		ALLOC_GROW(packs->objects, packs->objects_nr + 1,
			   packs->objects_alloc);
		object = &packs->objects[packs->objects_nr++];
		memcpy(object->pack_id, row[0], sizeof(object->pack_id));
		memcpy(object->oid, row[1], sizeof(object->oid));
	}
	mysql_free_result(res);
	return VEX_META_MYSQL_OK;
out:
	if (res)
		mysql_free_result(res);
	return ret;
}

static int state_insert_packs(struct vex_meta_mysql *db, const char *repo,
				      const struct vex_meta_mysql_saved_packs *packs)
{
	size_t i;

	for (i = 0; i < packs->nr; i++) {
		const struct vex_meta_mysql_saved_pack *pack = &packs->items[i];
		char *pack_id = sql_hex_literal(pack->pack_id, sizeof(pack->pack_id));
		char *pack_key = pack->pack_key ?
			sql_hex_literal(pack->pack_key, pack->pack_key_len) : NULL;
		char *idx_key = pack->idx_key ?
			sql_hex_literal(pack->idx_key, pack->idx_key_len) : vex_strdup("NULL");
		char *bitmap_key = pack->bitmap_key ?
			sql_hex_literal(pack->bitmap_key, pack->bitmap_key_len) : vex_strdup("NULL");
		char *state = sql_quote(db, pack->state);
		char *created_at = sql_quote(db, pack->created_at);
		char *sealed_at = pack->sealed_at ? sql_quote(db, pack->sealed_at) :
			vex_strdup("NULL");
		char *obsolete_at = pack->obsolete_at ? sql_quote(db, pack->obsolete_at) :
			vex_strdup("NULL");
		char *sql;
		int ret;

		if (!pack_id || !pack_key || !idx_key || !bitmap_key || !state ||
		    !created_at || !sealed_at || !obsolete_at) {
			free(obsolete_at);
			free(sealed_at);
			free(created_at);
			free(state);
			free(bitmap_key);
			free(idx_key);
			free(pack_key);
			free(pack_id);
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not encode preserved Vex pack metadata");
		}
		sql = vex_format("INSERT INTO pack_envelopes "
				 "(repo_id, pack_id, pack_key, idx_key, bitmap_key, "
				 "covering_generation, state, created_at, sealed_at, obsolete_at) "
				 "VALUES (%s, %s, %s, %s, %s, %" PRIu64 ", %s, %s, %s, %s)",
				 repo, pack_id, pack_key, idx_key, bitmap_key,
				 pack->covering_generation, state, created_at, sealed_at, obsolete_at);
		free(obsolete_at);
		free(sealed_at);
		free(created_at);
		free(state);
		free(bitmap_key);
		free(idx_key);
		free(pack_key);
		free(pack_id);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex pack restore query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (ret)
			return ret;
	}
	for (i = 0; i < packs->objects_nr; i++) {
		const struct vex_meta_mysql_saved_pack_object *object =
			&packs->objects[i];
		char *pack_id = sql_hex_literal(object->pack_id,
						       sizeof(object->pack_id));
		char *oid = sql_hex_literal(object->oid, sizeof(object->oid));
		char *sql;
		int ret;

		if (!pack_id || !oid) {
			free(oid);
			free(pack_id);
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not encode preserved Vex pack object metadata");
		}
		sql = vex_format("INSERT INTO pack_objects (repo_id, pack_id, oid) "
				 "VALUES (%s, %s, %s)", repo, pack_id, oid);
		free(oid);
		free(pack_id);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex pack-object restore query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (ret)
			return ret;
	}
	return VEX_META_MYSQL_OK;
}

static int state_delete_rows(struct vex_meta_mysql *db, const char *repo)
{
	static const char *const tables[] = {
		"commit_parents", "object_pins", "generation_objects", "pack_objects",
		"pack_envelopes", "objects",
		"generations", "refs", "ref_updates",
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(tables); i++) {
		char *sql = vex_format("DELETE FROM %s WHERE repo_id = %s", tables[i],
					       repo);
		int ret;

		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex metadata delete query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (ret)
			return ret;
	}
	return VEX_META_MYSQL_OK;
}

static const char *state_epoch_name(enum vex_epoch_state state)
{
	switch (state) {
	case VEX_EPOCH_OPEN: return "open";
	case VEX_EPOCH_SEALED: return "sealed";
	case VEX_EPOCH_SWEEPABLE: return "sweepable";
	case VEX_EPOCH_SWEPT: return "swept";
	case VEX_EPOCH_ABORTED: return "aborted";
	}
	return NULL;
}

static const char *state_object_name(enum vex_object_state state)
{
	switch (state) {
	case VEX_OBJECT_QUARANTINE: return "quarantine";
	case VEX_OBJECT_PUBLISHED: return "published";
	}
	return NULL;
}

static char *state_blob_key(const char *repo_id, const struct object_id *oid,
			    const struct git_hash_algo *hash_algo)
{
	char hex[GIT_MAX_HEXSZ + 1];

	bytes_to_hex(oid->hash, hash_algo->rawsz, hex);
	return vex_format("git/v1/%s/objects/%s", repo_id, hex);
}

static int state_insert_epochs(struct vex_meta_mysql *db, const char *repo,
			       const struct vex_meta_state *state)
{
	size_t i;

	for (i = 0; i < state->epochs_nr; i++) {
		const struct vex_meta_epoch *epoch = &state->epochs[i];
		const char *name = state_epoch_name(epoch->state);
		char *push = state_uuid_literal_or_null(epoch->push_id);
		char *sql;
		int ret;

		if (!epoch->id || !name || !push) {
			free(push);
			return set_error(db, VEX_META_MYSQL_ERR_INVALID,
					 "invalid Vex epoch state");
		}
		sql = vex_format("INSERT INTO generations "
				 "(repo_id, generation, state, push_id, sealed_at) VALUES "
				 "(%s, %" PRIu64 ", '%s', %s, %s)",
				 repo, epoch->id, name, push,
				 epoch->state == VEX_EPOCH_OPEN ? "NULL" : "CURRENT_TIMESTAMP(6)");
		free(push);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex epoch insert query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (ret)
			return ret;
	}
	return VEX_META_MYSQL_OK;
}

static int state_insert_objects(struct vex_meta_mysql *db, const char *repo,
				const char *repo_id,
				const struct git_hash_algo *hash_algo,
				const struct vex_meta_state *state)
{
	size_t i;

	for (i = 0; i < state->objects_nr; i++) {
		const struct vex_meta_object *object = &state->objects[i];
		const char *name = state_object_name(object->state);
		char *oid = state_oid_literal(&object->oid, hash_algo);
		char *key = state_blob_key(repo_id, &object->oid, hash_algo);
		char *quoted_key = key ? sql_quote(db, key) : NULL;
		char *sql;
		int ret;

		free(key);
		if (!object->epoch || !name || !oid || !quoted_key ||
		    object->type < OBJ_COMMIT || object->type > OBJ_TAG) {
			free(quoted_key);
			free(oid);
			return set_error(db, VEX_META_MYSQL_ERR_INVALID,
					 "invalid Vex object state");
		}
		sql = vex_format("INSERT INTO objects "
				 "(repo_id, oid, obj_type, size, blob_key, generation, state, published_at) "
				 "VALUES (%s, %s, %u, %lu, %s, %" PRIu64 ", '%s', %s)",
				 repo, oid, object->type, object->size, quoted_key,
				 object->epoch, name,
				 object->state == VEX_OBJECT_PUBLISHED ? "CURRENT_TIMESTAMP(6)" : "NULL");
		free(quoted_key);
		free(oid);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex object insert query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (ret)
			return ret;
	}
	return VEX_META_MYSQL_OK;
}

static int state_insert_generation_objects(struct vex_meta_mysql *db,
					  const char *repo,
					  const struct git_hash_algo *hash_algo,
					  const struct vex_meta_state *state)
{
	size_t i;

	for (i = 0; i < state->epoch_objects_nr; i++) {
		const struct vex_meta_epoch_object *entry = &state->epoch_objects[i];
		char *oid = state_oid_literal(&entry->oid, hash_algo);
		char *sql;
		int ret;

		if (!entry->epoch || !oid) {
			free(oid);
			return set_error(db, VEX_META_MYSQL_ERR_INVALID,
					 "invalid Vex generation object state");
		}
		sql = vex_format("INSERT INTO generation_objects (repo_id, generation, oid) "
				 "VALUES (%s, %" PRIu64 ", %s)", repo, entry->epoch, oid);
		free(oid);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex generation object insert query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (ret)
			return ret;
	}
	return VEX_META_MYSQL_OK;
}

static int state_insert_commit_parents(struct vex_meta_mysql *db,
					const char *repo,
					const struct git_hash_algo *hash_algo,
					const struct vex_meta_state *state)
{
	size_t i;

	for (i = 0; i < state->commit_parents_nr; i++) {
		const struct vex_meta_commit_parent *entry = &state->commit_parents[i];
		char *commit = state_oid_literal(&entry->commit_oid, hash_algo);
		char *parent = state_oid_literal(&entry->parent_oid, hash_algo);
		char *sql;
		int ret;

		if (!commit || !parent) {
			free(parent);
			free(commit);
			return set_error(db, VEX_META_MYSQL_ERR_INVALID,
					 "invalid Vex commit-parent state");
		}
		sql = vex_format("INSERT INTO commit_parents "
				 "(repo_id, commit_oid, parent_pos, parent_oid) VALUES "
				 "(%s, %s, %u, %s)", repo, commit, entry->position, parent);
		free(parent);
		free(commit);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex commit-parent insert query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (ret)
			return ret;
	}
	return VEX_META_MYSQL_OK;
}

static int state_target_literals(struct vex_meta_mysql *db,
				 const struct vex_ref_target *target,
				 const struct git_hash_algo *hash_algo,
				 char **oid_out, char **symref_out)
{
	char *oid = NULL;
	char *symref = NULL;

	if (!target || !oid_out || !symref_out)
		return VEX_META_MYSQL_ERR_INVALID;
	switch (target->type) {
	case VEX_REF_TARGET_ANY:
	case VEX_REF_TARGET_NONE:
		oid = vex_strdup("NULL");
		symref = vex_strdup("NULL");
		break;
	case VEX_REF_TARGET_OID:
		oid = state_oid_literal(&target->oid, hash_algo);
		symref = vex_strdup("NULL");
		break;
	case VEX_REF_TARGET_SYMREF:
		if (!target->symref || !*target->symref || strlen(target->symref) > 1024)
			return VEX_META_MYSQL_ERR_INVALID;
		oid = vex_strdup("NULL");
		symref = sql_quote(db, target->symref);
		break;
	default:
		return VEX_META_MYSQL_ERR_INVALID;
	}
	if (!oid || !symref) {
		free(symref);
		free(oid);
		return VEX_META_MYSQL_ERR_NOMEM;
	}
	*oid_out = oid;
	*symref_out = symref;
	return VEX_META_MYSQL_OK;
}

static int state_insert_refs(struct vex_meta_mysql *db, const char *repo,
			     const struct git_hash_algo *hash_algo,
			     const struct vex_meta_state *state)
{
	size_t i;

	for (i = 0; i < state->refs_nr; i++) {
		const struct vex_meta_ref *ref = &state->refs[i];
		char *refname;
		char *oid = NULL;
		char *symref = NULL;
		char *sql;
		int ret;

		if (!ref->name || !*ref->name || strlen(ref->name) > 1024 ||
		    !ref->version || (ref->target.type != VEX_REF_TARGET_OID &&
				      ref->target.type != VEX_REF_TARGET_SYMREF))
			return set_error(db, VEX_META_MYSQL_ERR_INVALID,
					 "invalid Vex ref state");
		refname = sql_quote(db, ref->name);
		ret = state_target_literals(db, &ref->target, hash_algo, &oid, &symref);
		if (!refname || ret) {
			free(symref);
			free(oid);
			free(refname);
			return set_error(db, ret ? ret : VEX_META_MYSQL_ERR_NOMEM,
					 "could not encode Vex ref state");
		}
		sql = vex_format("INSERT INTO refs "
				 "(repo_id, refname, is_symbolic, target_oid, symref_target, "
				 "peeled_oid, version) VALUES (%s, %s, %s, %s, %s, NULL, %" PRIu64 ")",
				 repo, refname,
				 ref->target.type == VEX_REF_TARGET_SYMREF ? "TRUE" : "FALSE",
				 oid, symref, ref->version);
		free(symref);
		free(oid);
		free(refname);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex ref insert query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (ret)
			return ret;
	}
	return VEX_META_MYSQL_OK;
}

static int state_insert_ref_updates(struct vex_meta_mysql *db, const char *repo,
				    const struct git_hash_algo *hash_algo,
				    const struct vex_meta_state *state)
{
	size_t i;

	for (i = 0; i < state->ref_updates_nr; i++) {
		const struct vex_meta_ref_update *entry = &state->ref_updates[i];
		const struct vex_ref_update *update = &entry->update;
		char *refname = NULL;
		char *old_oid = NULL;
		char *old_target = NULL;
		char *new_oid = NULL;
		char *new_target = NULL;
		char *actor = NULL;
		char *push_id = NULL;
		char *reason = NULL;
		char *message = NULL;
		char *sql = NULL;
		const char *is_ff;
		int ret = VEX_META_MYSQL_OK;

		if (!entry->id || !update->refname || !*update->refname ||
		    strlen(update->refname) > 1024 || !update->reason ||
		    !*update->reason || strlen(update->reason) > 32 ||
		    (update->actor && strlen(update->actor) > 255) ||
		    (update->message && strlen(update->message) > 1024) ||
		    (update->is_fast_forward != -1 && update->is_fast_forward != 0 &&
		     update->is_fast_forward != 1))
			return set_error(db, VEX_META_MYSQL_ERR_INVALID,
					 "invalid Vex ref audit state");
		refname = sql_quote(db, update->refname);
		actor = sql_quote(db, update->actor);
		push_id = state_uuid_literal_or_null(update->push_id);
		reason = sql_quote(db, update->reason);
		message = sql_quote(db, update->message);
		if (!refname || !actor || !push_id || !reason || !message ||
		    (ret = state_target_literals(db, &update->old_target, hash_algo,
						 &old_oid, &old_target)) ||
		    (ret = state_target_literals(db, &update->new_target, hash_algo,
						 &new_oid, &new_target))) {
			if (!ret)
				ret = VEX_META_MYSQL_ERR_NOMEM;
			ret = set_error(db, ret, "could not encode Vex ref audit state");
			goto out;
		}
		is_ff = update->is_fast_forward < 0 ? "NULL" :
			(update->is_fast_forward ? "TRUE" : "FALSE");
		sql = vex_format("INSERT INTO ref_updates "
				 "(repo_id, id, refname, old_oid, new_oid, old_target, new_target, "
				 "is_ff, actor, push_id, reason, message) VALUES "
				 "(%s, %" PRIu64 ", %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
				 repo, entry->id, refname, old_oid, new_oid, old_target,
				 new_target, is_ff, actor, push_id, reason, message);
		if (!sql) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not allocate Vex ref audit insert query");
			goto out;
		}
		ret = mysql_exec(db, sql);
out:
		free(sql);
		free(message);
		free(reason);
		free(push_id);
		free(actor);
		free(new_target);
		free(new_oid);
		free(old_target);
		free(old_oid);
		free(refname);
		if (ret)
			return ret;
	}
	return VEX_META_MYSQL_OK;
}

int vex_meta_mysql_state_save_locked(struct vex_meta_mysql *db,
				      const char *repo_id,
				      const struct git_hash_algo *hash_algo,
				      const struct vex_meta_state *state)
{
	char *repo;
	struct vex_meta_mysql_saved_pins pins = { 0 };
	struct vex_meta_mysql_saved_packs packs = { 0 };
	int ret;

	if (!db || !db->mysql || !db->state_transaction || !repo_id || !hash_algo ||
	    !state)
		return VEX_META_MYSQL_ERR_STATE;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "invalid Vex repository UUID");
	if ((ret = state_load_pins(db, repo, &pins)) ||
	    (ret = state_load_packs(db, repo, &packs)) ||
	    (ret = state_delete_rows(db, repo)) ||
	    (ret = state_insert_epochs(db, repo, state)) ||
	    (ret = state_insert_objects(db, repo, repo_id, hash_algo, state)) ||
	    (ret = state_insert_generation_objects(db, repo, hash_algo, state)) ||
	    (ret = state_insert_commit_parents(db, repo, hash_algo, state)) ||
	    (ret = state_insert_refs(db, repo, hash_algo, state)) ||
	    (ret = state_insert_ref_updates(db, repo, hash_algo, state)) ||
	    (ret = state_insert_pins(db, repo, &pins)) ||
	    (ret = state_insert_packs(db, repo, &packs)))
		goto out;
	ret = vex_mysql_commit(db);
	if (!ret)
		db->state_transaction = 0;
	out:
	state_saved_packs_release(&packs);
	state_saved_pins_release(&pins);
	free(repo);
	return ret;
}
#else /* VEX_META_MYSQL_STORE */

int vex_meta_mysql_state_lock(struct vex_meta_mysql *db, const char *repo_id)
{
	(void)repo_id;
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
}

void vex_meta_mysql_state_unlock(struct vex_meta_mysql *db)
{
	(void)db;
}

int vex_meta_mysql_state_load(struct vex_meta_mysql *db, const char *repo_id,
			       const struct git_hash_algo *hash_algo,
			       struct vex_meta_state *state)
{
	(void)repo_id;
	(void)hash_algo;
	(void)state;
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
}

int vex_meta_mysql_state_save_locked(struct vex_meta_mysql *db,
				      const char *repo_id,
				      const struct git_hash_algo *hash_algo,
				      const struct vex_meta_state *state)
{
	(void)repo_id;
	(void)hash_algo;
	(void)state;
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
}

#endif /* VEX_META_MYSQL_STORE */
#endif /* VEX_HAVE_MYSQL_CLIENT */

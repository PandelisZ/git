#include "vex/meta-mysql.h"

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

#else /* VEX_HAVE_MYSQL_CLIENT */

#include <mysql.h>

#define VEX_META_MYSQL_ERROR_SIZE 512

struct vex_meta_mysql {
	MYSQL *mysql;
	char error[VEX_META_MYSQL_ERROR_SIZE];
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
		mysql_close(db->mysql);
		free(db);
		return VEX_META_MYSQL_ERR_CONNECT;
	}
	if (mysql_set_character_set(db->mysql, "utf8mb4")) {
		set_mysql_error(db, "MySQL character set setup failed");
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

#endif /* VEX_HAVE_MYSQL_CLIENT */

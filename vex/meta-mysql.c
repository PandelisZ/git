#include "vex/meta-mysql.h"

#ifdef VEX_META_MYSQL_STORE
#include "git-compat-util.h"
#include "refs.h"
#include "strbuf.h"
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
#include <sys/stat.h>

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

int vex_meta_mysql_state_for_each_expired_open_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		unsigned int min_age_seconds,
		vex_meta_mysql_generation_iter_fn fn, void *cb_data)
{
	(void)db;
	(void)repo_id;
	(void)min_age_seconds;
	(void)fn;
	(void)cb_data;
	return mysql_unavailable();
}

int vex_meta_mysql_state_refresh_receive_heartbeat_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		uint64_t generation)
{
	(void)db;
	(void)repo_id;
	(void)generation;
	return mysql_unavailable();
}

int vex_meta_mysql_state_for_each_expired_sealed_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		unsigned int min_age_seconds,
		vex_meta_mysql_generation_iter_fn fn, void *cb_data)
{
	(void)db;
	(void)repo_id;
	(void)min_age_seconds;
	(void)fn;
	(void)cb_data;
	return mysql_unavailable();
}

int vex_meta_mysql_state_for_each_sweep_tombstone_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		vex_meta_mysql_sweep_tombstone_iter_fn fn, void *cb_data)
{
	(void)db;
	(void)repo_id;
	(void)fn;
	(void)cb_data;
	return mysql_unavailable();
}

int vex_meta_mysql_state_for_each_pending_sweep_blob_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		vex_meta_mysql_oid_iter_fn fn, void *cb_data)
{
	(void)db;
	(void)repo_id;
	(void)fn;
	(void)cb_data;
	return mysql_unavailable();
}

int vex_meta_mysql_state_mark_sweep_tombstone_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len, uint64_t generation,
		unsigned int tombstone_seconds, int *marked)
{
	(void)db;
	(void)repo_id;
	(void)oid;
	(void)oid_len;
	(void)generation;
	(void)tombstone_seconds;
	if (marked)
		*marked = 0;
	return mysql_unavailable();
}

int vex_meta_mysql_state_cancel_sweep_tombstone_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len, int *cancelled)
{
	(void)db;
	(void)repo_id;
	(void)oid;
	(void)oid_len;
	if (cancelled)
		*cancelled = 0;
	return mysql_unavailable();
}

int vex_meta_mysql_state_mark_sweep_metadata_deleted_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len)
{
	(void)db;
	(void)repo_id;
	(void)oid;
	(void)oid_len;
	return mysql_unavailable();
}

int vex_meta_mysql_state_prepare_sweep_blob_delete_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len, int *ready)
{
	(void)db;
	(void)repo_id;
	(void)oid;
	(void)oid_len;
	if (ready)
		*ready = 0;
	return mysql_unavailable();
}

int vex_meta_mysql_state_mark_sweep_blob_deleted_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len)
{
	(void)db;
	(void)repo_id;
	(void)oid;
	(void)oid_len;
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

int vex_meta_mysql_stage_objects(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct vex_meta_mysql_staged_object *objects, size_t objects_nr)
{
	(void)db;
	(void)repo_id;
	(void)generation;
	(void)objects;
	(void)objects_nr;
	return mysql_unavailable();
}

int vex_meta_mysql_receive_pack_stage_manifest(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct git_hash_algo *hash_algo,
	const struct vex_meta_mysql_receive_pack *pack,
	const char *manifest_path, uint64_t expected_members)
{
	(void)db;
	(void)repo_id;
	(void)generation;
	(void)hash_algo;
	(void)pack;
	(void)manifest_path;
	(void)expected_members;
	return mysql_unavailable();
}

int vex_meta_mysql_receive_pack_object_get_visible(
	struct vex_meta_mysql *db, const char *repo_id,
	const unsigned char *oid, size_t oid_len, uint64_t visible_generation,
	struct vex_meta_mysql_receive_pack_object *object)
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

void vex_meta_mysql_receive_pack_object_release(
	struct vex_meta_mysql_receive_pack_object *object)
{
	if (!object)
		return;
	free(object->pack_key);
	free(object->idx_key);
	memset(object, 0, sizeof(*object));
}

int vex_meta_mysql_stage_commit_parents(
	struct vex_meta_mysql *db, const char *repo_id,
	const struct vex_meta_mysql_commit_parent *parents, size_t parents_nr)
{
	(void)db;
	(void)repo_id;
	(void)parents;
	(void)parents_nr;
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

int vex_meta_mysql_current_sealed_pack(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const struct git_hash_algo *hash_algo,
				       struct vex_meta_mysql_pack *pack)
{
	(void)db;
	(void)repo_id;
	(void)hash_algo;
	if (pack)
		memset(pack, 0, sizeof(*pack));
	return mysql_unavailable();
}

void vex_meta_mysql_pack_release(struct vex_meta_mysql_pack *pack)
{
	if (!pack)
		return;
	free(pack->pack_key);
	free(pack->idx_key);
	memset(pack, 0, sizeof(*pack));
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

int vex_meta_mysql_receive_pack_seal_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct git_hash_algo *hash_algo,
	const struct vex_ref_update *updates, size_t updates_nr)
{
	(void)db;
	(void)repo_id;
	(void)generation;
	(void)hash_algo;
	(void)updates;
	(void)updates_nr;
	return mysql_unavailable();
}

int vex_meta_mysql_receive_pack_get_building_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	unsigned char out_pack_id[VEX_META_MYSQL_OID_BYTES])
{
	(void)db;
	(void)repo_id;
	(void)generation;
	if (out_pack_id)
		memset(out_pack_id, 0, VEX_META_MYSQL_OID_BYTES);
	return mysql_unavailable();
}

int vex_meta_mysql_receive_pack_abort_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	unsigned char out_pack_id[VEX_META_MYSQL_OID_BYTES])
{
	(void)db;
	(void)repo_id;
	(void)generation;
	if (out_pack_id)
		memset(out_pack_id, 0, VEX_META_MYSQL_OID_BYTES);
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

int vex_meta_mysql_clone_lease_acquire(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const unsigned char *pack_id, size_t pack_id_len,
				       const unsigned char lease_id[VEX_META_MYSQL_LEASE_ID_BYTES],
				       unsigned int ttl_seconds)
{
	(void)db;
	(void)repo_id;
	(void)pack_id;
	(void)pack_id_len;
	(void)lease_id;
	(void)ttl_seconds;
	return mysql_unavailable();
}

int vex_meta_mysql_clone_lease_release(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const unsigned char lease_id[VEX_META_MYSQL_LEASE_ID_BYTES])
{
	(void)db;
	(void)repo_id;
	(void)lease_id;
	return mysql_unavailable();
}

int vex_meta_mysql_for_each_active_pack_object(struct vex_meta_mysql *db,
						const char *repo_id,
						vex_meta_mysql_oid_iter_fn fn,
						void *cb_data)
{
	(void)db;
	(void)repo_id;
	(void)fn;
	(void)cb_data;
	return mysql_unavailable();
}

int vex_meta_mysql_for_each_active_clone_lease_object(
					struct vex_meta_mysql *db, const char *repo_id,
					vex_meta_mysql_oid_iter_fn fn, void *cb_data)
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

int vex_meta_mysql_state_commit_locked(struct vex_meta_mysql *db)
{
	(void)db;
	return mysql_unavailable();
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
#define VEX_META_MYSQL_SWEEP_MAX_SECONDS (365U * 24U * 60U * 60U)
#define VEX_META_MYSQL_PACK_REBUILD_DEFAULT_DELAY_SECONDS 60U
#define VEX_META_MYSQL_PACK_REBUILD_MAX_DELAY_SECONDS 86400U

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
	unsigned int local_infile = 1;

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
	/*
	 * The direct receive-pack path bulk-loads a trusted local manifest into a
	 * temporary table.  Enable only the client capability here; the server
	 * still independently controls local_infile and rejects it unless the
	 * deployment deliberately enables the feature.
	 */
	if (mysql_options(db->mysql, MYSQL_OPT_LOCAL_INFILE, &local_infile)) {
		set_error(db, VEX_META_MYSQL_ERR_CONNECT,
			  "MySQL LOCAL INFILE client setup failed: %s",
			  mysql_error(db->mysql));
		mysql_close(db->mysql);
		free(db);
		return VEX_META_MYSQL_ERR_CONNECT;
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

/*
 * Compose volumes predate clone leases.  Keep the additive migration in the
 * driver as well as schema.sql so an existing local MySQL volume becomes
 * usable on restart; a lease deliberately has no pack-row FK because normal
 * Vex state saves temporarily replace immutable pack rows.
 */
static int ensure_clone_leases_table(struct vex_meta_mysql *db)
{
	static const char create_sql[] =
		"CREATE TABLE IF NOT EXISTS clone_leases ("
		"repo_id BINARY(16) NOT NULL, "
		"lease_id BINARY(16) NOT NULL, "
		"pack_id BINARY(32) NOT NULL, "
		"expires_at DATETIME(6) NOT NULL, "
		"created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6), "
		"PRIMARY KEY (repo_id, lease_id), "
		"KEY clone_leases_active (repo_id, expires_at, pack_id), "
		"CONSTRAINT clone_leases_repo_fk FOREIGN KEY (repo_id) "
		"REFERENCES repositories (repo_id)"
		") ENGINE=InnoDB";

	return mysql_exec(db, create_sql);
}

/*
 * A repository may have at most one outstanding pack rebuild request.  It
 * holds no Git bytes and deliberately has no pack-row foreign key: a receive
 * can invalidate an immutable envelope before an operator-side builder has
 * materialized the replacement.
 */
static int ensure_pack_rebuild_requests_table(struct vex_meta_mysql *db)
{
	static const char create_sql[] =
		"CREATE TABLE IF NOT EXISTS pack_rebuild_requests ("
		"repo_id BINARY(16) NOT NULL, "
		"target_generation BIGINT UNSIGNED NOT NULL, "
		"requested_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6), "
		"last_requested_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6), "
		"not_before DATETIME(6) NOT NULL, "
		"PRIMARY KEY (repo_id), "
		"KEY pack_rebuild_requests_due (not_before, repo_id), "
		"CONSTRAINT pack_rebuild_requests_repo_fk FOREIGN KEY (repo_id) "
		"REFERENCES repositories (repo_id)"
		") ENGINE=InnoDB";

	return mysql_exec(db, create_sql);
}

/*
 * Existing Compose volumes predate destructive sweeping.  Keep this additive
 * migration alongside schema.sql so an operator can restart into a safe
 * tombstone protocol without resetting otherwise durable Vex metadata.
 */
static int ensure_sweep_tombstones_table(struct vex_meta_mysql *db)
{
	static const char create_sql[] =
		"CREATE TABLE IF NOT EXISTS object_sweep_tombstones ("
		"repo_id BINARY(16) NOT NULL, "
		"oid BINARY(32) NOT NULL, "
		"generation BIGINT UNSIGNED NOT NULL, "
		"marked_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6), "
		"delete_after DATETIME(6) NOT NULL, "
		"metadata_deleted_at DATETIME(6) NULL, "
		"blob_deleted_at DATETIME(6) NULL, "
		"PRIMARY KEY (repo_id, oid), "
		"KEY object_sweep_tombstones_pending "
		"(repo_id, metadata_deleted_at, blob_deleted_at, delete_after), "
		"CONSTRAINT object_sweep_tombstones_repo_fk FOREIGN KEY (repo_id) "
		"REFERENCES repositories (repo_id)"
		") ENGINE=InnoDB";

	return mysql_exec(db, create_sql);
}

/*
 * A raw receive pack has a different retention contract from a derived pack:
 * its S3 pair is canonical for the narrow full-mirror seed, so its members
 * must not be forced through objects/generation_objects just to make a pack
 * selectable.  Keep this additive migration in the driver for existing
 * Compose volumes as well as schema.sql for fresh databases.
 */
static int ensure_receive_pack_schema(struct vex_meta_mysql *db)
{
	static const char lock_sql[] =
		"SELECT GET_LOCK('vex-schema-receive-pack-v1', 5)";
	static const char unlock_sql[] =
		"SELECT RELEASE_LOCK('vex-schema-receive-pack-v1')";
	static const char source_kind_column_sql[] =
		"SELECT COUNT(*) FROM information_schema.columns "
		"WHERE table_schema = DATABASE() AND table_name = 'pack_envelopes' "
		"AND column_name = 'source_kind'";
	static const char receive_generation_column_sql[] =
		"SELECT COUNT(*) FROM information_schema.columns "
		"WHERE table_schema = DATABASE() AND table_name = 'pack_envelopes' "
		"AND column_name = 'receive_generation'";
	static const char full_snapshot_column_sql[] =
		"SELECT COUNT(*) FROM information_schema.columns "
		"WHERE table_schema = DATABASE() AND table_name = 'pack_envelopes' "
		"AND column_name = 'is_full_snapshot'";
	static const char receive_index_sql[] =
		"SELECT COUNT(DISTINCT index_name) FROM information_schema.statistics "
		"WHERE table_schema = DATABASE() AND table_name = 'pack_envelopes' "
		"AND index_name = 'pack_envelopes_receive'";
	static const char add_source_kind_sql[] =
		"ALTER TABLE pack_envelopes ADD COLUMN source_kind "
		"ENUM('derived', 'receive') NOT NULL DEFAULT 'derived' AFTER state";
	static const char add_receive_generation_sql[] =
		"ALTER TABLE pack_envelopes ADD COLUMN receive_generation "
		"BIGINT UNSIGNED NULL AFTER source_kind";
	static const char add_full_snapshot_sql[] =
		"ALTER TABLE pack_envelopes ADD COLUMN is_full_snapshot "
		"BOOLEAN NOT NULL DEFAULT TRUE AFTER receive_generation";
	static const char add_receive_index_sql[] =
		"ALTER TABLE pack_envelopes ADD INDEX pack_envelopes_receive "
		"(repo_id, source_kind, state, receive_generation)";
	static const char create_received_objects_sql[] =
		"CREATE TABLE IF NOT EXISTS received_pack_objects ("
		"repo_id BINARY(16) NOT NULL, "
		"pack_id BINARY(32) NOT NULL, "
		"oid BINARY(32) NOT NULL, "
		"obj_type TINYINT UNSIGNED NOT NULL, "
		"size BIGINT UNSIGNED NOT NULL, "
		"PRIMARY KEY (repo_id, pack_id, oid), "
		"KEY received_pack_objects_lookup (repo_id, oid, pack_id), "
		"CONSTRAINT received_pack_objects_pack_fk "
		"FOREIGN KEY (repo_id, pack_id) REFERENCES "
		"pack_envelopes (repo_id, pack_id)"
		") ENGINE=InnoDB";
	uint64_t locked;
	uint64_t columns;
	uint64_t indexes;
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
				 "timed out acquiring receive-pack schema migration lock");

	ret = query_one_u64(db, source_kind_column_sql, &columns, &found);
	if (!ret && (!found || columns > 1))
		ret = set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				"invalid Vex receive-pack source_kind schema");
	if (!ret && !columns)
		ret = mysql_exec(db, add_source_kind_sql);
	if (!ret)
		ret = query_one_u64(db, receive_generation_column_sql, &columns,
					    &found);
	if (!ret && (!found || columns > 1))
		ret = set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				"invalid Vex receive-pack generation schema");
	if (!ret && !columns)
		ret = mysql_exec(db, add_receive_generation_sql);
	if (!ret)
		ret = query_one_u64(db, full_snapshot_column_sql, &columns, &found);
	if (!ret && (!found || columns > 1))
		ret = set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				"invalid Vex receive-pack snapshot schema");
	if (!ret && !columns)
		ret = mysql_exec(db, add_full_snapshot_sql);
	if (!ret)
		ret = query_one_u64(db, receive_index_sql, &indexes, &found);
	if (!ret && (!found || indexes > 1))
		ret = set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				"invalid Vex receive-pack envelope index");
	if (!ret && !indexes)
		ret = mysql_exec(db, add_receive_index_sql);
	if (!ret)
		ret = mysql_exec(db, create_received_objects_sql);

	release_ret = query_one_u64(db, unlock_sql, &released, &release_found);
	if (!ret && (release_ret || !release_found || released != 1)) {
		if (release_ret)
			ret = release_ret;
		else
			ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
					"lost receive-pack schema migration lock");
	}
	return ret;
}

/*
 * Existing Compose volumes predate live receive heartbeats.  Keep this
 * additive migration here as well as schema.sql so a normal gateway restart
 * gains the protection without resetting durable repository metadata.
 */
static int ensure_generations_receive_heartbeat_column(struct vex_meta_mysql *db)
{
	static const char column_sql[] =
		"SELECT COUNT(*) FROM information_schema.columns "
		"WHERE table_schema = DATABASE() AND table_name = 'generations' "
		"AND column_name = 'receive_heartbeat_at'";
	static const char index_sql[] =
		"SELECT COUNT(DISTINCT index_name) FROM information_schema.statistics "
		"WHERE table_schema = DATABASE() AND table_name = 'generations' "
		"AND index_name = 'generations_open_heartbeat'";
	static const char alter_sql[] =
		"ALTER TABLE generations ADD COLUMN "
		"receive_heartbeat_at DATETIME(6) NULL AFTER expires_at";
	static const char add_index_sql[] =
		"ALTER TABLE generations ADD INDEX "
		"generations_open_heartbeat (repo_id, state, receive_heartbeat_at)";
	uint64_t columns;
	uint64_t indexes;
	int found;
	int ret;

	ret = query_one_u64(db, column_sql, &columns, &found);
	if (ret)
		return ret;
	if (!found || columns > 1)
		return set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				 "invalid Vex generations heartbeat schema");
	if (columns == 1)
		ret = VEX_META_MYSQL_OK;
	else
		ret = mysql_exec(db, alter_sql);
	if (ret)
		return ret;
	ret = query_one_u64(db, index_sql, &indexes, &found);
	if (ret)
		return ret;
	if (!found || indexes > 1)
		return set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				 "invalid Vex generations heartbeat index");
	if (indexes == 1)
		return VEX_META_MYSQL_OK;
	return mysql_exec(db, add_index_sql);
}

int vex_meta_mysql_schema_check(struct vex_meta_mysql *db,
				unsigned int *table_count)
{
	static const char table_sql[] =
		"SELECT COUNT(*) FROM information_schema.tables "
		"WHERE table_schema = DATABASE() AND table_name IN "
		"('repositories','generations','objects','generation_objects',"
		"'commit_parents','refs','ref_updates','object_pins',"
		"'pack_envelopes','pack_objects','clone_leases',"
		"'received_pack_objects','pack_rebuild_requests',"
		"'object_sweep_tombstones')";
	static const char blob_key_sql[] =
		"SELECT COUNT(*) FROM information_schema.columns "
		"WHERE table_schema = DATABASE() AND table_name = 'objects' "
		"AND column_name = 'blob_key' AND data_type = 'varbinary'";
	static const char heartbeat_sql[] =
		"SELECT COUNT(*) FROM information_schema.columns "
		"WHERE table_schema = DATABASE() AND table_name = 'generations' "
		"AND column_name = 'receive_heartbeat_at' AND data_type = 'datetime'";
	static const char receive_columns_sql[] =
		"SELECT COUNT(*) FROM information_schema.columns "
		"WHERE table_schema = DATABASE() AND table_name = 'pack_envelopes' "
		"AND ((column_name = 'source_kind' AND data_type = 'enum') OR "
		"(column_name = 'receive_generation' AND data_type = 'bigint') OR "
		"(column_name = 'is_full_snapshot' AND data_type = 'tinyint'))";
	static const char received_object_columns_sql[] =
		"SELECT COUNT(*) FROM information_schema.columns "
		"WHERE table_schema = DATABASE() AND table_name = 'received_pack_objects' "
		"AND ((column_name = 'repo_id' AND data_type = 'binary') OR "
		"(column_name = 'pack_id' AND data_type = 'binary') OR "
		"(column_name = 'oid' AND data_type = 'binary') OR "
		"(column_name = 'obj_type' AND data_type = 'tinyint') OR "
		"(column_name = 'size' AND data_type = 'bigint'))";
	uint64_t tables;
	uint64_t blob_key;
	uint64_t heartbeat;
	uint64_t receive_columns;
	uint64_t received_object_columns;
	int found;
	int ret;

	if (!db || !db->mysql)
		return VEX_META_MYSQL_ERR_INVALID;
	ret = ensure_generations_receive_heartbeat_column(db);
	if (ret)
		return ret;
	ret = ensure_clone_leases_table(db);
	if (ret)
		return ret;
	ret = ensure_pack_rebuild_requests_table(db);
	if (ret)
		return ret;
	ret = ensure_sweep_tombstones_table(db);
	if (ret)
		return ret;
	ret = ensure_receive_pack_schema(db);
	if (ret)
		return ret;
	ret = query_one_u64(db, table_sql, &tables, &found);
	if (ret)
		return ret;
	if (!found || tables != 14) {
		if (table_count)
			*table_count = found && tables <= UINT_MAX ? tables : 0;
		return set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				 "expected 14 Vex metadata tables, found %" PRIu64,
				 found ? tables : 0);
	}
	ret = query_one_u64(db, blob_key_sql, &blob_key, &found);
	if (ret)
		return ret;
	if (!found || blob_key != 1)
		return set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				 "objects.blob_key is not VARBINARY metadata");
	ret = query_one_u64(db, heartbeat_sql, &heartbeat, &found);
	if (ret)
		return ret;
	if (!found || heartbeat != 1)
		return set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				 "generations.receive_heartbeat_at is not durable datetime metadata");
	ret = query_one_u64(db, receive_columns_sql, &receive_columns, &found);
	if (ret)
		return ret;
	if (!found || receive_columns != 3)
		return set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				 "pack_envelopes receive metadata schema is incomplete");
	ret = query_one_u64(db, received_object_columns_sql,
				    &received_object_columns, &found);
	if (ret)
		return ret;
	if (!found || received_object_columns != 5)
		return set_error(db, VEX_META_MYSQL_ERR_SCHEMA,
				 "received_pack_objects metadata schema is incomplete");
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

	/* A raw receive map has no objects/generation_objects rows to clean up. */
	sql = vex_format("DELETE rpo FROM received_pack_objects AS rpo "
			 "INNER JOIN pack_envelopes AS p ON p.repo_id = rpo.repo_id "
			 "AND p.pack_id = rpo.pack_id WHERE p.repo_id = %s "
			 "AND p.source_kind = 'receive' AND p.state = 'building' "
			 "AND p.receive_generation = %" PRIu64, repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive-pack compensation query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	sql = vex_format("DELETE FROM pack_envelopes WHERE repo_id = %s "
			 "AND source_kind = 'receive' AND state = 'building' "
			 "AND receive_generation = %" PRIu64, repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive-pack envelope compensation");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;

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

int vex_meta_mysql_state_for_each_expired_open_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		unsigned int min_age_seconds,
		vex_meta_mysql_generation_iter_fn fn, void *cb_data)
{
	char *repo = NULL;
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	uint64_t *generations = NULL;
	size_t generations_nr = 0;
	size_t generations_alloc = 0;
	size_t i;
	int ret = VEX_META_MYSQL_OK;

	if (!db || !db->mysql || !db->state_transaction || !fn ||
	    min_age_seconds > 86400)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID");
		goto out;
	}
	sql = vex_format("SELECT generation FROM generations WHERE repo_id = %s "
			 "AND state = 'open' AND ("
			 "(receive_heartbeat_at IS NOT NULL AND receive_heartbeat_at <= "
			 "DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL %u SECOND)) OR "
			 "(receive_heartbeat_at IS NULL AND created_at <= "
			 "DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL %u SECOND))) "
			 "ORDER BY generation FOR UPDATE", repo, min_age_seconds,
			 min_age_seconds);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate expired Vex epoch query");
		goto out;
	}
	if (mysql_query(db->mysql, sql)) {
		ret = set_mysql_error(db, "MySQL expired Vex epoch read failed");
		goto out;
	}
	res = mysql_store_result(db->mysql);
	if (!res) {
		ret = set_mysql_error(db,
				"MySQL expired Vex epoch result retrieval failed");
		goto out;
	}
	if (mysql_num_fields(res) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid expired Vex epoch metadata");
		goto out;
	}
	while ((row = mysql_fetch_row(res))) {
		char *end;
		uintmax_t parsed;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !lengths || !lengths[0]) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned an invalid expired Vex generation");
			goto out;
		}
		errno = 0;
		parsed = strtoumax(row[0], &end, 10);
		if (errno || !*row[0] || *end || !parsed ||
		    parsed > UINT64_MAX) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned an invalid expired Vex generation");
			goto out;
		}
		ALLOC_GROW(generations, generations_nr + 1, generations_alloc);
		generations[generations_nr++] = parsed;
	}
	/* The result is buffered before callbacks mutate the loaded state. */
	mysql_free_result(res);
	res = NULL;
	for (i = 0; i < generations_nr; i++) {
		ret = fn(generations[i], cb_data);
		if (ret)
			goto out;
	}

out:
	if (res)
		mysql_free_result(res);
	free(generations);
	free(sql);
	free(repo);
	return ret;
}

int vex_meta_mysql_state_refresh_receive_heartbeat_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		uint64_t generation)
{
	char *repo = NULL;
	char *sql = NULL;
	int ret;

	if (!db || !db->mysql || !db->state_transaction || !generation)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "invalid Vex repository UUID");
	sql = vex_format("UPDATE generations SET receive_heartbeat_at = "
			 "CURRENT_TIMESTAMP(6) WHERE repo_id = %s "
			 "AND generation = %" PRIu64 " AND state = 'open'",
			 repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate receive heartbeat update query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != 1)
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"cannot heartbeat a missing or non-open Vex generation");
out:
	free(sql);
	free(repo);
	return ret;
}

int vex_meta_mysql_state_for_each_expired_sealed_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		unsigned int min_age_seconds,
		vex_meta_mysql_generation_iter_fn fn, void *cb_data)
{
	char *repo = NULL;
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	uint64_t *generations = NULL;
	size_t generations_nr = 0;
	size_t generations_alloc = 0;
	size_t i;
	int ret = VEX_META_MYSQL_OK;

	if (!db || !db->mysql || !db->state_transaction || !fn ||
	    !min_age_seconds || min_age_seconds > VEX_META_MYSQL_SWEEP_MAX_SECONDS)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID");
		goto out;
	}
	/*
	 * Do not trust a client timestamp for retention.  The seal timestamp is
	 * durable state and this comparison is evaluated by the MySQL server while
	 * the repository row remains locked.
	 */
	sql = vex_format("SELECT generation FROM generations WHERE repo_id = %s "
			 "AND state = 'sealed' AND sealed_at IS NOT NULL "
			 "AND sealed_at <= DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL %u SECOND) "
			 "ORDER BY generation FOR UPDATE", repo, min_age_seconds);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate expired sealed-generation query");
		goto out;
	}
	if (mysql_query(db->mysql, sql)) {
		ret = set_mysql_error(db, "MySQL expired sealed-generation read failed");
		goto out;
	}
	res = mysql_store_result(db->mysql);
	if (!res) {
		ret = set_mysql_error(db,
				"MySQL expired sealed-generation result retrieval failed");
		goto out;
	}
	if (mysql_num_fields(res) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid expired sealed-generation metadata");
		goto out;
	}
	while ((row = mysql_fetch_row(res))) {
		char *end;
		uintmax_t parsed;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !lengths || !lengths[0]) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned an invalid expired sealed generation");
			goto out;
		}
		errno = 0;
		parsed = strtoumax(row[0], &end, 10);
		if (errno || !*row[0] || *end || !parsed ||
		    parsed > UINT64_MAX) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned an invalid expired sealed generation");
			goto out;
		}
		ALLOC_GROW(generations, generations_nr + 1, generations_alloc);
		generations[generations_nr++] = parsed;
	}
	mysql_free_result(res);
	res = NULL;
	for (i = 0; i < generations_nr; i++) {
		ret = fn(generations[i], cb_data);
		if (ret)
			goto out;
	}

out:
	if (res)
		mysql_free_result(res);
	free(generations);
	free(sql);
	free(repo);
	return ret;
}

int vex_meta_mysql_state_for_each_sweep_tombstone_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		vex_meta_mysql_sweep_tombstone_iter_fn fn, void *cb_data)
{
	char *repo = NULL;
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	struct vex_meta_mysql_sweep_tombstone *items = NULL;
	size_t items_nr = 0;
	size_t items_alloc = 0;
	size_t i;
	int ret = VEX_META_MYSQL_OK;

	if (!db || !db->mysql || !db->state_transaction || !fn)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID");
		goto out;
	}
	sql = vex_format("SELECT oid, generation, "
			 "delete_after <= CURRENT_TIMESTAMP(6) "
			 "FROM object_sweep_tombstones WHERE repo_id = %s "
			 "AND metadata_deleted_at IS NULL ORDER BY oid FOR UPDATE", repo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex sweep-tombstone query");
		goto out;
	}
	if (mysql_query(db->mysql, sql)) {
		ret = set_mysql_error(db, "MySQL Vex sweep-tombstone read failed");
		goto out;
	}
	res = mysql_store_result(db->mysql);
	if (!res) {
		ret = set_mysql_error(db,
				"MySQL Vex sweep-tombstone result retrieval failed");
		goto out;
	}
	if (mysql_num_fields(res) != 3) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid Vex sweep-tombstone metadata");
		goto out;
	}
	while ((row = mysql_fetch_row(res))) {
		struct vex_meta_mysql_sweep_tombstone *item;
		char *end;
		uintmax_t generation;
		uintmax_t due;

		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !row[1] || !row[2] || !lengths ||
		    lengths[0] != VEX_META_MYSQL_OID_BYTES) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex sweep-tombstone row");
			goto out;
		}
		errno = 0;
		generation = strtoumax(row[1], &end, 10);
		if (errno || !*row[1] || *end || !generation ||
		    generation > UINT64_MAX) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex sweep-tombstone generation");
			goto out;
		}
		errno = 0;
		due = strtoumax(row[2], &end, 10);
		if (errno || !*row[2] || *end || due > 1) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex sweep-tombstone deadline");
			goto out;
		}
		ALLOC_GROW(items, items_nr + 1, items_alloc);
		item = &items[items_nr++];
		memset(item, 0, sizeof(*item));
		memcpy(item->oid, row[0], sizeof(item->oid));
		item->generation = generation;
		item->due = due;
	}
	mysql_free_result(res);
	res = NULL;
	for (i = 0; i < items_nr; i++) {
		ret = fn(&items[i], cb_data);
		if (ret)
			goto out;
	}

out:
	if (res)
		mysql_free_result(res);
	free(items);
	free(sql);
	free(repo);
	return ret;
}

int vex_meta_mysql_state_for_each_pending_sweep_blob_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		vex_meta_mysql_oid_iter_fn fn, void *cb_data)
{
	char *repo = NULL;
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	unsigned char (*oids)[VEX_META_MYSQL_OID_BYTES] = NULL;
	size_t oids_nr = 0;
	size_t oids_alloc = 0;
	size_t i;
	int ret = VEX_META_MYSQL_OK;

	if (!db || !db->mysql || !db->state_transaction || !fn)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID");
		goto out;
	}
	sql = vex_format("SELECT oid FROM object_sweep_tombstones "
			 "WHERE repo_id = %s AND metadata_deleted_at IS NOT NULL "
			 "AND blob_deleted_at IS NULL ORDER BY oid FOR UPDATE", repo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate pending Vex sweep-blob query");
		goto out;
	}
	if (mysql_query(db->mysql, sql)) {
		ret = set_mysql_error(db, "MySQL pending Vex sweep-blob read failed");
		goto out;
	}
	res = mysql_store_result(db->mysql);
	if (!res) {
		ret = set_mysql_error(db,
				"MySQL pending Vex sweep-blob result retrieval failed");
		goto out;
	}
	if (mysql_num_fields(res) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid pending Vex sweep-blob metadata");
		goto out;
	}
	while ((row = mysql_fetch_row(res))) {
		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !lengths ||
		    lengths[0] != VEX_META_MYSQL_OID_BYTES) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid pending Vex sweep-blob id");
			goto out;
		}
		ALLOC_GROW(oids, oids_nr + 1, oids_alloc);
		memcpy(oids[oids_nr++], row[0], VEX_META_MYSQL_OID_BYTES);
	}
	mysql_free_result(res);
	res = NULL;
	for (i = 0; i < oids_nr; i++) {
		ret = fn(oids[i], cb_data);
		if (ret)
			goto out;
	}

out:
	if (res)
		mysql_free_result(res);
	free(oids);
	free(sql);
	free(repo);
	return ret;
}

int vex_meta_mysql_state_mark_sweep_tombstone_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len, uint64_t generation,
		unsigned int tombstone_seconds, int *marked)
{
	char *repo = NULL;
	char *object = NULL;
	char *sql = NULL;
	int ret;

	if (marked)
		*marked = 0;
	if (!db || !db->mysql || !db->state_transaction || !generation ||
	    !tombstone_seconds ||
	    tombstone_seconds > VEX_META_MYSQL_SWEEP_MAX_SECONDS)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	object = sql_oid_literal(oid, oid_len);
	if (!repo || !object) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex sweep-tombstone metadata");
		goto out;
	}
	/*
	 * An OID can be republished after an older completed sweep.  Reset that
	 * historic tombstone only when its metadata was already deleted, so the
	 * newly published copy receives a full second grace period if it later
	 * becomes unreachable again.
	 */
	sql = vex_format("INSERT INTO object_sweep_tombstones "
			 "(repo_id, oid, generation, delete_after) VALUES "
			 "(%s, %s, %" PRIu64 ", DATE_ADD(CURRENT_TIMESTAMP(6), "
			 "INTERVAL %u SECOND)) ON DUPLICATE KEY UPDATE "
			 "generation = IF(metadata_deleted_at IS NOT NULL, %" PRIu64 ", generation), "
			 "marked_at = IF(metadata_deleted_at IS NOT NULL, CURRENT_TIMESTAMP(6), marked_at), "
			 "delete_after = IF(metadata_deleted_at IS NOT NULL, "
			 "DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL %u SECOND), delete_after), "
			 "blob_deleted_at = IF(metadata_deleted_at IS NOT NULL, NULL, blob_deleted_at), "
			 "metadata_deleted_at = IF(metadata_deleted_at IS NOT NULL, NULL, metadata_deleted_at)",
			 repo, object, generation, tombstone_seconds, generation,
			 tombstone_seconds);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex sweep-tombstone insert");
		goto out;
	}
	ret = mysql_exec(db, sql);
	if (!ret && marked)
		*marked = mysql_affected_rows(db->mysql) != 0;

out:
	free(sql);
	free(object);
	free(repo);
	return ret;
}

int vex_meta_mysql_state_cancel_sweep_tombstone_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len, int *cancelled)
{
	char *repo = NULL;
	char *object = NULL;
	char *sql = NULL;
	int ret;

	if (cancelled)
		*cancelled = 0;
	if (!db || !db->mysql || !db->state_transaction)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	object = sql_oid_literal(oid, oid_len);
	if (!repo || !object) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex sweep-tombstone cancellation");
		goto out;
	}
	sql = vex_format("DELETE FROM object_sweep_tombstones WHERE repo_id = %s "
			 "AND oid = %s AND metadata_deleted_at IS NULL", repo, object);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex sweep-tombstone cancellation");
		goto out;
	}
	ret = mysql_exec(db, sql);
	if (!ret && cancelled)
		*cancelled = mysql_affected_rows(db->mysql) == 1;

out:
	free(sql);
	free(object);
	free(repo);
	return ret;
}

int vex_meta_mysql_state_mark_sweep_metadata_deleted_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len)
{
	char *repo = NULL;
	char *object = NULL;
	char *sql = NULL;
	int ret;

	if (!db || !db->mysql || !db->state_transaction)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	object = sql_oid_literal(oid, oid_len);
	if (!repo || !object) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex sweep metadata deletion");
		goto out;
	}
	sql = vex_format("UPDATE object_sweep_tombstones "
			 "SET metadata_deleted_at = CURRENT_TIMESTAMP(6) "
			 "WHERE repo_id = %s AND oid = %s "
			 "AND metadata_deleted_at IS NULL", repo, object);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex sweep metadata deletion");
		goto out;
	}
	ret = mysql_exec(db, sql);
	if (!ret && mysql_affected_rows(db->mysql) != 1)
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"Vex sweep tombstone changed while deleting metadata");

out:
	free(sql);
	free(object);
	free(repo);
	return ret;
}

int vex_meta_mysql_state_prepare_sweep_blob_delete_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len, int *ready)
{
	char *repo = NULL;
	char *object = NULL;
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	uint64_t object_count;
	int found;
	int ret;

	if (ready)
		*ready = 0;
	if (!db || !db->mysql || !db->state_transaction || !ready)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	object = sql_oid_literal(oid, oid_len);
	if (!repo || !object) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex sweep blob-deletion recheck");
		goto out;
	}
	/* Lock the pending row before the caller performs its S3 DELETE. */
	sql = vex_format("SELECT oid FROM object_sweep_tombstones "
			 "WHERE repo_id = %s AND oid = %s "
			 "AND metadata_deleted_at IS NOT NULL AND blob_deleted_at IS NULL "
			 "FOR UPDATE", repo, object);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex sweep blob-deletion recheck");
		goto out;
	}
	if (mysql_query(db->mysql, sql)) {
		ret = set_mysql_error(db, "MySQL Vex sweep blob-deletion recheck failed");
		goto out;
	}
	res = mysql_store_result(db->mysql);
	if (!res) {
		ret = set_mysql_error(db,
				"MySQL Vex sweep blob-deletion result retrieval failed");
		goto out;
	}
	if (mysql_num_fields(res) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid Vex sweep blob-deletion metadata");
		goto out;
	}
	row = mysql_fetch_row(res);
	if (!row) {
		ret = VEX_META_MYSQL_OK;
		goto out;
	}
	lengths = mysql_fetch_lengths(res);
	if (!row[0] || !lengths || lengths[0] != VEX_META_MYSQL_OID_BYTES) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned an invalid pending Vex sweep blob id");
		goto out;
	}
	mysql_free_result(res);
	res = NULL;
	free(sql);
	sql = NULL;
	sql = vex_format("SELECT COUNT(*) FROM objects WHERE repo_id = %s "
			 "AND oid = %s", repo, object);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex sweep object recheck");
		goto out;
	}
	ret = query_one_u64(db, sql, &object_count, &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (!found) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL did not return Vex sweep object recheck metadata");
		goto out;
	}
	if (!object_count) {
		*ready = 1;
		ret = VEX_META_MYSQL_OK;
		goto out;
	}
	/* A republished immutable OID supersedes this historic cleanup record. */
	sql = vex_format("DELETE FROM object_sweep_tombstones "
			 "WHERE repo_id = %s AND oid = %s "
			 "AND metadata_deleted_at IS NOT NULL AND blob_deleted_at IS NULL",
			 repo, object);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate rescued Vex sweep tombstone deletion");
		goto out;
	}
	ret = mysql_exec(db, sql);
	if (!ret && mysql_affected_rows(db->mysql) != 1)
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"Vex sweep tombstone changed while preserving a republished object");

out:
	if (res)
		mysql_free_result(res);
	free(sql);
	free(object);
	free(repo);
	return ret;
}

int vex_meta_mysql_state_mark_sweep_blob_deleted_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len)
{
	char *repo = NULL;
	char *object = NULL;
	char *sql = NULL;
	int ret;

	if (!db || !db->mysql || !db->state_transaction)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	object = sql_oid_literal(oid, oid_len);
	if (!repo || !object) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex sweep blob-deletion metadata");
		goto out;
	}
	sql = vex_format("UPDATE object_sweep_tombstones "
			 "SET blob_deleted_at = CURRENT_TIMESTAMP(6) "
			 "WHERE repo_id = %s AND oid = %s "
			 "AND metadata_deleted_at IS NOT NULL AND blob_deleted_at IS NULL",
			 repo, object);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex sweep blob-deletion update");
		goto out;
	}
	ret = mysql_exec(db, sql);
	if (!ret && mysql_affected_rows(db->mysql) != 1)
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"Vex sweep tombstone changed while recording blob deletion");

out:
	free(sql);
	free(object);
	free(repo);
	return ret;
}

/*
 * Append a receive batch without materializing and rewriting the repository's
 * complete state snapshot for every object.  The normal state adapter still
 * owns ref publication; this is intentionally limited to durable quarantine
 * object/claim metadata after the matching S3 staging batch has succeeded.
 */
int vex_meta_mysql_stage_objects(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct vex_meta_mysql_staged_object *objects, size_t objects_nr)
{
	char *repo = NULL;
	struct strbuf sql = STRBUF_INIT;
	unsigned char (*normalized)[VEX_META_MYSQL_OID_BYTES] = NULL;
	unsigned char *matched = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	uint64_t checked_generation;
	size_t i;
	int found;
	int in_transaction = 0;
	int ret = VEX_META_MYSQL_ERR;

	if (!db || !db->mysql || db->state_transaction || !generation ||
	    (!objects && objects_nr))
		return VEX_META_MYSQL_ERR_INVALID;
	if (!objects_nr)
		return VEX_META_MYSQL_OK;
	/* Keep the SQL values well below a normal MySQL packet and lock window. */
	if (objects_nr > 512)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "Vex receive metadata batch exceeds 512 objects");
	CALLOC_ARRAY(normalized, objects_nr);
	CALLOC_ARRAY(matched, objects_nr);
	for (i = 0; i < objects_nr; i++) {
		size_t j;

		if (objects[i].object_type < 1 || objects[i].object_type > 4 ||
		    !objects[i].blob_key || strlen(objects[i].blob_key) > 512 ||
		    normalize_oid(objects[i].oid, objects[i].oid_len, normalized[i])) {
			ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
					"invalid Vex receive object metadata");
			goto out;
		}
		for (j = 0; j < i; j++)
			if (!memcmp(normalized[i], normalized[j],
				    VEX_META_MYSQL_OID_BYTES)) {
				ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
						"duplicate object in Vex receive metadata batch");
				goto out;
			}
	}
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID");
		goto out;
	}
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	/* Serialize with snapshot saves so an append cannot be rewritten away. */
	if ((ret = require_repository_locked(db, repo)))
		goto out;
	strbuf_addf(&sql, "SELECT generation FROM generations WHERE repo_id = %s "
			    "AND generation = %" PRIu64 " AND state = 'open' FOR UPDATE",
			    repo, generation);
	ret = query_one_u64(db, sql.buf, &checked_generation, &found);
	strbuf_reset(&sql);
	if (ret)
		goto out;
	if (!found || checked_generation != generation) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"cannot stage metadata in a non-open Vex generation");
		goto out;
	}

	/* blob_key is a location; this SQL deliberately has no Git byte payload. */
	strbuf_addstr(&sql, "INSERT IGNORE INTO objects "
		       "(repo_id, oid, obj_type, size, blob_key, generation, state) VALUES ");
	for (i = 0; i < objects_nr; i++) {
		char *object = sql_hex_literal(normalized[i], sizeof(normalized[i]));
		char *key = sql_quote(db, objects[i].blob_key);

		if (!object || !key) {
			free(key);
			free(object);
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not encode Vex receive object batch");
			goto out;
		}
		strbuf_addf(&sql, "%s(%s, %s, %u, %" PRIu64 ", %s, %" PRIu64 ", "
				    "'quarantine')", i ? "," : "", repo, object,
				    objects[i].object_type, objects[i].size, key, generation);
		free(key);
		free(object);
	}
	ret = mysql_exec(db, sql.buf);
	strbuf_reset(&sql);
	if (ret)
		goto out;

	/*
	 * INSERT IGNORE is concurrency-friendly; this locked read turns a hidden
	 * duplicate-key conflict into an explicit type/size/key integrity check.
	 */
	strbuf_addf(&sql, "SELECT oid, obj_type, size, blob_key FROM objects "
			    "WHERE repo_id = %s AND oid IN (", repo);
	for (i = 0; i < objects_nr; i++) {
		char *object = sql_hex_literal(normalized[i], sizeof(normalized[i]));

		if (!object) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not encode Vex receive object lookup");
			goto out;
		}
		strbuf_addf(&sql, "%s%s", i ? "," : "", object);
		free(object);
	}
	strbuf_addstr(&sql, ") FOR UPDATE");
	if (mysql_query(db->mysql, sql.buf)) {
		ret = set_mysql_error(db, "MySQL receive object verification failed");
		goto out;
	}
	strbuf_reset(&sql);
	res = mysql_store_result(db->mysql);
	if (!res) {
		ret = set_mysql_error(db, "MySQL receive object verification result failed");
		goto out;
	}
	if (mysql_num_fields(res) != 4) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid Vex receive object columns");
		goto out;
	}
	while ((row = mysql_fetch_row(res))) {
		unsigned long *lengths = mysql_fetch_lengths(res);
		uint64_t object_type;
		uint64_t size;
		size_t at = objects_nr;

		if (!row[0] || !row[1] || !row[2] || !row[3] || !lengths ||
		    lengths[0] != VEX_META_MYSQL_OID_BYTES ||
		    parse_u64(row[1], &object_type) || parse_u64(row[2], &size)) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex receive object metadata");
			goto out;
		}
		for (i = 0; i < objects_nr; i++)
			if (!memcmp(row[0], normalized[i], VEX_META_MYSQL_OID_BYTES)) {
				at = i;
				break;
			}
		if (at == objects_nr || matched[at] ||
		    object_type != objects[at].object_type ||
		    size != objects[at].size ||
		    lengths[3] != strlen(objects[at].blob_key) ||
		    memcmp(row[3], objects[at].blob_key, lengths[3])) {
			ret = set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
					"Vex receive object conflicts with existing metadata");
			goto out;
		}
		matched[at] = 1;
	}
	mysql_free_result(res);
	res = NULL;
	for (i = 0; i < objects_nr; i++)
		if (!matched[i]) {
			ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
					"Vex receive object disappeared during staging");
			goto out;
		}

	strbuf_addstr(&sql, "INSERT IGNORE INTO generation_objects "
		       "(repo_id, generation, oid) VALUES ");
	for (i = 0; i < objects_nr; i++) {
		char *object = sql_hex_literal(normalized[i], sizeof(normalized[i]));

		if (!object) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not encode Vex receive generation claim");
			goto out;
		}
		strbuf_addf(&sql, "%s(%s, %" PRIu64 ", %s)",
				    i ? "," : "", repo, generation, object);
		free(object);
	}
	ret = mysql_exec(db, sql.buf);
	strbuf_reset(&sql);
	if (ret)
		goto out;
	ret = vex_mysql_commit(db);
	if (ret)
		goto out;
	in_transaction = 0;

out:
	if (res)
		mysql_free_result(res);
	if (in_transaction)
		vex_mysql_rollback(db);
	free(matched);
	free(normalized);
	strbuf_release(&sql);
	free(repo);
	return ret;
}

int vex_meta_mysql_stage_object(struct vex_meta_mysql *db,
			 const char *repo_id, uint64_t generation,
			 const unsigned char *oid, size_t oid_len,
			 unsigned int object_type, uint64_t size,
			 const char *blob_key)
{
	const struct vex_meta_mysql_staged_object object = {
		.oid = oid,
		.oid_len = oid_len,
		.object_type = object_type,
		.size = size,
		.blob_key = blob_key,
	};

	return vex_meta_mysql_stage_objects(db, repo_id, generation, &object, 1);
}

int vex_meta_mysql_stage_commit_parents(
	struct vex_meta_mysql *db, const char *repo_id,
	const struct vex_meta_mysql_commit_parent *parents, size_t parents_nr)
{
	char *repo = NULL;
	struct strbuf sql = STRBUF_INIT;
	size_t i;
	int in_transaction = 0;
	int ret = VEX_META_MYSQL_ERR;

	if (!db || !db->mysql || db->state_transaction ||
	    (!parents && parents_nr))
		return VEX_META_MYSQL_ERR_INVALID;
	if (!parents_nr)
		return VEX_META_MYSQL_OK;
	if (parents_nr > 4096)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "Vex receive commit-parent batch exceeds 4096 edges");
	for (i = 0; i < parents_nr; i++) {
		unsigned char checked[VEX_META_MYSQL_OID_BYTES];

		if (normalize_oid(parents[i].commit_oid, parents[i].commit_oid_len,
				  checked) ||
		    normalize_oid(parents[i].parent_oid, parents[i].parent_oid_len,
				  checked))
			return set_error(db, VEX_META_MYSQL_ERR_INVALID,
					 "invalid Vex receive commit-parent metadata");
	}
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID");
		goto out;
	}
	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	if ((ret = require_repository_locked(db, repo)))
		goto out;
	strbuf_addstr(&sql, "INSERT IGNORE INTO commit_parents "
		       "(repo_id, commit_oid, parent_pos, parent_oid) VALUES ");
	for (i = 0; i < parents_nr; i++) {
		unsigned char commit[VEX_META_MYSQL_OID_BYTES];
		unsigned char parent[VEX_META_MYSQL_OID_BYTES];
		char *commit_literal;
		char *parent_literal;

		/* Validation above means these normalizations cannot fail. */
		if (normalize_oid(parents[i].commit_oid, parents[i].commit_oid_len,
				  commit) ||
		    normalize_oid(parents[i].parent_oid, parents[i].parent_oid_len,
				  parent)) {
			ret = VEX_META_MYSQL_ERR_INVALID;
			goto out;
		}
		commit_literal = sql_hex_literal(commit, sizeof(commit));
		parent_literal = sql_hex_literal(parent, sizeof(parent));
		if (!commit_literal || !parent_literal) {
			free(parent_literal);
			free(commit_literal);
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not encode Vex receive commit-parent batch");
			goto out;
		}
		strbuf_addf(&sql, "%s(%s, %s, %u, %s)", i ? "," : "", repo,
				    commit_literal, parents[i].parent_pos, parent_literal);
		free(parent_literal);
		free(commit_literal);
	}
	ret = mysql_exec(db, sql.buf);
	if (ret)
		goto out;
	ret = vex_mysql_commit(db);
	if (ret)
		goto out;
	in_transaction = 0;
out:
	if (in_transaction)
		vex_mysql_rollback(db);
	strbuf_release(&sql);
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
	sql = vex_format("INSERT IGNORE INTO commit_parents "
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

static int for_each_oid_query(struct vex_meta_mysql *db, const char *repo_id,
			      const char *query_format,
			      vex_meta_mysql_oid_iter_fn fn, void *cb_data,
			      const char *context)
{
	char *repo = NULL;
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret = VEX_META_MYSQL_OK;

	if (!db || !db->mysql || !query_format || !fn)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID");
		goto out;
	}
	/* Some root sets UNION legacy and raw received-pack membership. */
	sql = vex_format(query_format, repo, repo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex %s query", context);
		goto out;
	}
	if (mysql_query(db->mysql, sql)) {
		ret = set_mysql_error(db, context);
		goto out;
	}
	res = mysql_store_result(db->mysql);
	if (!res) {
		ret = set_mysql_error(db, "MySQL Vex root result retrieval failed");
		goto out;
	}
	if (mysql_num_fields(res) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid Vex %s metadata", context);
		goto out;
	}
	while ((row = mysql_fetch_row(res))) {
		lengths = mysql_fetch_lengths(res);
		if (!row[0] || !lengths ||
		    lengths[0] != VEX_META_MYSQL_OID_BYTES) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex %s object ID", context);
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

int vex_meta_mysql_for_each_active_pack_object(struct vex_meta_mysql *db,
						const char *repo_id,
						vex_meta_mysql_oid_iter_fn fn,
						void *cb_data)
{
	static const char query_format[] =
		"SELECT DISTINCT roots.oid FROM ("
		"SELECT po.oid FROM pack_envelopes AS p INNER JOIN pack_objects AS po "
		"ON po.repo_id = p.repo_id AND po.pack_id = p.pack_id "
		"WHERE p.repo_id = %s AND p.state = 'sealed' "
		"UNION ALL "
		"SELECT rpo.oid FROM pack_envelopes AS p "
		"INNER JOIN received_pack_objects AS rpo "
		"ON rpo.repo_id = p.repo_id AND rpo.pack_id = p.pack_id "
		"WHERE p.repo_id = %s AND p.source_kind = 'receive' "
		"AND p.state IN ('sealed', 'obsolete')"
		") AS roots ORDER BY roots.oid";

	return for_each_oid_query(db, repo_id, query_format, fn, cb_data,
				  "MySQL active-pack root read failed");
}

int vex_meta_mysql_for_each_active_clone_lease_object(
					struct vex_meta_mysql *db, const char *repo_id,
					vex_meta_mysql_oid_iter_fn fn, void *cb_data)
{
	static const char query_format[] =
		"SELECT DISTINCT roots.oid FROM ("
		"SELECT po.oid FROM clone_leases AS l INNER JOIN pack_objects AS po "
		"ON po.repo_id = l.repo_id AND po.pack_id = l.pack_id "
		"WHERE l.repo_id = %s AND l.expires_at > CURRENT_TIMESTAMP(6) "
		"UNION ALL "
		"SELECT rpo.oid FROM clone_leases AS l "
		"INNER JOIN received_pack_objects AS rpo "
		"ON rpo.repo_id = l.repo_id AND rpo.pack_id = l.pack_id "
		"WHERE l.repo_id = %s AND l.expires_at > CURRENT_TIMESTAMP(6)"
		") AS roots ORDER BY roots.oid";

	return for_each_oid_query(db, repo_id, query_format, fn, cb_data,
				  "MySQL active-clone-lease root read failed");
}

int vex_meta_mysql_clone_lease_acquire(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const unsigned char *pack_id, size_t pack_id_len,
				       const unsigned char lease_id[VEX_META_MYSQL_LEASE_ID_BYTES],
				       unsigned int ttl_seconds)
{
	char *repo = NULL;
	char *pack = NULL;
	char *lease = NULL;
	char *sql = NULL;
	uint64_t packs;
	int found;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || db->state_transaction || !lease_id ||
	    !ttl_seconds || ttl_seconds > 86400)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	pack = sql_oid_literal(pack_id, pack_id_len);
	lease = sql_hex_literal(lease_id, VEX_META_MYSQL_LEASE_ID_BYTES);
	if (!repo || !pack || !lease) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex clone lease metadata");
		goto out;
	}
	if ((ret = mysql_begin(db)))
		goto out;
	in_transaction = 1;
	if ((ret = require_repository_locked(db, repo)))
		goto out;
	/* Expiry is the crash fallback; keep the table bounded on normal traffic. */
	sql = vex_format("DELETE FROM clone_leases WHERE repo_id = %s "
			 "AND expires_at <= CURRENT_TIMESTAMP(6)", repo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate expired Vex clone-lease cleanup");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	sql = vex_format("SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = %s "
			 "AND pack_id = %s AND state IN ('sealed', 'obsolete')", repo, pack);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex clone-lease pack check");
		goto out;
	}
	ret = query_one_u64(db, sql, &packs, &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (!found || packs != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOT_FOUND,
				"Vex derived pack is not leaseable");
		goto out;
	}
	sql = vex_format("INSERT INTO clone_leases "
			 "(repo_id, lease_id, pack_id, expires_at) VALUES "
			 "(%s, %s, %s, DATE_ADD(CURRENT_TIMESTAMP(6), "
			 "INTERVAL %u SECOND))", repo, lease, pack, ttl_seconds);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex clone-lease insert");
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
	free(lease);
	free(pack);
	free(repo);
	return ret;
}

int vex_meta_mysql_clone_lease_release(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const unsigned char lease_id[VEX_META_MYSQL_LEASE_ID_BYTES])
{
	char *repo = NULL;
	char *lease = NULL;
	char *sql = NULL;
	int in_transaction = 0;
	int ret;

	if (!db || !db->mysql || db->state_transaction || !lease_id)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	lease = sql_hex_literal(lease_id, VEX_META_MYSQL_LEASE_ID_BYTES);
	if (!repo || !lease) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex clone lease release metadata");
		goto out;
	}
	if ((ret = mysql_begin(db)))
		goto out;
	in_transaction = 1;
	if ((ret = require_repository_locked(db, repo)))
		goto out;
	sql = vex_format("DELETE FROM clone_leases WHERE repo_id = %s "
			 "AND lease_id = %s", repo, lease);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex clone-lease release");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOT_FOUND,
				"Vex clone lease does not exist");
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
	free(lease);
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

int vex_meta_mysql_current_sealed_pack(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const struct git_hash_algo *hash_algo,
				       struct vex_meta_mysql_pack *pack)
{
	char *repo = NULL;
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	struct object_id checked_id;
	char *pack_key = NULL;
	char *idx_key = NULL;
	uint64_t covering_generation;
	int ret = VEX_META_MYSQL_ERR;

	if (!db || !db->mysql || !repo_id || !hash_algo || !pack)
		return VEX_META_MYSQL_ERR_INVALID;
	memset(pack, 0, sizeof(*pack));
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID for pack selection");
		goto out;
	}
	/*
	 * A build can sample a generation before a concurrent push seals a newer
	 * one, then insert its row afterwards.  Selecting by state alone would
	 * make that stale snapshot readable.  Require exact equality with the
	 * newest sealed generation on every selection instead.
	 */
	sql = vex_format("SELECT p.pack_id, p.pack_key, p.idx_key, "
			 "p.covering_generation FROM pack_envelopes AS p "
			 "WHERE p.repo_id = %s AND p.state = 'sealed' "
			 "AND p.is_full_snapshot = TRUE AND p.idx_key IS NOT NULL "
			 "AND p.covering_generation = "
			 "(SELECT MAX(g.generation) FROM generations AS g "
			 "WHERE g.repo_id = %s AND g.state = 'sealed') "
			 "ORDER BY p.sealed_at DESC, p.pack_id LIMIT 1",
			 repo, repo);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate current Vex pack query");
		goto out;
	}
	ret = state_query_result(db, sql, &res);
	if (ret)
		goto out;
	if (mysql_num_fields(res) != 4) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid Vex current-pack metadata");
		goto out;
	}
	row = mysql_fetch_row(res);
	if (!row) {
		ret = VEX_META_MYSQL_ERR_NOT_FOUND;
		goto out;
	}
	lengths = mysql_fetch_lengths(res);
	if (!row[0] || !row[1] || !row[2] || !row[3] || !lengths ||
	    lengths[0] != VEX_META_MYSQL_OID_BYTES || !lengths[1] ||
	    lengths[1] > 512 || !lengths[2] || lengths[2] > 512 ||
	    parse_u64(row[3], &covering_generation) ||
	    state_oid_from_binary(row[0], lengths[0], hash_algo, &checked_id)) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid Vex current-pack metadata");
		goto out;
	}
	pack_key = state_row_text(row[1], lengths[1]);
	idx_key = state_row_text(row[2], lengths[2]);
	if (!pack_key || !idx_key) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned unsafe Vex current-pack keys");
		goto out;
	}
	memcpy(pack->pack_id, row[0], sizeof(pack->pack_id));
	pack->covering_generation = covering_generation;
	pack->pack_key = pack_key;
	pack->idx_key = idx_key;
	pack_key = NULL;
	idx_key = NULL;
	ret = VEX_META_MYSQL_OK;

out:
	free(idx_key);
	free(pack_key);
	if (res)
		mysql_free_result(res);
	free(sql);
	free(repo);
	return ret;
}

void vex_meta_mysql_pack_release(struct vex_meta_mysql_pack *pack)
{
	if (!pack)
		return;
	free(pack->pack_key);
	free(pack->idx_key);
	memset(pack, 0, sizeof(*pack));
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

static int receive_pack_manifest_path_valid(struct vex_meta_mysql *db,
					    const char *manifest_path)
{
	struct stat st;

	/* LOCAL INFILE must never be a general-purpose server-side file reader. */
	if (!manifest_path || manifest_path[0] != '/' ||
	    strpbrk(manifest_path, "\r\n"))
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "Vex receive manifest must be an absolute safe path");
	if (lstat(manifest_path, &st) || !S_ISREG(st.st_mode))
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "Vex receive manifest must be a regular non-symlink file");
	return VEX_META_MYSQL_OK;
}

static int receive_pack_manifest_count(struct vex_meta_mysql *db,
				       uint64_t *members)
{
	int found;
	int ret;

	ret = query_one_u64(db, "SELECT COUNT(*) FROM vex_receive_pack_manifest",
			    members, &found);
	if (ret)
		return ret;
	if (!found)
		return set_error(db, VEX_META_MYSQL_ERR,
				 "MySQL did not return Vex receive manifest count");
	return VEX_META_MYSQL_OK;
}

/*
 * `git init --ref-format=vex` persists HEAD before the first object exists.
 * It is a harmless discovery scaffold, not evidence of an existing Git
 * history.  Keep the raw full-seed boundary narrow: this is the only
 * pre-existing ref we ever admit, its symbolic target must be a valid unborn
 * branch, and it remains untouched while the seed creates real refs below
 * refs/.
 */
static int receive_pack_require_unborn_head_scaffold_locked(
		struct vex_meta_mysql *db, const char *repo)
{
	char *sql = NULL;
	char symref_target[1025];
	uint64_t refs;
	int found;
	int ret;

	sql = vex_format("SELECT COUNT(*) FROM refs WHERE repo_id = %s", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate Vex full-seed ref count");
	ret = query_one_u64(db, sql, &refs, &found);
	free(sql);
	if (ret)
		return ret;
	if (!found)
		return set_error(db, VEX_META_MYSQL_ERR,
				 "MySQL did not return Vex full-seed ref count");

	sql = vex_format("SELECT h.symref_target FROM refs AS h WHERE h.repo_id = %s "
			 "AND h.refname = _binary'HEAD' AND h.is_symbolic = TRUE "
			 "AND h.target_oid IS NULL AND h.peeled_oid IS NULL "
			 "AND h.symref_target IS NOT NULL AND INSTR(h.symref_target, X'00') = 0 "
			 "AND NOT EXISTS (SELECT 1 FROM refs AS target "
			 "WHERE target.repo_id = h.repo_id "
			 "AND target.refname = h.symref_target)", repo);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate Vex unborn HEAD scaffold check");
	ret = query_one_text(db, sql, symref_target, sizeof(symref_target), &found);
	free(sql);
	if (ret)
		return ret;
	if (!found)
		return set_error(db, VEX_META_MYSQL_ERR,
				 "MySQL did not return Vex unborn HEAD scaffold");
	if (refs != 1 || !starts_with(symref_target, "refs/heads/") ||
	    check_refname_format(symref_target, 0))
		return set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
				 "raw receive-pack full snapshot requires only the unborn "
				 "HEAD -> refs/heads/* scaffold");
	return VEX_META_MYSQL_OK;
}

/*
 * Import the metadata-only object map for a first full mirror.  The local
 * pack/idx pair has already been validated and staged in S3; this method
 * receives only a trusted TSV manifest and never reads Git payload bytes.
 */
int vex_meta_mysql_receive_pack_stage_manifest(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct git_hash_algo *hash_algo,
	const struct vex_meta_mysql_receive_pack *pack,
	const char *manifest_path, uint64_t expected_members)
{
	static const char create_manifest_table_sql[] =
		"CREATE TEMPORARY TABLE vex_receive_pack_manifest ("
		"oid_hex VARBINARY(64) NOT NULL, "
		"obj_type_text VARBINARY(8) NOT NULL, "
		"size_text VARBINARY(32) NOT NULL, "
		"PRIMARY KEY (oid_hex)"
		") ENGINE=InnoDB";
	static const char *const seed_tables[] = {
		"objects", "pack_envelopes",
	};
	char *repo = NULL;
	char *pack_id = NULL;
	char *pack_key = NULL;
	char *idx_key = NULL;
	char *manifest = NULL;
	char *sql = NULL;
	unsigned char normalized_pack[VEX_META_MYSQL_OID_BYTES];
	uint64_t members;
	uint64_t invalid_rows;
	uint64_t count;
	int found;
	int in_transaction = 0;
	int temp_table = 0;
	int ret = VEX_META_MYSQL_ERR;
	size_t table_i;

	if (!db || !db->mysql || db->state_transaction || !generation ||
	    !hash_algo || (hash_algo->rawsz != 20 && hash_algo->rawsz != 32) ||
	    !pack || !pack->full_snapshot || !expected_members ||
	    !pack->pack_key || !*pack->pack_key || !pack->idx_key || !*pack->idx_key ||
	    strlen(pack->pack_key) > 512 || strlen(pack->idx_key) > 512 ||
	    normalize_oid(pack->pack_id, pack->pack_id_len, normalized_pack))
		return VEX_META_MYSQL_ERR_INVALID;
	if ((ret = receive_pack_manifest_path_valid(db, manifest_path)))
		return ret;

	repo = sql_repo_literal(repo_id, NULL);
	pack_id = sql_hex_literal(normalized_pack, sizeof(normalized_pack));
	pack_key = sql_quote(db, pack->pack_key);
	idx_key = sql_quote(db, pack->idx_key);
	manifest = sql_quote(db, manifest_path);
	if (!repo || !pack_id || !pack_key || !idx_key || !manifest) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not encode Vex receive-pack metadata");
		goto out;
	}

	/* A connection-local table keeps LOAD DATA outside the repository lock. */
	ret = mysql_exec(db, "DROP TEMPORARY TABLE IF EXISTS vex_receive_pack_manifest");
	if (ret)
		goto out;
	ret = mysql_exec(db, create_manifest_table_sql);
	if (ret)
		goto out;
	temp_table = 1;
	sql = vex_format("LOAD DATA LOCAL INFILE %s INTO TABLE vex_receive_pack_manifest "
			 "CHARACTER SET ascii FIELDS TERMINATED BY '\\t' ESCAPED BY '' "
			 "LINES TERMINATED BY '\\n' (oid_hex, obj_type_text, size_text)",
			 manifest);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex receive manifest bulk-load query");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL LOCAL INFILE receive manifest load failed: %s",
				mysql_error(db->mysql));
		goto out;
	}

	/* Reject loose parsing: accepted rows are exact canonical Git metadata. */
	sql = vex_format("SELECT COUNT(*) FROM vex_receive_pack_manifest WHERE "
			 "LENGTH(oid_hex) <> %u OR NOT REGEXP_LIKE("
			 "CONVERT(oid_hex USING utf8mb4), '^[0-9a-f]+$', 'c') OR "
			 "NOT REGEXP_LIKE(CONVERT(obj_type_text USING utf8mb4), "
			 "'^[1-4]$', 'c') OR NOT REGEXP_LIKE("
			 "CONVERT(size_text USING utf8mb4), '^[0-9]+$', 'c') OR "
			 "LENGTH(size_text) > 20 OR "
			 "(LENGTH(size_text) = 20 AND size_text > '18446744073709551615')",
			 hash_algo->hexsz);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex receive manifest validation query");
		goto out;
	}
	ret = query_one_u64(db, sql, &invalid_rows, &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (!found || invalid_rows) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"Vex receive manifest has invalid object metadata");
		goto out;
	}
	if ((ret = receive_pack_manifest_count(db, &members)))
		goto out;
	if (members != expected_members) {
		ret = set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
				"Vex receive manifest count does not match indexed pack");
		goto out;
	}

	ret = mysql_begin(db);
	if (ret)
		goto out;
	in_transaction = 1;
	if ((ret = require_repository_locked(db, repo)) ||
	    (ret = state_verify_repository(db, repo, hash_algo)))
		goto out;

	/* A committed retry is okay only for the identical still-building pair. */
	sql = vex_format("SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = %s "
			 "AND pack_id = %s", repo, pack_id);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex receive-pack duplicate query");
		goto out;
	}
	ret = query_one_u64(db, sql, &count, &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (!found) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL did not return Vex receive-pack duplicate count");
		goto out;
	}
	if (count) {
		sql = vex_format("SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = %s "
			 "AND pack_id = %s AND state = 'building' AND source_kind = 'receive' "
			 "AND receive_generation = %" PRIu64 " AND is_full_snapshot = TRUE "
			 "AND pack_key = %s AND idx_key = %s", repo, pack_id, generation,
			 pack_key, idx_key);
		if (!sql) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not allocate Vex receive-pack retry query");
			goto out;
		}
		ret = query_one_u64(db, sql, &count, &found);
		free(sql);
		sql = NULL;
		if (ret)
			goto out;
		if (!found || count != 1) {
			ret = set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
					"Vex receive pack identifier already has incompatible metadata");
			goto out;
		}
		sql = vex_format("SELECT COUNT(*) FROM received_pack_objects WHERE "
			 "repo_id = %s AND pack_id = %s", repo, pack_id);
		if (!sql) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not allocate Vex receive-pack retry member query");
			goto out;
		}
		ret = query_one_u64(db, sql, &count, &found);
		free(sql);
		sql = NULL;
		if (ret)
			goto out;
		if (!found || count != expected_members) {
			ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
					"Vex receive pack retry has incomplete member metadata");
			goto out;
		}
		ret = vex_mysql_commit(db);
		if (!ret)
			in_transaction = 0;
		goto out;
	}

	/* First-seed gate: only init's unborn HEAD may pre-exist. */
	sql = vex_format("SELECT COUNT(*) FROM generations WHERE repo_id = %s "
			 "AND generation = %" PRIu64 " AND state = 'open'", repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex receive generation check");
		goto out;
	}
	ret = query_one_u64(db, sql, &count, &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (!found || count != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"cannot stage a raw pack in a non-open Vex generation");
		goto out;
	}
	if ((ret = receive_pack_require_unborn_head_scaffold_locked(db, repo)))
		goto out;
	for (table_i = 0; table_i < ARRAY_SIZE(seed_tables); table_i++) {
		sql = vex_format("SELECT COUNT(*) FROM %s WHERE repo_id = %s", seed_tables[table_i],
				 repo);
		if (!sql) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not allocate Vex full-seed emptiness check");
			goto out;
		}
		ret = query_one_u64(db, sql, &count, &found);
		free(sql);
		sql = NULL;
		if (ret)
			goto out;
		if (!found || count) {
			ret = set_error(db, VEX_META_MYSQL_ERR_CONFLICT,
					"raw receive-pack full snapshot requires an empty Vex repository");
			goto out;
		}
	}

	sql = vex_format("INSERT INTO pack_envelopes "
			 "(repo_id, pack_id, pack_key, idx_key, bitmap_key, covering_generation, "
			 "state, source_kind, receive_generation, is_full_snapshot) VALUES "
			 "(%s, %s, %s, %s, NULL, %" PRIu64 ", 'building', 'receive', "
			 "%" PRIu64 ", TRUE)", repo, pack_id, pack_key, idx_key,
			 generation, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex receive-pack envelope insert");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;

	sql = vex_format("INSERT INTO received_pack_objects "
			 "(repo_id, pack_id, oid, obj_type, size) SELECT %s, %s, %s, "
			 "CAST(obj_type_text AS UNSIGNED), CAST(size_text AS UNSIGNED) "
			 "FROM vex_receive_pack_manifest", repo, pack_id,
			 hash_algo->rawsz == 20 ?
			 "CONCAT(UNHEX(oid_hex), REPEAT(X'00', 12))" : "UNHEX(oid_hex)");
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex receive-pack member bulk insert");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != expected_members) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"Vex receive pack inserted an unexpected number of members");
		goto out;
	}
	ret = vex_mysql_commit(db);
	if (!ret)
		in_transaction = 0;

out:
	free(sql);
	if (in_transaction)
		vex_mysql_rollback(db);
	if (temp_table)
		(void)mysql_exec(db, "DROP TEMPORARY TABLE IF EXISTS vex_receive_pack_manifest");
	free(manifest);
	free(idx_key);
	free(pack_key);
	free(pack_id);
	free(repo);
	return ret;
}

int vex_meta_mysql_receive_pack_object_get_visible(
	struct vex_meta_mysql *db, const char *repo_id,
	const unsigned char *oid, size_t oid_len, uint64_t visible_generation,
	struct vex_meta_mysql_receive_pack_object *object)
{
	char *repo = NULL;
	char *object_id = NULL;
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	uint64_t object_type;
	uint64_t size;
	uint64_t generation;
	char *pack_key = NULL;
	char *idx_key = NULL;
	int ret = VEX_META_MYSQL_ERR;

	if (!db || !db->mysql || !object)
		return VEX_META_MYSQL_ERR_INVALID;
	memset(object, 0, sizeof(*object));
	repo = sql_repo_literal(repo_id, NULL);
	object_id = sql_oid_literal(oid, oid_len);
	if (!repo || !object_id) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex received-pack object lookup");
		goto out;
	}
	sql = vex_format("SELECT rpo.obj_type, rpo.size, p.pack_id, p.pack_key, "
			 "p.idx_key, p.receive_generation FROM received_pack_objects AS rpo "
			 "INNER JOIN pack_envelopes AS p ON p.repo_id = rpo.repo_id "
			 "AND p.pack_id = rpo.pack_id WHERE rpo.repo_id = %s "
			 "AND rpo.oid = %s AND (p.state IN ('sealed', 'obsolete') OR "
			 "(p.state = 'building' AND p.receive_generation = %" PRIu64 ")) "
			 "ORDER BY p.state = 'sealed' DESC, p.sealed_at DESC, p.pack_id LIMIT 1",
			 repo, object_id, visible_generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex received-pack object lookup");
		goto out;
	}
	ret = state_query_result(db, sql, &res);
	if (ret)
		goto out;
	if (mysql_num_fields(res) != 6) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid Vex received-pack object metadata");
		goto out;
	}
	row = mysql_fetch_row(res);
	if (!row) {
		ret = VEX_META_MYSQL_ERR_NOT_FOUND;
		goto out;
	}
	lengths = mysql_fetch_lengths(res);
	if (!row[0] || !row[1] || !row[2] || !row[3] || !row[4] || !row[5] ||
	    !lengths || lengths[2] != VEX_META_MYSQL_OID_BYTES ||
	    !lengths[3] || lengths[3] > 512 || !lengths[4] || lengths[4] > 512 ||
	    parse_u64(row[0], &object_type) || object_type < 1 || object_type > 4 ||
	    parse_u64(row[1], &size) || parse_u64(row[5], &generation) || !generation) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned malformed Vex received-pack object metadata");
		goto out;
	}
	pack_key = state_row_text(row[3], lengths[3]);
	idx_key = state_row_text(row[4], lengths[4]);
	if (!pack_key || !idx_key) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned unsafe Vex received-pack keys");
		goto out;
	}
	object->object_type = (unsigned int)object_type;
	object->size = size;
	object->generation = generation;
	memcpy(object->pack_id, row[2], sizeof(object->pack_id));
	object->pack_key = pack_key;
	object->idx_key = idx_key;
	pack_key = NULL;
	idx_key = NULL;
	ret = VEX_META_MYSQL_OK;

out:
	free(idx_key);
	free(pack_key);
	if (res)
		mysql_free_result(res);
	free(sql);
	free(object_id);
	free(repo);
	return ret;
}

void vex_meta_mysql_receive_pack_object_release(
	struct vex_meta_mysql_receive_pack_object *object)
{
	if (!object)
		return;
	free(object->pack_key);
	free(object->idx_key);
	memset(object, 0, sizeof(*object));
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

int vex_meta_mysql_state_commit_locked(struct vex_meta_mysql *db)
{
	int ret;

	if (!db || !db->mysql || !db->state_transaction)
		return VEX_META_MYSQL_ERR_STATE;
	ret = vex_mysql_commit(db);
	if (!ret)
		db->state_transaction = 0;
	return ret;
}

static int state_load_epochs(struct vex_meta_mysql *db, const char *repo,
			     struct vex_meta_state *state)
{
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret;

	sql = vex_format("SELECT generation, state, push_id, "
			 "DATE_FORMAT(created_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f'), "
			 "DATE_FORMAT(receive_heartbeat_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f'), "
			 "DATE_FORMAT(sealed_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f') "
			 "FROM generations "
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
		if (!row[0] || !row[1] || !row[3] || !lengths ||
		    parse_u64(row[0], &id) || !id ||
		    parse_epoch_state(row[1], &epoch_state) ||
		    (row[2] && lengths[2] != 16) || !lengths[3] ||
		    lengths[3] > 32 || (row[4] && lengths[4] > 32) ||
		    (row[5] && lengths[5] > 32) ||
		    id == UINT64_MAX) {
			ret = set_error(db, VEX_META_MYSQL_ERR,
					"MySQL returned invalid Vex generation metadata");
			goto out;
		}
		ALLOC_GROW(state->epochs, state->epochs_nr + 1, state->epochs_alloc);
		epoch = &state->epochs[state->epochs_nr++];
		epoch->id = id;
		epoch->state = (enum vex_epoch_state)epoch_state;
		epoch->push_id = state_row_uuid(row[2], row[2] ? lengths[2] : 0);
		epoch->created_at = state_row_text(row[3], lengths[3]);
		epoch->receive_heartbeat_at = row[4] ?
			state_row_text(row[4], lengths[4]) : NULL;
		epoch->sealed_at = row[5] ? state_row_text(row[5], lengths[5]) : NULL;
		if ((row[2] && !epoch->push_id) || !epoch->created_at) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not allocate Vex generation push id");
			goto out;
		}
		if (row[4] && !epoch->receive_heartbeat_at) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not allocate Vex generation heartbeat timestamp");
			goto out;
		}
		if (row[5] && !epoch->sealed_at) {
			ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					"could not allocate Vex generation seal timestamp");
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
 * immutable pack rows and membership through that replacement rather than
 * making packs block subsequent receives.  A sealed pack becomes obsolete if
 * a later sealed generation makes its coverage stale; a future deletion path
 * must leave it obsolete before deleting an object it references.
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

	/* Expired pins are not sweep roots and must not recreate a dangling FK. */
	sql = vex_format("SELECT oid, reason, "
			 "DATE_FORMAT(created_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f'), "
			 "DATE_FORMAT(expires_at, '%%Y-%%m-%%d %%H:%%i:%%s.%%f') "
			 "FROM object_pins WHERE repo_id = %s "
			 "AND (expires_at IS NULL OR expires_at > CURRENT_TIMESTAMP(6)) "
			 "ORDER BY oid, reason", repo);
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

static uint64_t state_latest_sealed_generation(const struct vex_meta_state *state)
{
	uint64_t generation = 0;
	size_t i;

	for (i = 0; i < state->epochs_nr; i++) {
		const struct vex_meta_epoch *epoch = &state->epochs[i];

		if (epoch->state == VEX_EPOCH_SEALED && epoch->id > generation)
			generation = epoch->id;
	}
	return generation;
}

static int state_has_saved_object(const struct vex_meta_state *state,
				  const unsigned char oid[VEX_META_MYSQL_OID_BYTES],
				  const struct git_hash_algo *hash_algo)
{
	size_t i;

	for (i = 0; i < state->objects_nr; i++)
		if (!memcmp(state->objects[i].oid.hash, oid, hash_algo->rawsz))
			return 1;
	return 0;
}

static int state_saved_pack_is_incomplete(
		const struct vex_meta_mysql_saved_packs *packs,
		const struct vex_meta_mysql_saved_pack *pack,
		const struct vex_meta_state *state,
		const struct git_hash_algo *hash_algo)
{
	size_t i;

	for (i = 0; i < packs->objects_nr; i++) {
		const struct vex_meta_mysql_saved_pack_object *object =
			&packs->objects[i];

		if (memcmp(object->pack_id, pack->pack_id, sizeof(pack->pack_id)))
			continue;
		if (!state_has_saved_object(state, object->oid, hash_algo))
			return 1;
	}
	return 0;
}

static unsigned int pack_rebuild_debounce_seconds(void)
{
	const char *value = getenv("GIT_VEX_PACK_REBUILD_DELAY_SECONDS");
	char *end;
	unsigned long seconds;

	if (!value || !*value)
		return VEX_META_MYSQL_PACK_REBUILD_DEFAULT_DELAY_SECONDS;
	errno = 0;
	seconds = strtoul(value, &end, 10);
	if (errno || *end || !seconds ||
	    seconds > VEX_META_MYSQL_PACK_REBUILD_MAX_DELAY_SECONDS)
		return VEX_META_MYSQL_PACK_REBUILD_DEFAULT_DELAY_SECONDS;
	return (unsigned int)seconds;
}

static int state_has_current_complete_pack(
		const struct vex_meta_mysql_saved_packs *packs,
		uint64_t sealed_generation,
		const struct vex_meta_state *state_snapshot,
		const struct git_hash_algo *hash_algo)
{
	size_t i;

	for (i = 0; i < packs->nr; i++) {
		const struct vex_meta_mysql_saved_pack *pack = &packs->items[i];

		if (strcmp(pack->state, "sealed") ||
		    pack->covering_generation != sealed_generation ||
		    !pack->idx_key || !pack->idx_key_len)
			continue;
		if (!state_saved_pack_is_incomplete(packs, pack, state_snapshot,
						   hash_algo))
			return 1;
	}
	return 0;
}

/*
 * A receive never runs pack-objects while its repository state lock is held.
 * Instead it records a single target generation.  Later receives coalesce
 * into the same request but retain the first not_before value, giving a busy
 * repository a bounded batching window rather than indefinitely postponing
 * its builder.
 */
static int state_update_pack_rebuild_request(
		struct vex_meta_mysql *db, const char *repo,
		uint64_t sealed_generation, int has_current_pack)
{
	char *sql;
	int ret;

	if (!sealed_generation)
		return VEX_META_MYSQL_OK;
	if (has_current_pack) {
		sql = vex_format("DELETE FROM pack_rebuild_requests "
				 "WHERE repo_id = %s AND target_generation <= %" PRIu64,
				 repo, sealed_generation);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex pack rebuild clear query");
		ret = mysql_exec(db, sql);
		free(sql);
		return ret;
	} else {
		unsigned int delay = pack_rebuild_debounce_seconds();

		sql = vex_format("INSERT INTO pack_rebuild_requests "
				 "(repo_id, target_generation, requested_at, "
				 "last_requested_at, not_before) VALUES "
				 "(%s, %" PRIu64 ", CURRENT_TIMESTAMP(6), "
				 "CURRENT_TIMESTAMP(6), DATE_ADD(CURRENT_TIMESTAMP(6), "
				 "INTERVAL %u SECOND)) ON DUPLICATE KEY UPDATE "
				 "target_generation = GREATEST(target_generation, %" PRIu64 "), "
				 "last_requested_at = CURRENT_TIMESTAMP(6), "
				 "not_before = LEAST(not_before, "
				 "DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL %u SECOND))",
				 repo, sealed_generation, delay, sealed_generation, delay);
		if (!sql)
			return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
					 "could not allocate Vex pack rebuild queue query");
		ret = mysql_exec(db, sql);
		free(sql);
		if (!ret)
			trace_printf("vex: queued derived-pack rebuild target=%" PRIu64
				     " debounce=%us", sealed_generation, delay);
		return ret;
	}
}

static int state_insert_packs(struct vex_meta_mysql *db, const char *repo,
				      const struct vex_meta_mysql_saved_packs *packs,
				      uint64_t sealed_generation,
				      const struct vex_meta_state *state_snapshot,
				      const struct git_hash_algo *hash_algo)
{
	size_t i;

	for (i = 0; i < packs->nr; i++) {
		const struct vex_meta_mysql_saved_pack *pack = &packs->items[i];
		/* Never restore a stale sealed artifact as selectable/current. */
		int obsolete = !strcmp(pack->state, "sealed") &&
			pack->covering_generation < sealed_generation;
		int incomplete = state_saved_pack_is_incomplete(packs, pack,
							state_snapshot, hash_algo);
		const char *target_state;
		char *pack_id;
		char *pack_key;
		char *idx_key;
		char *bitmap_key;
		char *state;
		char *created_at;
		char *sealed_at;
		char *obsolete_at;
		char *sql;
		int ret;

		/*
		 * Every sealed pack is a sweep root.  Losing any of its members is
		 * therefore a state corruption, not an excuse to revive a pack whose
		 * immutable bytes can no longer satisfy its advertised membership.
		 */
		if (incomplete && !strcmp(pack->state, "sealed"))
			return set_error(db, VEX_META_MYSQL_ERR_STATE,
					 "a sealed Vex pack would lose a swept object");
		target_state = incomplete ? "deleted" :
			(obsolete ? "obsolete" : pack->state);
		pack_id = sql_hex_literal(pack->pack_id, sizeof(pack->pack_id));
		pack_key = pack->pack_key ?
			sql_hex_literal(pack->pack_key, pack->pack_key_len) : NULL;
		idx_key = pack->idx_key ?
			sql_hex_literal(pack->idx_key, pack->idx_key_len) : vex_strdup("NULL");
		bitmap_key = pack->bitmap_key ?
			sql_hex_literal(pack->bitmap_key, pack->bitmap_key_len) : vex_strdup("NULL");
		state = sql_quote(db, target_state);
		created_at = sql_quote(db, pack->created_at);
		sealed_at = pack->sealed_at ? sql_quote(db, pack->sealed_at) :
			vex_strdup("NULL");
		obsolete_at = incomplete || obsolete ?
			vex_strdup("CURRENT_TIMESTAMP(6)") :
			(pack->obsolete_at ? sql_quote(db, pack->obsolete_at) :
			 vex_strdup("NULL"));

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

		/* The enclosing obsolete/deleted pack can retain no dangling FK. */
		if (!state_has_saved_object(state_snapshot, object->oid, hash_algo))
			continue;

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
		char *created_at = epoch->created_at ?
			sql_quote(db, epoch->created_at) :
			vex_strdup("CURRENT_TIMESTAMP(6)");
		char *heartbeat_at = epoch->receive_heartbeat_at ?
			sql_quote(db, epoch->receive_heartbeat_at) : vex_strdup("NULL");
		char *sealed_at = epoch->state == VEX_EPOCH_OPEN ?
			vex_strdup("NULL") :
			(epoch->sealed_at ? sql_quote(db, epoch->sealed_at) :
			 vex_strdup("CURRENT_TIMESTAMP(6)"));
		char *sql;
		int ret;

		if (!epoch->id || !name || !push || !created_at || !heartbeat_at ||
		    !sealed_at) {
			free(sealed_at);
			free(heartbeat_at);
			free(created_at);
			free(push);
			return set_error(db, VEX_META_MYSQL_ERR_INVALID,
					 "invalid Vex epoch state");
		}
		sql = vex_format("INSERT INTO generations "
				 "(repo_id, generation, state, push_id, created_at, "
				 "receive_heartbeat_at, sealed_at) VALUES "
				 "(%s, %" PRIu64 ", '%s', %s, %s, %s, %s)",
				 repo, epoch->id, name, push,
				 created_at, heartbeat_at, sealed_at);
		free(sealed_at);
		free(heartbeat_at);
		free(created_at);
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

static int receive_pack_find_building_sql_locked(
		struct vex_meta_mysql *db, const char *repo, uint64_t generation,
		unsigned char pack_id[VEX_META_MYSQL_OID_BYTES])
{
	char *sql = NULL;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	unsigned long *lengths;
	int ret = VEX_META_MYSQL_ERR;

	memset(pack_id, 0, VEX_META_MYSQL_OID_BYTES);
	sql = vex_format("SELECT pack_id FROM pack_envelopes WHERE repo_id = %s "
			 "AND source_kind = 'receive' AND state = 'building' "
			 "AND receive_generation = %" PRIu64 " "
			 "AND is_full_snapshot = TRUE FOR UPDATE", repo, generation);
	if (!sql)
		return set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				 "could not allocate Vex building receive-pack query");
	ret = state_query_result(db, sql, &res);
	if (ret)
		goto out;
	if (mysql_num_fields(res) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned invalid building receive-pack metadata");
		goto out;
	}
	row = mysql_fetch_row(res);
	if (!row) {
		ret = VEX_META_MYSQL_ERR_NOT_FOUND;
		goto out;
	}
	lengths = mysql_fetch_lengths(res);
	if (!lengths || lengths[0] != VEX_META_MYSQL_OID_BYTES) {
		ret = set_error(db, VEX_META_MYSQL_ERR,
				"MySQL returned malformed building receive-pack identifier");
		goto out;
	}
	memcpy(pack_id, row[0], VEX_META_MYSQL_OID_BYTES);
	if (mysql_fetch_row(res)) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"Vex generation has more than one building receive pack");
		goto out;
	}
	ret = VEX_META_MYSQL_OK;

out:
	if (res)
		mysql_free_result(res);
	free(sql);
	return ret;
}

int vex_meta_mysql_receive_pack_get_building_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	unsigned char out_pack_id[VEX_META_MYSQL_OID_BYTES])
{
	char *repo = NULL;
	int ret;

	if (out_pack_id)
		memset(out_pack_id, 0, VEX_META_MYSQL_OID_BYTES);
	if (!db || !db->mysql || !db->state_transaction || !generation ||
	    !out_pack_id)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "invalid Vex repository UUID for raw receive-pack lookup");
	ret = receive_pack_find_building_sql_locked(db, repo, generation,
					    out_pack_id);
	free(repo);
	return ret;
}

static int receive_pack_require_member_locked(struct vex_meta_mysql *db,
					       const char *repo,
					       const unsigned char pack_id[VEX_META_MYSQL_OID_BYTES],
					       const struct object_id *oid,
					       const struct git_hash_algo *hash_algo)
{
	char *pack = NULL;
	char *object = NULL;
	char *sql = NULL;
	uint64_t members;
	int found;
	int ret;

	pack = sql_hex_literal(pack_id, VEX_META_MYSQL_OID_BYTES);
	object = state_oid_literal(oid, hash_algo);
	if (!pack || !object) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex raw receive-pack ref target");
		goto out;
	}
	sql = vex_format("SELECT COUNT(*) FROM received_pack_objects WHERE "
			 "repo_id = %s AND pack_id = %s AND oid = %s", repo, pack, object);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex raw receive-pack membership query");
		goto out;
	}
	ret = query_one_u64(db, sql, &members, &found);
	if (ret)
		goto out;
	if (!found || members != 1)
		ret = set_error(db, VEX_META_MYSQL_ERR_NOT_FOUND,
				"Vex ref target is absent from the building receive pack");

out:
	free(sql);
	free(object);
	free(pack);
	return ret;
}

static int receive_pack_seed_update_valid(struct vex_meta_mysql *db,
					  const struct vex_ref_update *update)
{
	if (!update || !update->refname || !*update->refname ||
	    strlen(update->refname) > 1024 ||
	    !strcmp(update->refname, "HEAD") ||
	    update->old_target.type != VEX_REF_TARGET_NONE ||
	    update->expected_version || update->new_target.type != VEX_REF_TARGET_OID ||
	    (update->actor && strlen(update->actor) > 255) ||
	    (update->reason && strlen(update->reason) > 32) ||
	    (update->message && strlen(update->message) > 1024) ||
	    (update->is_fast_forward != -1 && update->is_fast_forward != 0 &&
	     update->is_fast_forward != 1))
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "raw receive-pack seed supports only new OID refs and preserves HEAD");
	return VEX_META_MYSQL_OK;
}

static int receive_pack_insert_ref_locked(
		struct vex_meta_mysql *db, const char *repo,
		const struct vex_ref_update *update,
		const struct git_hash_algo *hash_algo)
{
	char *refname = NULL;
	char *object = NULL;
	char *actor = NULL;
	char *push_id = NULL;
	char *reason = NULL;
	char *message = NULL;
	char *sql = NULL;
	const char *is_ff;
	int ret = VEX_META_MYSQL_ERR;

	if ((ret = receive_pack_seed_update_valid(db, update)))
		return ret;
	refname = sql_quote(db, update->refname);
	object = state_oid_literal(&update->new_target.oid, hash_algo);
	actor = sql_quote(db, update->actor);
	push_id = state_uuid_literal_or_null(update->push_id);
	reason = sql_quote(db, update->reason ? update->reason : "receive seed");
	message = sql_quote(db, update->message);
	if (!refname || !object || !actor || !push_id || !reason || !message) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not encode Vex raw receive-pack ref metadata");
		goto out;
	}
	sql = vex_format("INSERT INTO refs "
			 "(repo_id, refname, is_symbolic, target_oid, symref_target, "
			 "peeled_oid, version) VALUES (%s, %s, FALSE, %s, NULL, NULL, 1)",
			 repo, refname, object);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex raw receive-pack ref insert");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	is_ff = update->is_fast_forward < 0 ? "NULL" :
		(update->is_fast_forward ? "TRUE" : "FALSE");
	sql = vex_format("INSERT INTO ref_updates "
			 "(repo_id, refname, old_oid, new_oid, old_target, new_target, "
			 "is_ff, actor, push_id, reason, message) VALUES "
			 "(%s, %s, NULL, %s, NULL, NULL, %s, %s, %s, %s, %s)",
			 repo, refname, object, is_ff, actor, push_id, reason, message);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate Vex raw receive-pack ref audit insert");
		goto out;
	}
	ret = mysql_exec(db, sql);

out:
	free(sql);
	free(message);
	free(reason);
	free(push_id);
	free(actor);
	free(object);
	free(refname);
	return ret;
}

int vex_meta_mysql_receive_pack_seal_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct git_hash_algo *hash_algo,
	const struct vex_ref_update *updates, size_t updates_nr)
{
	char *repo = NULL;
	char *pack = NULL;
	char *sql = NULL;
	unsigned char pack_id[VEX_META_MYSQL_OID_BYTES];
	uint64_t count;
	int found;
	int ret = VEX_META_MYSQL_ERR;
	size_t i;

	if (!db || !db->mysql || !db->state_transaction || !generation ||
	    !hash_algo || (hash_algo->rawsz != 20 && hash_algo->rawsz != 32) ||
	    (!updates && updates_nr))
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID for raw receive-pack seal");
		goto out;
	}
	if ((ret = state_verify_repository(db, repo, hash_algo)))
		goto out;
	sql = vex_format("SELECT COUNT(*) FROM generations WHERE repo_id = %s "
			 "AND generation = %" PRIu64 " AND state = 'open' FOR UPDATE",
			 repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive-pack generation seal check");
		goto out;
	}
	ret = query_one_u64(db, sql, &count, &found);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (!found || count != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"cannot seal a missing or non-open raw receive generation");
		goto out;
	}
	if ((ret = receive_pack_find_building_sql_locked(db, repo, generation, pack_id)) ||
	    (ret = receive_pack_require_unborn_head_scaffold_locked(db, repo)))
		goto out;
	for (i = 0; i < updates_nr; i++) {
		if ((ret = receive_pack_seed_update_valid(db, &updates[i])) ||
		    (ret = receive_pack_require_member_locked(db, repo, pack_id,
						      &updates[i].new_target.oid,
						      hash_algo)) ||
		    (ret = receive_pack_insert_ref_locked(db, repo, &updates[i], hash_algo)))
			goto out;
	}
	pack = sql_hex_literal(pack_id, sizeof(pack_id));
	if (!pack) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not encode raw receive-pack identifier for seal");
		goto out;
	}
	sql = vex_format("UPDATE pack_envelopes SET state = 'sealed', "
			 "sealed_at = CURRENT_TIMESTAMP(6) WHERE repo_id = %s "
			 "AND pack_id = %s AND source_kind = 'receive' "
			 "AND state = 'building' AND receive_generation = %" PRIu64,
			 repo, pack, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive-pack envelope seal");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"building raw receive-pack changed before seal");
		goto out;
	}
	sql = vex_format("UPDATE generations SET state = 'sealed', "
			 "sealed_at = CURRENT_TIMESTAMP(6) WHERE repo_id = %s "
			 "AND generation = %" PRIu64 " AND state = 'open'", repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive generation seal");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"raw receive generation changed before seal");
		goto out;
	}
	/* A full raw pair is already a current clone seed; no derived build needed. */
	sql = vex_format("DELETE FROM pack_rebuild_requests WHERE repo_id = %s "
			 "AND target_generation <= %" PRIu64, repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive-pack rebuild clear");
		goto out;
	}
	ret = mysql_exec(db, sql);

out:
	free(sql);
	free(pack);
	free(repo);
	return ret;
}

int vex_meta_mysql_receive_pack_abort_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	unsigned char out_pack_id[VEX_META_MYSQL_OID_BYTES])
{
	char *repo = NULL;
	char *pack = NULL;
	char *sql = NULL;
	unsigned char pack_id[VEX_META_MYSQL_OID_BYTES];
	int ret = VEX_META_MYSQL_ERR;

	if (out_pack_id)
		memset(out_pack_id, 0, VEX_META_MYSQL_OID_BYTES);
	if (!db || !db->mysql || !db->state_transaction || !generation ||
	    !out_pack_id)
		return VEX_META_MYSQL_ERR_INVALID;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo) {
		ret = set_error(db, VEX_META_MYSQL_ERR_INVALID,
				"invalid Vex repository UUID for raw receive-pack abort");
		goto out;
	}
	if ((ret = receive_pack_find_building_sql_locked(db, repo, generation, pack_id)))
		goto out;
	pack = sql_hex_literal(pack_id, sizeof(pack_id));
	if (!pack) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not encode raw receive-pack identifier for abort");
		goto out;
	}
	sql = vex_format("DELETE FROM received_pack_objects WHERE repo_id = %s "
			 "AND pack_id = %s", repo, pack);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive-pack member cleanup");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	sql = vex_format("DELETE FROM pack_envelopes WHERE repo_id = %s "
			 "AND pack_id = %s AND source_kind = 'receive' "
			 "AND state = 'building' AND receive_generation = %" PRIu64,
			 repo, pack, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive-pack envelope cleanup");
		goto out;
	}
	ret = mysql_exec(db, sql);
	free(sql);
	sql = NULL;
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"building raw receive-pack changed before abort");
		goto out;
	}
	sql = vex_format("UPDATE generations SET state = 'aborted' "
			 "WHERE repo_id = %s AND generation = %" PRIu64 " "
			 "AND state = 'open'", repo, generation);
	if (!sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive generation abort");
		goto out;
	}
	ret = mysql_exec(db, sql);
	if (ret)
		goto out;
	if (mysql_affected_rows(db->mysql) != 1) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"raw receive generation changed before abort");
		goto out;
	}
	memcpy(out_pack_id, pack_id, sizeof(pack_id));
	ret = VEX_META_MYSQL_OK;

out:
	free(sql);
	free(pack);
	free(repo);
	return ret;
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
	char *raw_sql = NULL;
	struct vex_meta_mysql_saved_pins pins = { 0 };
	struct vex_meta_mysql_saved_packs packs = { 0 };
	uint64_t sealed_generation;
	uint64_t raw_receive_packs;
	int raw_found;
	int has_current_pack;
	int ret;

	if (!db || !db->mysql || !db->state_transaction || !repo_id || !hash_algo ||
	    !state)
		return VEX_META_MYSQL_ERR_STATE;
	repo = sql_repo_literal(repo_id, NULL);
	if (!repo)
		return set_error(db, VEX_META_MYSQL_ERR_INVALID,
				 "invalid Vex repository UUID");
	/*
	 * The snapshot writer rewrites objects and pack_envelopes.  Until the
	 * generic pack-set/read path can serve a raw base plus later loose objects,
	 * reject rather than silently lose the canonical raw mapping or publish an
	 * unreadable incremental generation.
	 */
	raw_sql = vex_format("SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = %s "
			 "AND source_kind = 'receive' AND state <> 'deleted'", repo);
	if (!raw_sql) {
		ret = set_error(db, VEX_META_MYSQL_ERR_NOMEM,
				"could not allocate raw receive-pack state-save guard");
		goto out;
	}
	ret = query_one_u64(db, raw_sql, &raw_receive_packs, &raw_found);
	free(raw_sql);
	raw_sql = NULL;
	if (ret)
		goto out;
	if (!raw_found || raw_receive_packs) {
		ret = set_error(db, VEX_META_MYSQL_ERR_STATE,
				"legacy state save is disabled while a raw receive pack is retained");
		goto out;
	}
	if ((ret = state_load_pins(db, repo, &pins)) ||
	    (ret = state_load_packs(db, repo, &packs)))
		goto out;
	sealed_generation = state_latest_sealed_generation(state);
	has_current_pack = state_has_current_complete_pack(&packs,
			sealed_generation, state, hash_algo);
	if ((ret = state_delete_rows(db, repo)) ||
	    (ret = state_insert_epochs(db, repo, state)) ||
	    (ret = state_insert_objects(db, repo, repo_id, hash_algo, state)) ||
	    (ret = state_insert_generation_objects(db, repo, hash_algo, state)) ||
	    (ret = state_insert_commit_parents(db, repo, hash_algo, state)) ||
	    (ret = state_insert_refs(db, repo, hash_algo, state)) ||
	    (ret = state_insert_ref_updates(db, repo, hash_algo, state)) ||
	    (ret = state_insert_pins(db, repo, &pins)) ||
	    (ret = state_insert_packs(db, repo, &packs,
			      sealed_generation, state, hash_algo)) ||
	    (ret = state_update_pack_rebuild_request(db, repo,
					       sealed_generation, has_current_pack)))
		goto out;
	ret = vex_mysql_commit(db);
	if (!ret)
		db->state_transaction = 0;
out:
	free(raw_sql);
	state_saved_packs_release(&packs);
	state_saved_pins_release(&pins);
	free(repo);
	return ret;
}
#else /* VEX_META_MYSQL_STORE */

int vex_meta_mysql_current_sealed_pack(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const struct git_hash_algo *hash_algo,
				       struct vex_meta_mysql_pack *pack)
{
	(void)repo_id;
	(void)hash_algo;
	if (pack)
		memset(pack, 0, sizeof(*pack));
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
}

void vex_meta_mysql_pack_release(struct vex_meta_mysql_pack *pack)
{
	if (!pack)
		return;
	free(pack->pack_key);
	free(pack->idx_key);
	memset(pack, 0, sizeof(*pack));
}

int vex_meta_mysql_receive_pack_stage_manifest(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct git_hash_algo *hash_algo,
	const struct vex_meta_mysql_receive_pack *pack,
	const char *manifest_path, uint64_t expected_members)
{
	(void)repo_id;
	(void)generation;
	(void)hash_algo;
	(void)pack;
	(void)manifest_path;
	(void)expected_members;
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
}

int vex_meta_mysql_receive_pack_object_get_visible(
	struct vex_meta_mysql *db, const char *repo_id,
	const unsigned char *oid, size_t oid_len, uint64_t visible_generation,
	struct vex_meta_mysql_receive_pack_object *object)
{
	(void)repo_id;
	(void)oid;
	(void)oid_len;
	(void)visible_generation;
	if (object)
		memset(object, 0, sizeof(*object));
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
}

void vex_meta_mysql_receive_pack_object_release(
	struct vex_meta_mysql_receive_pack_object *object)
{
	if (!object)
		return;
	free(object->pack_key);
	free(object->idx_key);
	memset(object, 0, sizeof(*object));
}

int vex_meta_mysql_receive_pack_seal_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct git_hash_algo *hash_algo,
	const struct vex_ref_update *updates, size_t updates_nr)
{
	(void)repo_id;
	(void)generation;
	(void)hash_algo;
	(void)updates;
	(void)updates_nr;
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
}

int vex_meta_mysql_receive_pack_get_building_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	unsigned char out_pack_id[VEX_META_MYSQL_OID_BYTES])
{
	(void)repo_id;
	(void)generation;
	if (out_pack_id)
		memset(out_pack_id, 0, VEX_META_MYSQL_OID_BYTES);
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
}

int vex_meta_mysql_receive_pack_abort_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	unsigned char out_pack_id[VEX_META_MYSQL_OID_BYTES])
{
	(void)repo_id;
	(void)generation;
	if (out_pack_id)
		memset(out_pack_id, 0, VEX_META_MYSQL_OID_BYTES);
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
}

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

int vex_meta_mysql_state_refresh_receive_heartbeat_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		uint64_t generation)
{
	(void)repo_id;
	(void)generation;
	return set_error(db, VEX_META_MYSQL_ERR_STATE,
			 "Vex MySQL store adapter was not compiled into this binary");
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

#include "git-compat-util.h"
#include "vex/blob-s3.h"
#include "hex.h"
#include "parse.h"
#include "run-command.h"
#include "strbuf.h"
#include "tempfile.h"
#include "wrapper.h"

/*
 * Git's main binary is not linked to libcurl: only the HTTP transport helper
 * is.  The local curl executable is already a required part of the HTTP
 * development topology and supports --aws-sigv4.  Invoking it through the
 * run-command API avoids adding a second libcurl linkage path to Git's core.
 *
 * Credentials are passed through a mode-0600 curl config file, never as argv
 * entries.  A config file is used instead of curl's stdin because curl may
 * close stdin as soon as it has parsed its config, which makes a generic
 * pipe writer susceptible to SIGPIPE.
 */

struct vex_blob_s3 {
	char *endpoint;
	char *bucket;
	char *region;
	char *access_key;
	char *secret_key;
	char *repo_id;
	char *ca_file;
	const struct git_hash_algo *hash_algo;
	int insecure_tls;
	/* Test-only one-shot fault for receive-epoch compensation coverage. */
	int test_fail_promote_once;
	/* Test-only fault for proving a derived-pack cache serves object bytes. */
	int test_fail_reads;
	/* Test-only fault for proving metadata deletion never depends on S3 success. */
	int test_fail_final_delete;
	/* Test-only partial staging fault for the bounded receive batch. */
	int test_fail_stage_batch_once;
};

/*
 * The seed performance gate gives one 4,096-object receive batch one S3
 * transfer window.  Byte pressure remains bounded by the store's 32 MiB
 * batch cap; this is an explicit local stress setting, not an unbounded
 * transfer queue.
 */
#define VEX_S3_BATCH_PARALLEL 4096

static int contains_control(const char *value)
{
	const unsigned char *p = (const unsigned char *)value;

	for (; *p; p++)
		if (*p < 0x20 || *p == 0x7f)
			return 1;
	return 0;
}

static int valid_repo_id(const char *repo_id, struct strbuf *normalized)
{
	size_t i;
	size_t len;
	int hyphenated;

	if (!repo_id)
		return 0;
	len = strlen(repo_id);
	hyphenated = len == 36;
	if (len != 32 && !hyphenated)
		return 0;
	for (i = 0; i < len; i++) {
		unsigned char c = repo_id[i];

		if (hyphenated && (i == 8 || i == 13 || i == 18 ||
				  i == 23)) {
			if (c != '-')
				return 0;
			continue;
		}
		if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))
			return 0;
		strbuf_addch(normalized, c);
	}
	return 1;
}

static int valid_bucket(const char *bucket)
{
	size_t i;
	size_t len;

	if (!bucket)
		return 0;
	len = strlen(bucket);
	if (len < 3 || len > 63 || bucket[0] == '.' || bucket[0] == '-' ||
	    bucket[len - 1] == '.' || bucket[len - 1] == '-')
		return 0;
	for (i = 0; i < len; i++) {
		unsigned char c = bucket[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		      c == '.' || c == '-'))
			return 0;
	}
	return !strstr(bucket, "..");
}

static int valid_region(const char *region)
{
	const unsigned char *p = (const unsigned char *)region;

	if (!region || !*region)
		return 0;
	for (; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') ||
		      (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '-'))
			return 0;
	return 1;
}

static int valid_endpoint(const char *endpoint)
{
	size_t len;

	if (!endpoint || contains_control(endpoint) || strchr(endpoint, '?') ||
	    strchr(endpoint, '#') || strchr(endpoint, '@'))
		return 0;
	if (starts_with(endpoint, "http://"))
		len = strlen("http://");
	else if (starts_with(endpoint, "https://"))
		len = strlen("https://");
	else
		return 0;
	return endpoint[len] != '\0';
}

static int valid_credential(const char *value)
{
	return value && *value && !contains_control(value);
}

static void secure_strbuf_release(struct strbuf *buf)
{
	if (buf->buf)
		memset(buf->buf, 0, buf->len);
	strbuf_release(buf);
}

static int curl_config_add(struct strbuf *config, const char *name,
			   const char *value)
{
	const unsigned char *p = (const unsigned char *)value;

	if (!value || contains_control(value))
		return -1;
	strbuf_addf(config, "%s = \"", name);
	for (; *p; p++) {
		if (*p == '\\' || *p == '"')
			strbuf_addch(config, '\\');
		strbuf_addch(config, *p);
	}
	strbuf_addstr(config, "\"\n");
	return 0;
}

static int parse_http_status(const struct strbuf *output, int *status)
{
	const char *p = output->buf;
	size_t len = output->len;
	int value;

	while (len && isspace((unsigned char)*p)) {
		p++;
		len--;
	}
	if (len < 3 || p[0] < '0' || p[0] > '9' || p[1] < '0' ||
	    p[1] > '9' || p[2] < '0' || p[2] > '9')
		return -1;
	value = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
	p += 3;
	len -= 3;
	while (len && isspace((unsigned char)*p)) {
		p++;
		len--;
	}
	if (len)
		return -1;
	*status = value;
	return 0;
}

static int curl_result(int curl_ret, const struct strbuf *output)
{
	int status;

	if (!parse_http_status(output, &status) && status == 404)
		return VEX_BLOB_S3_ERR_NOT_FOUND;
	if (!curl_ret && !parse_http_status(output, &status) &&
	    status >= 200 && status < 300)
		return VEX_BLOB_S3_OK;
	return VEX_BLOB_S3_ERR;
}

static int run_curl(struct vex_blob_s3 *blob, const char *method,
		    const char *url, const char *input_file,
		    const char *output_file, const char *header)
{
	struct child_process curl = CHILD_PROCESS_INIT;
	struct strbuf config = STRBUF_INIT;
	struct strbuf auth = STRBUF_INIT;
	struct strbuf output = STRBUF_INIT;
	struct strbuf signing = STRBUF_INIT;
	struct tempfile *config_file = NULL;
	struct tempfile *status_file = NULL;
	int status_fd = -1;
	int ret;

	if (!exists_in_PATH("curl"))
		return VEX_BLOB_S3_ERR_UNAVAILABLE;
	strbuf_addf(&auth, "%s:%s", blob->access_key, blob->secret_key);
	strbuf_addf(&signing, "aws:amz:%s:s3", blob->region);
	if (curl_config_add(&config, "url", url) ||
	    (strcmp(method, "HEAD") &&
	     curl_config_add(&config, "request", method)) ||
	    curl_config_add(&config, "user", auth.buf) ||
	    curl_config_add(&config, "aws-sigv4", signing.buf) ||
	    (input_file && curl_config_add(&config, "upload-file", input_file)) ||
	    (header && curl_config_add(&config, "header", header))) {
		ret = VEX_BLOB_S3_ERR_INVALID;
		goto done;
	}
	config_file = mks_tempfile_t("vex-s3-curl-XXXXXX");
	if (!config_file ||
	    write_in_full(config_file->fd, config.buf, config.len) < 0 ||
	    close_tempfile_gently(config_file) < 0) {
		ret = VEX_BLOB_S3_ERR;
		goto done;
	}
	status_file = mks_tempfile_t("vex-s3-status-XXXXXX");
	if (!status_file || close_tempfile_gently(status_file) < 0) {
		ret = VEX_BLOB_S3_ERR;
		goto done;
	}
	status_fd = open_nofollow(status_file->filename.buf, O_WRONLY | O_TRUNC);
	if (status_fd < 0) {
		ret = VEX_BLOB_S3_ERR;
		goto done;
	}

	strvec_pushl(&curl.args, "curl", "--config", config_file->filename.buf,
		     "--silent",
		     "--fail", "--output", output_file,
		     "--write-out", "%{http_code}", NULL);
	if (!strcmp(method, "HEAD"))
		strvec_push(&curl.args, "--head");
	if (blob->ca_file)
		strvec_pushl(&curl.args, "--cacert", blob->ca_file, NULL);
	if (blob->insecure_tls)
		strvec_push(&curl.args, "--insecure");

	curl.out = status_fd;
	status_fd = -1;
	ret = run_command(&curl);
	if (strbuf_read_file(&output, status_file->filename.buf, 64) < 0)
		ret = VEX_BLOB_S3_ERR;
	else
		ret = curl_result(ret, &output);
	done:
	if (status_fd >= 0)
		close(status_fd);
	delete_tempfile(&config_file);
	delete_tempfile(&status_file);
	secure_strbuf_release(&config);
	secure_strbuf_release(&auth);
	strbuf_release(&signing);
	strbuf_release(&output);
	return ret;
}

/*
 * The receive path spends most of its time writing many tiny immutable
 * objects.  Keep curl as the one AWS-SigV4 implementation in this binary,
 * but hand it a bounded parallel batch instead of paying one process start
 * and connection setup per object.  Every request stays an ordinary S3 PUT,
 * COPY, or DELETE at its own key; batching changes transport concurrency, not
 * the object layout or visibility rules.
 */
struct vex_s3_batch_request {
	const char *method;
	const char *url;
	const char *input_file;
	const char *header;
};

static int curl_config_add_request(struct strbuf *config,
				   struct vex_blob_s3 *blob,
				   const struct vex_s3_batch_request *request,
				   int more)
{
	struct strbuf auth = STRBUF_INIT;
	struct strbuf signing = STRBUF_INIT;
	int ret = -1;

	if (!request || !request->method || !request->url)
		goto out;
	strbuf_addf(&auth, "%s:%s", blob->access_key, blob->secret_key);
	strbuf_addf(&signing, "aws:amz:%s:s3", blob->region);
	if (curl_config_add(config, "url", request->url) ||
	    curl_config_add(config, "request", request->method) ||
	    curl_config_add(config, "user", auth.buf) ||
	    curl_config_add(config, "aws-sigv4", signing.buf) ||
	    (request->input_file &&
	     curl_config_add(config, "upload-file", request->input_file)) ||
	    (request->header &&
	     curl_config_add(config, "header", request->header)) ||
	    curl_config_add(config, "output", "/dev/null"))
		goto out;
	/*
	 * `--next` restores transfer-local options.  Keep each request quiet and
	 * fail-fast so curl's parallel progress meter cannot enter Git's stderr
	 * (which receive-pack would otherwise relay to a stock client).
	 */
	strbuf_addstr(config, "silent\nshow-error\nfail\n");
	if (more)
		strbuf_addstr(config, "next\n");
	ret = 0;
out:
	secure_strbuf_release(&auth);
	strbuf_release(&signing);
	return ret;
}

static int run_curl_parallel(struct vex_blob_s3 *blob,
			     const struct vex_s3_batch_request *requests,
			     size_t requests_nr)
{
	struct child_process curl = CHILD_PROCESS_INIT;
	struct strbuf config = STRBUF_INIT;
	struct tempfile *config_file = NULL;
	char parallel[32];
	size_t i;
	int ret = VEX_BLOB_S3_ERR;

	if (!requests_nr)
		return VEX_BLOB_S3_OK;
	if (!exists_in_PATH("curl"))
		return VEX_BLOB_S3_ERR_UNAVAILABLE;
	for (i = 0; i < requests_nr; i++)
		if (curl_config_add_request(&config, blob, &requests[i],
					    i + 1 < requests_nr)) {
			ret = VEX_BLOB_S3_ERR_INVALID;
			goto out;
		}
	config_file = mks_tempfile_t("vex-s3-curl-batch-XXXXXX");
	if (!config_file ||
	    write_in_full(config_file->fd, config.buf, config.len) < 0 ||
	    close_tempfile_gently(config_file) < 0)
		goto out;

	xsnprintf(parallel, sizeof(parallel), "%u", VEX_S3_BATCH_PARALLEL);
	/*
	 * curl 7.88 (the runtime version in the gateway image) can still write its
	 * parallel progress meter after a config-file `next`, even with `silent`
	 * set for each transfer.  Direct the whole batch transport diagnostic
	 * stream away from Git's stderr: receive-pack must never forward a curl
	 * progress line as smart-HTTP sideband output.  The caller gets a precise
	 * success/failure result from curl's exit status, so this does not hide a
	 * storage failure from the transaction.
	 */
	strvec_pushl(&curl.args, "curl", "--silent", "--show-error", "--fail",
		     "--stderr", "/dev/null", "--parallel", "--parallel-immediate",
		     "--parallel-max", parallel, "--config", config_file->filename.buf,
		     NULL);
	if (blob->ca_file)
		strvec_pushl(&curl.args, "--cacert", blob->ca_file, NULL);
	if (blob->insecure_tls)
		strvec_push(&curl.args, "--insecure");
	ret = run_command(&curl) ? VEX_BLOB_S3_ERR : VEX_BLOB_S3_OK;
out:
	delete_tempfile(&config_file);
	secure_strbuf_release(&config);
	return ret;
}

static int make_url(const struct vex_blob_s3 *blob, const char *key,
		    struct strbuf *url)
{
	if (!blob || !key || !url)
		return VEX_BLOB_S3_ERR_INVALID;
	strbuf_addf(url, "%s/%s", blob->endpoint, blob->bucket);
	if (*key)
		strbuf_addf(url, "/%s", key);
	return VEX_BLOB_S3_OK;
}

static int head_key(struct vex_blob_s3 *blob, const char *key)
{
	struct strbuf url = STRBUF_INIT;
	int ret;

	ret = make_url(blob, key, &url);
	if (!ret)
		ret = run_curl(blob, "HEAD", url.buf, NULL, "/dev/null", NULL);
	strbuf_release(&url);
	return ret;
}

static int delete_key(struct vex_blob_s3 *blob, const char *key)
{
	struct strbuf url = STRBUF_INIT;
	int ret;

	ret = make_url(blob, key, &url);
	if (!ret)
		ret = run_curl(blob, "DELETE", url.buf, NULL, "/dev/null", NULL);
	strbuf_release(&url);
	return ret;
}

static int put_key(struct vex_blob_s3 *blob, const char *key,
		   const void *data, unsigned long size)
{
	struct tempfile *temp;
	struct strbuf url = STRBUF_INIT;
	int ret;

	if (size && !data)
		return VEX_BLOB_S3_ERR_INVALID;
	temp = mks_tempfile_t("vex-s3-put-XXXXXX");
	if (!temp)
		return VEX_BLOB_S3_ERR;
	if ((size && write_in_full(temp->fd, data, size) < 0) ||
	    close_tempfile_gently(temp) < 0) {
		delete_tempfile(&temp);
		return VEX_BLOB_S3_ERR;
	}
	ret = make_url(blob, key, &url);
	if (!ret)
		ret = run_curl(blob, "PUT", url.buf, temp->filename.buf,
			       "/dev/null", "Content-Type: application/octet-stream");
	strbuf_release(&url);
	delete_tempfile(&temp);
	return ret;
}

static int read_key(struct vex_blob_s3 *blob, const char *key,
		    void **data, unsigned long *size)
{
	struct tempfile *temp;
	struct strbuf url = STRBUF_INIT;
	struct stat st;
	void *contents;
	ssize_t bytes;
	int fd = -1;
	int ret;

	if (blob->test_fail_reads)
		return VEX_BLOB_S3_ERR_UNAVAILABLE;
	if (!data || !size)
		return VEX_BLOB_S3_ERR_INVALID;
	*data = NULL;
	*size = 0;
	temp = mks_tempfile_t("vex-s3-get-XXXXXX");
	if (!temp)
		return VEX_BLOB_S3_ERR;
	if (close_tempfile_gently(temp) < 0) {
		delete_tempfile(&temp);
		return VEX_BLOB_S3_ERR;
	}
	ret = make_url(blob, key, &url);
	if (!ret)
		ret = run_curl(blob, "GET", url.buf, NULL, temp->filename.buf, NULL);
	strbuf_release(&url);
	if (ret)
		goto done;
	fd = open_nofollow(temp->filename.buf, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
	    st.st_size < 0 || (uintmax_t)st.st_size > ULONG_MAX) {
		ret = VEX_BLOB_S3_ERR;
		goto done;
	}
	contents = xmallocz(st.st_size);
	bytes = read_in_full(fd, contents, st.st_size);
	if (bytes != st.st_size) {
		free(contents);
		ret = VEX_BLOB_S3_ERR;
		goto done;
	}
	*data = contents;
	*size = st.st_size;
	done:
	if (fd >= 0)
		close(fd);
	delete_tempfile(&temp);
	return ret;
}

static int download_key(struct vex_blob_s3 *blob, const char *key,
			const char *destination)
{
	struct strbuf url = STRBUF_INIT;
	int ret;

	if (blob->test_fail_reads)
		return VEX_BLOB_S3_ERR_UNAVAILABLE;
	if (!destination || !*destination || contains_control(destination))
		return VEX_BLOB_S3_ERR_INVALID;
	ret = make_url(blob, key, &url);
	if (!ret)
		ret = run_curl(blob, "GET", url.buf, NULL, destination, NULL);
	strbuf_release(&url);
	return ret;
}

static int derived_pack_key(const struct vex_blob_s3 *blob,
			    const unsigned char *pack_id, size_t pack_id_len,
			    int index, struct strbuf *key)
{
	char hex[GIT_MAX_HEXSZ + 1];

	if (!blob || !blob->hash_algo || !pack_id ||
	    pack_id_len != blob->hash_algo->rawsz || !key)
		return VEX_BLOB_S3_ERR_INVALID;
	hash_to_hex_algop_r(hex, pack_id, blob->hash_algo);
	strbuf_addf(key, "git/v1/%s/packs/%s.%s", blob->repo_id, hex,
		    index ? "idx" : "pack");
	return VEX_BLOB_S3_OK;
}

int vex_blob_s3_open(struct vex_blob_s3 **blob_out,
		     const struct vex_blob_s3_options *opts)
{
	struct vex_blob_s3 *blob;
	struct strbuf repo_id = STRBUF_INIT;
	struct strbuf endpoint = STRBUF_INIT;
	size_t len;

	if (!blob_out || !opts || !opts->hash_algo ||
	    !valid_endpoint(opts->endpoint) || !valid_bucket(opts->bucket) ||
	    !valid_region(opts->region) || !valid_credential(opts->access_key) ||
	    !valid_credential(opts->secret_key) ||
	    (opts->ca_file && contains_control(opts->ca_file)) ||
	    !valid_repo_id(opts->repo_id, &repo_id)) {
		strbuf_release(&repo_id);
		return VEX_BLOB_S3_ERR_INVALID;
	}
	strbuf_addstr(&endpoint, opts->endpoint);
	while (endpoint.len && endpoint.buf[endpoint.len - 1] == '/')
		strbuf_setlen(&endpoint, endpoint.len - 1);
	len = starts_with(endpoint.buf, "https://") ? strlen("https://") :
		strlen("http://");
	if (endpoint.len <= len) {
		strbuf_release(&repo_id);
		strbuf_release(&endpoint);
		return VEX_BLOB_S3_ERR_INVALID;
	}

	CALLOC_ARRAY(blob, 1);
	blob->endpoint = strbuf_detach(&endpoint, NULL);
	blob->bucket = xstrdup(opts->bucket);
	blob->region = xstrdup(opts->region);
	blob->access_key = xstrdup(opts->access_key);
	blob->secret_key = xstrdup(opts->secret_key);
	blob->repo_id = strbuf_detach(&repo_id, NULL);
	blob->ca_file = opts->ca_file ? xstrdup(opts->ca_file) : NULL;
	blob->hash_algo = opts->hash_algo;
	blob->insecure_tls = !!opts->insecure_tls;
	/*
	 * This is deliberately not a storage configuration option.  It is an
	 * opt-in test hook used by the Compose fault-injection gate; a process
	 * with the variable set fails its first attempted staged-to-final copy.
	 */
	blob->test_fail_promote_once =
		git_env_bool("GIT_VEX_TEST_S3_FAIL_PROMOTE_ONCE", 0) == 1;
	/*
	 * This deliberately affects every S3 byte-read (canonical object or
	 * derived pack).  The Compose cache test first materializes and verifies
	 * the pair, then turns this on to prove the next request uses no S3 bytes.
	 */
	blob->test_fail_reads =
		git_env_bool("GIT_VEX_TEST_S3_FAIL_READS", 0) == 1;
	/*
	 * Used only by the destructive-sweep integration gate.  A failed delete
	 * leaves its durable metadata tombstone pending for the next invocation.
	 */
	blob->test_fail_final_delete =
		git_env_bool("GIT_VEX_TEST_S3_FAIL_FINAL_DELETE", 0) == 1;
	/*
	 * This hook exercises the only awkward batch outcome: a request subset
	 * reached S3 but the batch as a whole failed before metadata was written.
	 * The driver must remove every attempted staging key in that case.
	 */
	blob->test_fail_stage_batch_once =
		git_env_bool("GIT_VEX_TEST_S3_FAIL_STAGE_BATCH_ONCE", 0) == 1;
	*blob_out = blob;
	return VEX_BLOB_S3_OK;
}

void vex_blob_s3_close(struct vex_blob_s3 *blob)
{
	if (!blob)
		return;
	free(blob->endpoint);
	free(blob->bucket);
	free(blob->region);
	free(blob->access_key);
	if (blob->secret_key) {
		memset(blob->secret_key, 0, strlen(blob->secret_key));
		free(blob->secret_key);
	}
	free(blob->repo_id);
	free(blob->ca_file);
	free(blob);
}

int vex_blob_s3_check_bucket(struct vex_blob_s3 *blob)
{
	return head_key(blob, "");
}

int vex_blob_s3_final_key(const struct vex_blob_s3 *blob,
			  const struct object_id *oid, struct strbuf *key)
{
	char hex[GIT_MAX_HEXSZ + 1];

	if (!blob || !oid || !key)
		return VEX_BLOB_S3_ERR_INVALID;
	hash_to_hex_algop_r(hex, oid->hash, blob->hash_algo);
	strbuf_addf(key, "git/v1/%s/objects/%s", blob->repo_id, hex);
	return VEX_BLOB_S3_OK;
}

int vex_blob_s3_staging_key(const struct vex_blob_s3 *blob, uint64_t epoch,
			    const struct object_id *oid, struct strbuf *key)
{
	char hex[GIT_MAX_HEXSZ + 1];

	if (!blob || !oid || !key)
		return VEX_BLOB_S3_ERR_INVALID;
	hash_to_hex_algop_r(hex, oid->hash, blob->hash_algo);
	strbuf_addf(key, "git/v1/%s/staging/%"PRIu64"/objects/%s",
		    blob->repo_id, epoch, hex);
	return VEX_BLOB_S3_OK;
}

int vex_blob_s3_put_staging(struct vex_blob_s3 *blob, uint64_t epoch,
			    const struct object_id *oid,
			    const void *data, unsigned long size)
{
	struct strbuf key = STRBUF_INIT;
	int ret;

	ret = vex_blob_s3_staging_key(blob, epoch, oid, &key);
	if (!ret)
		ret = put_key(blob, key.buf, data, size);
	strbuf_release(&key);
	return ret;
}

struct vex_s3_batch_upload {
	struct tempfile *temp;
	struct strbuf staging;
	struct strbuf url;
};

static void release_batch_uploads(struct vex_s3_batch_upload *uploads,
				  size_t uploads_nr)
{
	size_t i;

	for (i = 0; i < uploads_nr; i++) {
		delete_tempfile(&uploads[i].temp);
		strbuf_release(&uploads[i].url);
		strbuf_release(&uploads[i].staging);
	}
	free(uploads);
}

int vex_blob_s3_put_staging_batch(
	struct vex_blob_s3 *blob, uint64_t epoch,
	const struct vex_blob_s3_staged_object *objects, size_t objects_nr)
{
	struct vex_s3_batch_upload *uploads = NULL;
	struct vex_s3_batch_request *requests = NULL;
	static const char content_type[] = "Content-Type: application/octet-stream";
	size_t i;
	int ret = VEX_BLOB_S3_ERR;

	if (!blob || (!objects && objects_nr))
		return VEX_BLOB_S3_ERR_INVALID;
	if (!objects_nr)
		return VEX_BLOB_S3_OK;
	CALLOC_ARRAY(uploads, objects_nr);
	CALLOC_ARRAY(requests, objects_nr);
	for (i = 0; i < objects_nr; i++) {
		const struct vex_blob_s3_staged_object *object = &objects[i];
		struct vex_s3_batch_upload *upload = &uploads[i];

		if (!object->oid || (object->size && !object->data)) {
			ret = VEX_BLOB_S3_ERR_INVALID;
			goto cleanup;
		}
		upload->temp = mks_tempfile_t("vex-s3-put-batch-XXXXXX");
		if (!upload->temp ||
		    (object->size && write_in_full(upload->temp->fd, object->data,
						 object->size) < 0) ||
		    close_tempfile_gently(upload->temp) < 0)
			goto cleanup;
		ret = vex_blob_s3_staging_key(blob, epoch, object->oid,
					      &upload->staging);
		if (ret)
			goto cleanup;
		ret = make_url(blob, upload->staging.buf, &upload->url);
		if (ret)
			goto cleanup;
		requests[i].method = "PUT";
		requests[i].url = upload->url.buf;
		requests[i].input_file = upload->temp->filename.buf;
		requests[i].header = content_type;
	}

	if (blob->test_fail_stage_batch_once) {
		/* Write one real key, then exercise all-or-nothing cleanup. */
		blob->test_fail_stage_batch_once = 0;
		ret = run_curl_parallel(blob, requests, 1);
		if (!ret)
			ret = VEX_BLOB_S3_ERR;
		goto cleanup;
	}
	ret = run_curl_parallel(blob, requests, objects_nr);
cleanup:
	if (ret) {
		/*
		 * No metadata was written yet.  Best-effort deletion of every
		 * attempt leaves a failed receive with no discoverable quarantine
		 * state; the caller can fail atomically rather than publishing a
		 * partial batch.
		 */
		for (i = 0; i < objects_nr; i++)
			if (uploads[i].staging.len)
				(void)delete_key(blob, uploads[i].staging.buf);
	}
	release_batch_uploads(uploads, objects_nr);
	free(requests);
	return ret;
}

int vex_blob_s3_delete_staging(struct vex_blob_s3 *blob, uint64_t epoch,
			       const struct object_id *oid)
{
	struct strbuf key = STRBUF_INIT;
	int ret;

	ret = vex_blob_s3_staging_key(blob, epoch, oid, &key);
	if (!ret)
		ret = delete_key(blob, key.buf);
	strbuf_release(&key);
	return ret;
}

int vex_blob_s3_delete_final(struct vex_blob_s3 *blob,
			     const struct object_id *oid)
{
	struct strbuf key = STRBUF_INIT;
	int ret;

	if (blob->test_fail_final_delete)
		return VEX_BLOB_S3_ERR;
	ret = vex_blob_s3_final_key(blob, oid, &key);
	if (!ret)
		ret = delete_key(blob, key.buf);
	strbuf_release(&key);
	return ret;
}

int vex_blob_s3_promote_object(struct vex_blob_s3 *blob, uint64_t epoch,
			       const struct object_id *oid)
{
	return vex_blob_s3_promote_batch(blob, epoch, oid, 1);
}

struct vex_s3_batch_promotion {
	struct strbuf staging;
	struct strbuf final;
	struct strbuf final_url;
	struct strbuf staging_url;
	struct strbuf copy_source;
};

static void release_batch_promotions(struct vex_s3_batch_promotion *promotions,
				     size_t promotions_nr)
{
	size_t i;

	for (i = 0; i < promotions_nr; i++) {
		strbuf_release(&promotions[i].copy_source);
		strbuf_release(&promotions[i].staging_url);
		strbuf_release(&promotions[i].final_url);
		strbuf_release(&promotions[i].final);
		strbuf_release(&promotions[i].staging);
	}
	free(promotions);
}

int vex_blob_s3_promote_batch(struct vex_blob_s3 *blob, uint64_t epoch,
			      const struct object_id *oids, size_t oids_nr)
{
	struct vex_s3_batch_promotion *promotions = NULL;
	struct vex_s3_batch_request *requests = NULL;
	size_t i;
	int ret = VEX_BLOB_S3_ERR;

	if (!blob || (!oids && oids_nr))
		return VEX_BLOB_S3_ERR_INVALID;
	if (!oids_nr)
		return VEX_BLOB_S3_OK;
	if (blob->test_fail_promote_once) {
		blob->test_fail_promote_once = 0;
		return VEX_BLOB_S3_ERR;
	}
	CALLOC_ARRAY(promotions, oids_nr);
	CALLOC_ARRAY(requests, oids_nr);
	for (i = 0; i < oids_nr; i++) {
		struct vex_s3_batch_promotion *promotion = &promotions[i];

		ret = vex_blob_s3_staging_key(blob, epoch, &oids[i],
					      &promotion->staging);
		if (ret)
			goto done;
		ret = vex_blob_s3_final_key(blob, &oids[i], &promotion->final);
		if (ret)
			goto done;
		ret = make_url(blob, promotion->final.buf, &promotion->final_url);
		if (ret)
			goto done;
		ret = make_url(blob, promotion->staging.buf, &promotion->staging_url);
		if (ret)
			goto done;
		/*
		 * The source was hash-verified before staging.  Copying it over an
		 * existing content-addressed key is therefore idempotent (and repairs
		 * a stale orphan) without a serialized HEAD round trip per object.
		 */
		strbuf_addf(&promotion->copy_source,
			    "x-amz-copy-source: /%s/%s", blob->bucket,
			    promotion->staging.buf);
		requests[i].method = "PUT";
		requests[i].url = promotion->final_url.buf;
		requests[i].input_file = NULL;
		requests[i].header = promotion->copy_source.buf;
	}

	ret = run_curl_parallel(blob, requests, oids_nr);
	if (ret)
		goto done;
	for (i = 0; i < oids_nr; i++) {
		requests[i].method = "DELETE";
		requests[i].url = promotions[i].staging_url.buf;
		requests[i].input_file = NULL;
		requests[i].header = NULL;
	}
	ret = run_curl_parallel(blob, requests, oids_nr);
done:
	/* A failed copy/delete leaves no metadata visible and is retryable. */
	free(requests);
	release_batch_promotions(promotions, oids_nr);
	return ret;
}

int vex_blob_s3_has_final(struct vex_blob_s3 *blob,
			  const struct object_id *oid)
{
	struct strbuf key = STRBUF_INIT;
	int ret;

	ret = vex_blob_s3_final_key(blob, oid, &key);
	if (!ret)
		ret = head_key(blob, key.buf);
	strbuf_release(&key);
	if (ret == VEX_BLOB_S3_OK)
		return 1;
	if (ret == VEX_BLOB_S3_ERR_NOT_FOUND)
		return 0;
	return ret;
}

int vex_blob_s3_read_final(struct vex_blob_s3 *blob,
			   const struct object_id *oid,
			   void **data, unsigned long *size)
{
	struct strbuf key = STRBUF_INIT;
	int ret;

	ret = vex_blob_s3_final_key(blob, oid, &key);
	if (!ret)
		ret = read_key(blob, key.buf, data, size);
	strbuf_release(&key);
	return ret;
}

int vex_blob_s3_read_staging(struct vex_blob_s3 *blob, uint64_t epoch,
			     const struct object_id *oid,
			     void **data, unsigned long *size)
{
	struct strbuf key = STRBUF_INIT;
	int ret;

	ret = vex_blob_s3_staging_key(blob, epoch, oid, &key);
	if (!ret)
		ret = read_key(blob, key.buf, data, size);
	strbuf_release(&key);
	return ret;
}

int vex_blob_s3_download_derived_pack(struct vex_blob_s3 *blob,
				      const unsigned char *pack_id,
				      size_t pack_id_len,
				      int index,
				      const char *destination)
{
	struct strbuf key = STRBUF_INIT;
	int ret;

	ret = derived_pack_key(blob, pack_id, pack_id_len, index, &key);
	if (!ret)
		ret = download_key(blob, key.buf, destination);
	strbuf_release(&key);
	return ret;
}

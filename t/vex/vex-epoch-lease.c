#include "git-compat-util.h"
#include "parse-options.h"
#include "vex/blob-s3.h"
#include "vex/meta-mysql.h"
#include "vex/store.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>

static const char * const epoch_lease_usage[] = {
	"vex-epoch-lease --repo-id=<32-lowercase-hex> (--hold-seconds=<n> | --recover-seconds=<n>)",
	NULL,
};

static const char *env_or(const char *name, const char *fallback)
{
	const char *value = getenv(name);

	return value && *value ? value : fallback;
}

static int env_true(const char *name)
{
	const char *value = getenv(name);

	return value && (!strcmp(value, "1") || !strcmp(value, "true") ||
			 !strcmp(value, "yes"));
}

static unsigned int env_port(const char *name, unsigned int fallback)
{
	const char *value = env_or(name, NULL);
	char *end;
	unsigned long port;

	if (!value)
		return fallback;
	errno = 0;
	port = strtoul(value, &end, 10);
	if (errno || !*value || *end || !port || port > UINT_MAX)
		die("invalid %s", name);
	return (unsigned int)port;
}

static unsigned int parse_seconds(const char *value, const char *option)
{
	char *end;
	unsigned long seconds;

	if (!value || !*value)
		die("%s requires an integer from 1 through 86400", option);
	errno = 0;
	seconds = strtoul(value, &end, 10);
	if (errno || *end || !seconds || seconds > 86400)
		die("%s must be an integer from 1 through 86400", option);
	return (unsigned int)seconds;
}

static void require_store_ok(struct vex_store *store, int ret,
			     const char *operation)
{
	if (!ret)
		return;
	die("%s failed: %s (%s)", operation, vex_store_result_string(ret),
	    vex_store_error(store));
}

int cmd_main(int argc, const char **argv)
{
	const char *repo_id = NULL;
	const char *hold_spec = NULL;
	const char *recover_spec = NULL;
	struct vex_blob_s3_options blob = {
		.endpoint = getenv("GIT_VEX_S3_ENDPOINT"),
		.bucket = getenv("GIT_VEX_S3_BUCKET"),
		.region = env_or("GIT_VEX_S3_REGION", "us-east-1"),
		.access_key = getenv("GIT_VEX_S3_ACCESS_KEY"),
		.secret_key = getenv("GIT_VEX_S3_SECRET_KEY"),
		.ca_file = getenv("GIT_VEX_S3_CA_FILE"),
		.insecure_tls = env_true("GIT_VEX_S3_INSECURE_TLS"),
	};
	struct vex_meta_mysql_options mysql = {
		.host = env_or("GIT_VEX_MYSQL_HOST", "127.0.0.1"),
		.port = 0,
		.user = getenv("GIT_VEX_MYSQL_USER"),
		.password = getenv("GIT_VEX_MYSQL_PASSWORD"),
		.database = getenv("GIT_VEX_MYSQL_DATABASE"),
		.unix_socket = getenv("GIT_VEX_MYSQL_SOCKET"),
	};
	struct vex_store_options options = {
		.backend = VEX_STORE_BACKEND_MYSQL_S3,
		.root = NULL,
		.hash_algo = &hash_algos[GIT_HASH_SHA1],
		.blob_s3 = &blob,
		.meta_mysql = &mysql,
	};
	struct vex_store *store = NULL;
	struct vex_epoch *epoch = NULL;
	struct option options_table[] = {
		OPT_STRING(0, "repo-id", &repo_id, "32-hex", "Vex repository UUID"),
		OPT_STRING(0, "hold-seconds", &hold_spec, "seconds",
			   "hold a live receive epoch and refresh its heartbeat"),
		OPT_STRING(0, "recover-seconds", &recover_spec, "seconds",
			   "reclaim receive epochs whose last heartbeat is stale"),
		OPT_END(),
	};
	unsigned int seconds;
	unsigned int recovered = 0;
	int ret;

	argc = parse_options(argc, argv, NULL, options_table, epoch_lease_usage, 0);
	if (argc || !repo_id || (!!hold_spec + !!recover_spec) != 1)
		usage_with_options(epoch_lease_usage, options_table);
	options.repo_id = repo_id;
	mysql.port = env_port("GIT_VEX_MYSQL_PORT", 3306);

	ret = vex_store_open(&store, &options);
	if (ret)
		die("could not open Vex MySQL+S3 store: %s", vex_store_result_string(ret));
	if (hold_spec) {
		unsigned int i;

		seconds = parse_seconds(hold_spec, "--hold-seconds");
		require_store_ok(store, vex_repo_ensure(store), "ensure Vex repository");
		require_store_ok(store,
			vex_epoch_allocate(store, "vex-epoch-lease", &epoch),
			"allocate receive epoch");
		printf("vex epoch lease: state=active epoch=%" PRIu64
		       " hold_seconds=%u\n", vex_epoch_id(epoch), seconds);
		fflush(stdout);
		for (i = 0; i < seconds; i++) {
			sleep(1);
			require_store_ok(store, vex_epoch_heartbeat(epoch),
					 "refresh receive heartbeat");
		}
		require_store_ok(store, vex_epoch_compensate(epoch),
				 "compensate held receive epoch");
		vex_epoch_free(epoch);
	} else {
		seconds = parse_seconds(recover_spec, "--recover-seconds");
		require_store_ok(store,
			vex_store_recover_expired_epochs(store, seconds, &recovered),
			"recover stale receive epochs");
		printf("vex epoch lease: state=recovered recovered=%u age_seconds=%u\n",
		       recovered, seconds);
	}
	vex_store_close(store);
	return 0;
}

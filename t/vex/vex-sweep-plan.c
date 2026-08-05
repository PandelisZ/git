#include "git-compat-util.h"
#include "hex.h"
#include "odb/source-vex.h"
#include "parse-options.h"
#include "parse.h"
#include "repository.h"
#include "vex/store.h"

static const char * const sweep_plan_usage[] = {
	"vex-sweep-plan --git-dir=<vex-bare-repository> [--pin=<oid>:<reason> | --unpin=<oid>:<reason> | --lease-current-pack=<seconds> | --release-pack-lease=<lease-id> | --sweep [--grace-seconds=<seconds>] [--tombstone-seconds=<seconds>]]",
	NULL,
};

static int parse_pin_spec(const char *spec, const struct git_hash_algo *hash_algo,
			  struct object_id *oid, const char **reason)
{
	size_t hexsz;

	if (!spec || !hash_algo || !oid || !reason)
		return -1;
	hexsz = hash_algo->hexsz;
	if (strlen(spec) <= hexsz + 1 || spec[hexsz] != ':' ||
	    get_oid_hex_algop(spec, oid, hash_algo))
		return -1;
	*reason = spec + hexsz + 1;
	if (strlen(*reason) > 255 || strpbrk(*reason, "\r\n"))
		return -1;
	return 0;
}

static int parse_lease_ttl(const char *spec, unsigned int *ttl)
{
	char *end;
	unsigned long value;

	if (!spec || !*spec || !ttl)
		return -1;
	errno = 0;
	value = strtoul(spec, &end, 10);
	if (errno || !*spec || *end || !value || value > 86400)
		return -1;
	*ttl = value;
	return 0;
}

static int parse_sweep_seconds(const char *spec, unsigned int *seconds,
			       int allow_short)
{
	char *end;
	unsigned long value;

	if (!spec || !*spec || !seconds)
		return -1;
	errno = 0;
	value = strtoul(spec, &end, 10);
	if (errno || !*spec || *end || !value ||
	    value > VEX_STORE_SWEEP_MAX_SECONDS || (!allow_short && value < 60))
		return -1;
	*seconds = value;
	return 0;
}

static int parse_lease_id(const char *spec,
			  unsigned char lease_id[VEX_PACK_LEASE_ID_BYTES])
{
	if (!spec || strlen(spec) != VEX_PACK_LEASE_ID_BYTES * 2)
		return -1;
	return hex_to_bytes(lease_id, spec, VEX_PACK_LEASE_ID_BYTES);
}

static void format_lease_id(const unsigned char lease_id[VEX_PACK_LEASE_ID_BYTES],
			    char out[VEX_PACK_LEASE_ID_BYTES * 2 + 1])
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < VEX_PACK_LEASE_ID_BYTES; i++) {
		out[i * 2] = hex[lease_id[i] >> 4];
		out[i * 2 + 1] = hex[lease_id[i] & 0x0f];
	}
	out[VEX_PACK_LEASE_ID_BYTES * 2] = '\0';
}

int cmd_main(int argc, const char **argv)
{
	const char *git_dir = NULL;
	const char *pin_spec = NULL;
	const char *unpin_spec = NULL;
	const char *lease_current_spec = NULL;
	const char *release_lease_spec = NULL;
	const char *grace_spec = NULL;
	const char *tombstone_spec = NULL;
	const char *reason;
	struct repository repo;
	struct vex_store *store = NULL;
	struct vex_sweep_plan plan;
	struct vex_sweep_result sweep;
	struct vex_pack_artifact artifact;
	struct vex_pack_lease lease = { 0 };
	struct object_id oid;
	int run_sweep = 0;
	struct option options[] = {
		OPT_STRING(0, "git-dir", &git_dir, "path",
			   "thin Vex bare repository"),
		OPT_STRING(0, "pin", &pin_spec, "<oid>:<reason>",
			   "add a durable Vex sweep root"),
		OPT_STRING(0, "unpin", &unpin_spec, "<oid>:<reason>",
			   "remove a durable Vex sweep root"),
		OPT_STRING(0, "lease-current-pack", &lease_current_spec, "seconds",
			   "retain the current immutable pack for a bounded interval"),
		OPT_STRING(0, "release-pack-lease", &release_lease_spec, "lease-id",
			   "release a prior immutable-pack lease"),
		OPT_BOOL(0, "sweep", &run_sweep,
			 "run the grace-period-aware destructive Vex sweep"),
		OPT_STRING(0, "grace-seconds", &grace_spec, "seconds",
			   "sealed-generation grace before recording a sweep tombstone"),
		OPT_STRING(0, "tombstone-seconds", &tombstone_spec, "seconds",
			   "durable tombstone delay before metadata and S3 deletion"),
		OPT_END(),
	};
	char oid_hex[GIT_MAX_HEXSZ + 1];
	char lease_hex[VEX_PACK_LEASE_ID_BYTES * 2 + 1];
	unsigned int lease_ttl;
	unsigned int grace_seconds = VEX_STORE_SWEEP_DEFAULT_GRACE_SECONDS;
	unsigned int tombstone_seconds = VEX_STORE_SWEEP_DEFAULT_TOMBSTONE_SECONDS;
	unsigned int actions;
	int allow_short_sweep =
		git_env_bool("GIT_VEX_TEST_ALLOW_SHORT_SWEEP_GRACE", 0) == 1;
	int ret;

	memset(&repo, 0, sizeof(repo));
	argc = parse_options(argc, argv, NULL, options, sweep_plan_usage, 0);
	actions = !!pin_spec + !!unpin_spec + !!lease_current_spec +
		!!release_lease_spec + !!run_sweep;
	if (argc || !git_dir || actions > 1 ||
	    ((grace_spec || tombstone_spec) && !run_sweep))
		usage_with_options(sweep_plan_usage, options);
	if (repo_init(&repo, git_dir, NULL))
		die("could not initialize repository '%s'", git_dir);
	if (!repo_uses_vex_storage(&repo))
		die("'%s' is not a Vex storage repository", git_dir);
	ret = vex_store_open_for_repository(&repo, git_dir, &store);
	if (ret)
		die("could not open Vex store: %s", vex_store_result_string(ret));
	if (pin_spec || unpin_spec) {
		if (parse_pin_spec(pin_spec ? pin_spec : unpin_spec, repo.hash_algo,
				   &oid, &reason))
			die("pin specification must be <full-object-id>:<nonempty-reason>");
		if (pin_spec)
			ret = vex_store_pin_object(store, &oid, reason);
		else
			ret = vex_store_unpin_object(store, &oid, reason);
		if (ret)
			die("could not %s Vex object pin: %s",
			    pin_spec ? "add" : "remove", vex_store_result_string(ret));
		hash_to_hex_algop_r(oid_hex, oid.hash, repo.hash_algo);
		printf("vex sweep pin: action=%s oid=%s reason=%s\n",
		       pin_spec ? "added" : "removed", oid_hex, reason);
	} else if (lease_current_spec) {
		if (parse_lease_ttl(lease_current_spec, &lease_ttl))
			die("pack lease TTL must be an integer from 1 through 86400 seconds");
		ret = vex_store_current_pack(store, &artifact);
		if (ret)
			die("could not select current Vex pack: %s", vex_store_result_string(ret));
		ret = vex_store_pack_lease_acquire(store, &artifact, lease_ttl, &lease);
		if (ret)
			die("could not acquire Vex pack lease: %s", vex_store_result_string(ret));
		format_lease_id(lease.id, lease_hex);
		hash_to_hex_algop_r(oid_hex, artifact.pack_id.hash, repo.hash_algo);
		printf("vex pack lease: action=acquired lease=%s pack=%s expires_in=%u\n",
		       lease_hex, oid_hex, lease_ttl);
	} else if (release_lease_spec) {
		if (parse_lease_id(release_lease_spec, lease.id))
			die("pack lease ID must be exactly %u hexadecimal characters",
			    VEX_PACK_LEASE_ID_BYTES * 2);
		lease.active = 1;
		ret = vex_store_pack_lease_release(store, &lease);
		if (ret)
			die("could not release Vex pack lease: %s", vex_store_result_string(ret));
		printf("vex pack lease: action=released lease=%s\n", release_lease_spec);
	} else if (run_sweep) {
		if ((grace_spec && parse_sweep_seconds(grace_spec, &grace_seconds,
						      allow_short_sweep)) ||
		    (tombstone_spec && parse_sweep_seconds(tombstone_spec,
							  &tombstone_seconds,
							  allow_short_sweep)))
			die("sweep grace values must be 60 through %u seconds (or a nonzero test value with GIT_VEX_TEST_ALLOW_SHORT_SWEEP_GRACE=1)",
			    VEX_STORE_SWEEP_MAX_SECONDS);
		ret = vex_store_sweep(store, grace_seconds, tombstone_seconds, &sweep);
		printf("vex sweep: grace_seconds=%u tombstone_seconds=%u "
		       "published=%" PRIu64 " ref_roots=%" PRIu64
		       " pin_roots=%" PRIu64 " pack_roots=%" PRIu64
		       " lease_roots=%" PRIu64 " open_epoch_roots=%" PRIu64
		       " reachable=%" PRIu64 " sealed_candidates=%" PRIu64
		       " grace_candidates=%" PRIu64 " tombstones_marked=%" PRIu64
		       " tombstones_cancelled=%" PRIu64 " metadata_deleted=%" PRIu64
		       " blobs_deleted=%" PRIu64 " blobs_pending=%" PRIu64 "\n",
		       grace_seconds, tombstone_seconds, sweep.plan.published_objects,
		       sweep.plan.ref_roots, sweep.plan.pin_roots, sweep.plan.pack_roots,
		       sweep.plan.lease_roots, sweep.plan.open_epoch_roots,
		       sweep.plan.reachable_objects, sweep.plan.sealed_candidates,
		       sweep.grace_candidates, sweep.tombstones_marked,
		       sweep.tombstones_cancelled, sweep.metadata_deleted,
		       sweep.blobs_deleted, sweep.blobs_pending);
		if (ret)
			die("Vex destructive sweep left retryable cleanup: %s",
			    vex_store_result_string(ret));
	} else {
		ret = vex_store_sweep_plan(store, &plan);
		if (ret)
			die("could not build Vex sweep plan: %s", vex_store_result_string(ret));
		printf("vex sweep plan: published=%" PRIu64
		       " ref_roots=%" PRIu64 " pin_roots=%" PRIu64
		       " pack_roots=%" PRIu64 " lease_roots=%" PRIu64
		       " open_epoch_roots=%" PRIu64
		       " reachable=%" PRIu64 " sealed_candidates=%" PRIu64 "\n",
		       plan.published_objects, plan.ref_roots, plan.pin_roots,
		       plan.pack_roots, plan.lease_roots, plan.open_epoch_roots,
		       plan.reachable_objects, plan.sealed_candidates);
	}
	vex_store_close(store);
	repo_clear(&repo);
	return 0;
}

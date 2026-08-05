#include "git-compat-util.h"
#include "hex.h"
#include "odb/source-vex.h"
#include "parse-options.h"
#include "repository.h"
#include "vex/store.h"

static const char * const sweep_plan_usage[] = {
	"vex-sweep-plan --git-dir=<vex-bare-repository> [--pin=<oid>:<reason> | --unpin=<oid>:<reason>]",
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

int cmd_main(int argc, const char **argv)
{
	const char *git_dir = NULL;
	const char *pin_spec = NULL;
	const char *unpin_spec = NULL;
	const char *reason;
	struct repository repo;
	struct vex_store *store = NULL;
	struct vex_sweep_plan plan;
	struct object_id oid;
	struct option options[] = {
		OPT_STRING(0, "git-dir", &git_dir, "path",
			   "thin Vex bare repository"),
		OPT_STRING(0, "pin", &pin_spec, "<oid>:<reason>",
			   "add a durable Vex sweep root"),
		OPT_STRING(0, "unpin", &unpin_spec, "<oid>:<reason>",
			   "remove a durable Vex sweep root"),
		OPT_END(),
	};
	char oid_hex[GIT_MAX_HEXSZ + 1];
	int ret;

	memset(&repo, 0, sizeof(repo));
	argc = parse_options(argc, argv, NULL, options, sweep_plan_usage, 0);
	if (argc || !git_dir || (!!pin_spec == !!unpin_spec && (pin_spec || unpin_spec)))
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
	} else {
		ret = vex_store_sweep_plan(store, &plan);
		if (ret)
			die("could not build Vex sweep plan: %s", vex_store_result_string(ret));
		printf("vex sweep plan: published=%" PRIu64
		       " ref_roots=%" PRIu64 " pin_roots=%" PRIu64
		       " reachable=%" PRIu64 " sealed_candidates=%" PRIu64 "\n",
		       plan.published_objects, plan.ref_roots, plan.pin_roots,
		       plan.reachable_objects, plan.sealed_candidates);
	}
	vex_store_close(store);
	repo_clear(&repo);
	return 0;
}

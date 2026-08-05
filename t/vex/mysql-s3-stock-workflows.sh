#!/usr/bin/env bash

# Compose P0 workflow gate: two unmodified host Git clients diverge after an
# initial main push, publish and rebase a feature, force-push it with an exact
# lease, and integrate it with a no-fast-forward merge.  The final fresh clone
# and shared reconciliation prove that the Vex MySQL metadata and S3 bytes
# agree with the advertised main tip.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# Git's test harness exports build-local paths.  This data-plane workflow must
# use the host's stock Git client for every network operation.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-stock-workflows.sh' >&2
	exit 2
fi

up_args=()
case "${VEX_COMPOSE_NO_BUILD:-}" in
1|true|yes) up_args+=(--no-build) ;;
esac
"$root/scripts/vex/up.sh" "${up_args[@]}" >/dev/null
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"
set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

ca_file="$docker_dir/certs/rootCA.pem"
health=$(curl --fail --silent --show-error --cacert "$ca_file" \
	"https://localhost:${GIT_HTTPS_PORT}/healthz")
case "$health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf 'vex mysql-s3 workflows: endpoint did not report active MySQL+S3 storage: %s\n' "$health" >&2
	exit 1
	;;
esac

stock_git=/usr/bin/git
[ -x "$stock_git" ] || {
	printf '%s\n' 'vex mysql-s3 workflows: /usr/bin/git is required for this stock-client gate' >&2
	exit 127
}

run_id="$(date +%s)-$$"
repo_name="e2e/stock-workflows-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-stock-workflows.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

configure_client() {
	"$stock_git" -C "$1" config user.name vex-mysql-s3-stock-workflows
	"$stock_git" -C "$1" config user.email vex-mysql-s3-stock-workflows@example.invalid
}

# Bootstrap the only shared ancestor for the two subsequent working copies.
bootstrap="$scratch/bootstrap"
"$stock_git" init -q "$bootstrap"
configure_client "$bootstrap"
printf 'Vex MySQL+S3 stock workflow base\n' >"$bootstrap/README"
"$stock_git" -C "$bootstrap" add README
"$stock_git" -C "$bootstrap" -c commit.gpgSign=false commit -q -m 'Vex MySQL+S3 workflow base'
"$stock_git" -C "$bootstrap" branch -M main
initial_tip=$("$stock_git" -C "$bootstrap" rev-parse HEAD)
"$stock_git" -C "$bootstrap" remote add origin "$origin"
git_https -C "$bootstrap" push -q -u origin HEAD:refs/heads/main

# Both clients start from the same advertised main tip.  Giving clone an
# explicit branch avoids depending on the empty server repository's HEAD.
feature_client="$scratch/feature-client"
main_client="$scratch/main-client"
git_https clone -q --branch main "$origin" "$feature_client"
git_https clone -q --branch main "$origin" "$main_client"
configure_client "$feature_client"
configure_client "$main_client"
[ "$("$stock_git" -C "$feature_client" rev-parse HEAD)" = "$initial_tip" ] || {
	printf '%s\n' 'vex mysql-s3 workflows: feature client did not start at initial main' >&2
	exit 1
}
[ "$("$stock_git" -C "$main_client" rev-parse HEAD)" = "$initial_tip" ] || {
	printf '%s\n' 'vex mysql-s3 workflows: main client did not start at initial main' >&2
	exit 1
}

# Publish a feature from one side of the divergence.
"$stock_git" -C "$feature_client" switch -q -c feature
printf 'Vex MySQL+S3 feature before rebase\n' >"$feature_client/feature.txt"
"$stock_git" -C "$feature_client" add feature.txt
"$stock_git" -C "$feature_client" -c commit.gpgSign=false commit -q -m 'feature before rebase'
feature_before_rebase=$("$stock_git" -C "$feature_client" rev-parse HEAD)
git_https -C "$feature_client" push -q -u origin HEAD:refs/heads/feature
feature_remote_before_rebase=$(git_https ls-remote "$origin" refs/heads/feature | awk 'NR == 1 { print $1 }')
[ "$feature_remote_before_rebase" = "$feature_before_rebase" ] || {
	printf 'vex mysql-s3 workflows: feature push advertised %s, expected %s\n' \
		"$feature_remote_before_rebase" "$feature_before_rebase" >&2
	exit 1
}

# Independently advance main from the other clone, making feature stale.
printf 'Vex MySQL+S3 main advance\n' >"$main_client/main.txt"
"$stock_git" -C "$main_client" add main.txt
"$stock_git" -C "$main_client" -c commit.gpgSign=false commit -q -m 'main advances independently'
main_advance_tip=$("$stock_git" -C "$main_client" rev-parse HEAD)
git_https -C "$main_client" push -q origin HEAD:refs/heads/main
remote_main_after_advance=$(git_https ls-remote "$origin" refs/heads/main | awk 'NR == 1 { print $1 }')
[ "$remote_main_after_advance" = "$main_advance_tip" ] || {
	printf 'vex mysql-s3 workflows: main advance advertised %s, expected %s\n' \
		"$remote_main_after_advance" "$main_advance_tip" >&2
	exit 1
}

# Rebase the already-published feature onto main.  The old remote feature tip
# must no longer be an ancestor, so a plain push would be rejected; the exact
# lease below also protects against a concurrent writer changing that ref.
git_https -C "$feature_client" fetch -q origin \
	'+refs/heads/main:refs/remotes/origin/main'
"$stock_git" -C "$feature_client" -c commit.gpgSign=false rebase -q \
	refs/remotes/origin/main
feature_rebased_tip=$("$stock_git" -C "$feature_client" rev-parse HEAD)
[ "$feature_rebased_tip" != "$feature_before_rebase" ] || {
	printf '%s\n' 'vex mysql-s3 workflows: rebase did not rewrite the feature commit' >&2
	exit 1
}
"$stock_git" -C "$feature_client" merge-base --is-ancestor \
	"$main_advance_tip" "$feature_rebased_tip" || {
	printf '%s\n' 'vex mysql-s3 workflows: rebased feature does not contain the main advance' >&2
	exit 1
}
if "$stock_git" -C "$feature_client" merge-base --is-ancestor \
	"$feature_remote_before_rebase" "$feature_rebased_tip"
then
	printf '%s\n' 'vex mysql-s3 workflows: stale feature unexpectedly remains an ancestor after rebase' >&2
	exit 1
fi
git_https -C "$feature_client" push -q \
	"--force-with-lease=refs/heads/feature:${feature_remote_before_rebase}" \
	origin HEAD:refs/heads/feature
remote_feature_after_rebase=$(git_https ls-remote "$origin" refs/heads/feature | awk 'NR == 1 { print $1 }')
[ "$remote_feature_after_rebase" = "$feature_rebased_tip" ] || {
	printf 'vex mysql-s3 workflows: leased feature push advertised %s, expected %s\n' \
		"$remote_feature_after_rebase" "$feature_rebased_tip" >&2
	exit 1
}

# Integrate the rebased feature with an explicit merge commit, rather than a
# fast-forward update.  This keeps the full everyday workflow in the P0 gate.
git_https -C "$main_client" fetch -q origin \
	'+refs/heads/feature:refs/remotes/origin/feature'
"$stock_git" -C "$main_client" -c commit.gpgSign=false merge --no-ff -q \
	-m 'integrate rebased feature with no fast forward' refs/remotes/origin/feature
merge_tip=$("$stock_git" -C "$main_client" rev-parse HEAD)
merge_parent_count=$("$stock_git" -C "$main_client" show -s --format=%P "$merge_tip" | wc -w | tr -d ' ')
[ "$merge_parent_count" = 2 ] || {
	printf 'vex mysql-s3 workflows: --no-ff integration produced %s parents, expected 2\n' \
		"$merge_parent_count" >&2
	exit 1
}
git_https -C "$main_client" push -q origin HEAD:refs/heads/main

remote_main=$(git_https ls-remote "$origin" refs/heads/main | awk 'NR == 1 { print $1 }')
remote_feature=$(git_https ls-remote "$origin" refs/heads/feature | awk 'NR == 1 { print $1 }')
[ "$remote_main" = "$merge_tip" ] || {
	printf 'vex mysql-s3 workflows: final main advertised %s, expected %s\n' \
		"$remote_main" "$merge_tip" >&2
	exit 1
}
[ "$remote_feature" = "$feature_rebased_tip" ] || {
	printf 'vex mysql-s3 workflows: final feature advertised %s, expected %s\n' \
		"$remote_feature" "$feature_rebased_tip" >&2
	exit 1
}

# A third stock client must independently receive both refs and have a sound
# object graph; no working-copy state from either writer is reused here.
verifier="$scratch/verifier"
git_https clone -q --no-checkout "$origin" "$verifier"
git_https -C "$verifier" fetch -q origin \
	'+refs/heads/main:refs/remotes/origin/main' \
	'+refs/heads/feature:refs/remotes/origin/feature'
verified_main=$("$stock_git" -C "$verifier" rev-parse refs/remotes/origin/main)
verified_feature=$("$stock_git" -C "$verifier" rev-parse refs/remotes/origin/feature)
[ "$verified_main" = "$merge_tip" ] || {
	printf 'vex mysql-s3 workflows: fresh clone main is %s, expected %s\n' \
		"$verified_main" "$merge_tip" >&2
	exit 1
}
[ "$verified_feature" = "$feature_rebased_tip" ] || {
	printf 'vex mysql-s3 workflows: fresh clone feature is %s, expected %s\n' \
		"$verified_feature" "$feature_rebased_tip" >&2
	exit 1
}
"$stock_git" -C "$verifier" fsck --no-progress >/dev/null

# The shared reconciler verifies main's Vex ref row, published object row,
# sealed generation, and canonical S3 key.  It is kept separate so this
# workflow gate exercises exactly the same storage invariant as the ramp.
reconcile_json="$scratch/reconcile.json"
GIT_VEX_PUSH_STRESS_ORIGIN="$origin" \
GIT_VEX_PUSH_STRESS_BRANCH=main \
GIT_VEX_PUSH_STRESS_EXPECTED_TIP="$merge_tip" \
	"$root/t/vex/push-stress/compose-reconcile.sh" >"$reconcile_json"
grep -F '"canonical_blob_recon":"pass"' "$reconcile_json" >/dev/null || {
	printf '%s\n' 'vex mysql-s3 workflows: final MySQL/S3 reconciliation did not report pass' >&2
	exit 1
}

final_health=$(curl --fail --silent --show-error --cacert "$ca_file" \
	"https://localhost:${GIT_HTTPS_PORT}/healthz")
case "$final_health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf 'vex mysql-s3 workflows: gateway was unhealthy after the run: %s\n' "$final_health" >&2
	exit 1
	;;
esac

printf 'vex mysql-s3 workflows: PASS stock-Git HTTPS initial main, diverged feature/main, rebase, exact --force-with-lease, --no-ff merge, fresh clone/fsck/ref agreement, and MySQL/S3 reconciliation; main=%s feature=%s\n' \
	"$merge_tip" "$feature_rebased_tip"

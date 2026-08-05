#!/usr/bin/env bash

# Measure a supplied Vex HTTPS repository against the same Git graph on local
# disk.  This is deliberately a measurement harness, not an L4 acceptance
# gate: it records the observed ratio without applying an SLO or pass/fail
# threshold.
set -euo pipefail

usage() {
	cat <<'EOF'
usage: scripts/vex/bench-clone.sh --origin HTTPS_URL --baseline LOCAL_REPOSITORY \
       --branch BRANCH --output RESULT.json [--runs N] [--git-config FILE]

Run N fresh stock-Git --no-checkout clones from a Vex HTTPS repository and
from the matching local-disk Git repository.  RESULT.json records p50/p90
latencies and Vex-to-local ratios; it intentionally makes no L4 success claim.

--git-config supplies a private Git config file, for example one containing
http.sslCAInfo and an HTTP authorization header for a local Compose endpoint.
EOF
}

die() {
	printf 'vex clone benchmark: %s\n' "$*" >&2
	exit 2
}

origin=''
baseline=''
branch=''
output=''
runs=5
git_config=''

while [ "$#" -gt 0 ]; do
	case "$1" in
	--origin)
		[ "$#" -ge 2 ] || die '--origin requires a value'
		origin=$2
		shift 2
		;;
	--origin=*)
		origin=${1#--origin=}
		shift
		;;
	--baseline)
		[ "$#" -ge 2 ] || die '--baseline requires a value'
		baseline=$2
		shift 2
		;;
	--baseline=*)
		baseline=${1#--baseline=}
		shift
		;;
	--branch)
		[ "$#" -ge 2 ] || die '--branch requires a value'
		branch=$2
		shift 2
		;;
	--branch=*)
		branch=${1#--branch=}
		shift
		;;
	--output)
		[ "$#" -ge 2 ] || die '--output requires a path'
		output=$2
		shift 2
		;;
	--output=*)
		output=${1#--output=}
		shift
		;;
	--runs)
		[ "$#" -ge 2 ] || die '--runs requires an integer'
		runs=$2
		shift 2
		;;
	--runs=*)
		runs=${1#--runs=}
		shift
		;;
	--git-config)
		[ "$#" -ge 2 ] || die '--git-config requires a path'
		git_config=$2
		shift 2
		;;
	--git-config=*)
		git_config=${1#--git-config=}
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		die "unknown option: $1"
		;;
	esac
done

[ -n "$origin" ] || die '--origin is required'
[ -n "$baseline" ] || die '--baseline is required'
[ -n "$branch" ] || die '--branch is required'
[ -n "$output" ] || die '--output is required'
case "$origin" in
https://*) ;;
*) die "--origin must be an HTTPS URL: $origin" ;;
esac
case "$runs" in
''|*[!0-9]*) die "--runs must be a positive integer: $runs" ;;
esac
if [ "$runs" -lt 1 ] || [ "$runs" -gt 1000 ]; then
	die "--runs must be between 1 and 1000: $runs"
fi
[ -d "$baseline" ] || die "--baseline is not a local directory: $baseline"
baseline=$(CDPATH= cd -- "$baseline" && pwd -P)
if [ -n "$git_config" ]; then
	[ -f "$git_config" ] || die "--git-config is not a file: $git_config"
	git_config=$(CDPATH= cd -- "$(dirname -- "$git_config")" && pwd -P)/$(basename -- "$git_config")
	export GIT_CONFIG_GLOBAL="$git_config"
	export GIT_CONFIG_NOSYSTEM=1
fi
case "$output" in
/*) ;;
*) output="$PWD/$output" ;;
esac
mkdir -p -- "$(dirname -- "$output")"

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git || true)
fi
[ -n "$stock_git" ] && [ -x "$stock_git" ] || {
	printf 'vex clone benchmark: stock Git executable is not available: %s\n' "$stock_git" >&2
	exit 127
}
if ! command -v python3 >/dev/null 2>&1; then
	printf '%s\n' 'vex clone benchmark: python3 is required' >&2
	exit 127
fi

# Preserve ordinary Git configuration (including user-provided credentials),
# but never let the git/git test harness redirect this stock client to build
# helpers or an unrelated object directory.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CEILING_DIRECTORIES

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-clone-bench.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

python3 - "$stock_git" "$origin" "$baseline" "$branch" "$runs" "$output" "$scratch" <<'PY'
import json
import math
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit


stock_git, origin, baseline, branch, runs_text, output_text, scratch_text = sys.argv[1:]
runs = int(runs_text)
output = Path(output_text)
scratch = Path(scratch_text)
refname = "refs/heads/{}".format(branch)


class BenchmarkError(RuntimeError):
	pass


def redact_url(value: str) -> str:
	parsed = urlsplit(value)
	host = parsed.hostname or ""
	if ":" in host and not host.startswith("["):
		host = "[{}]".format(host)
	if parsed.port is not None:
		host = "{}:{}".format(host, parsed.port)
	# A Git endpoint does not need a query or fragment.  Leaving either in the
	# persisted artifact risks retaining a bearer-style query credential.
	return urlunsplit((parsed.scheme, host, parsed.path, "", ""))


origin_display = redact_url(origin)


def run_checked(args: list[str], action: str, *, capture: bool = False) -> str:
	completed = subprocess.run(
		args,
		stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
		stderr=subprocess.PIPE,
		text=True,
		check=False,
	)
	if completed.returncode:
		stderr = completed.stderr.strip().replace(origin, origin_display)
		raise BenchmarkError(
			"{} failed with exit {}{}".format(
				action,
				completed.returncode,
				": " + stderr if stderr else "",
			)
		)
	return completed.stdout if capture else ""


def remote_tip() -> str:
	result = run_checked(
		[stock_git, "ls-remote", origin, refname], "read Vex benchmark ref", capture=True
	).strip().splitlines()
	if len(result) != 1:
		raise BenchmarkError(
			"Vex benchmark ref {} returned {} rows".format(refname, len(result))
		)
	fields = result[0].split()
	if len(fields) != 2 or fields[1] != refname:
		raise BenchmarkError("Vex benchmark ref response is malformed: {}".format(result[0]))
	return fields[0]


def baseline_tip() -> str:
	return run_checked(
		[stock_git, "-C", baseline, "rev-parse", "--verify", refname + "^{commit}"],
		"read local baseline ref",
		capture=True,
	).strip()


def percentile(samples: list[float], fraction: float) -> float:
	ordered = sorted(samples)
	return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def clone_once(kind: str, source: str, iteration: int, expected_tip: str) -> dict[str, object]:
	destination = scratch / "{}-{}".format(kind, iteration + 1)
	command = [stock_git, "clone", "--quiet", "--no-checkout", "--branch", branch]
	if kind == "local_disk":
		# Avoid hardlinks/references so the baseline measures Git reading the
		# repository over its local transport instead of a metadata shortcut.
		command.append("--no-local")
	command.extend([source, str(destination)])
	started = time.perf_counter_ns()
	run_checked(command, "{} clone {}".format(kind, iteration + 1))
	elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
	try:
		actual_tip = run_checked(
			[stock_git, "-C", str(destination), "rev-parse", "HEAD"],
			"verify {} clone {} tip".format(kind, iteration + 1),
			capture=True,
		).strip()
		if actual_tip != expected_tip:
			raise BenchmarkError(
				"{} clone {} reached {}, expected {}".format(
					kind, iteration + 1, actual_tip, expected_tip
				)
			)
		run_checked(
			[stock_git, "-C", str(destination), "fsck", "--no-progress"],
			"fsck {} clone {}".format(kind, iteration + 1),
		)
	finally:
		shutil.rmtree(destination, ignore_errors=True)
	return {"iteration": iteration + 1, "elapsed_ms": round(elapsed_ms, 3)}


try:
	expected_tip = baseline_tip()
	if remote_tip() != expected_tip:
		raise BenchmarkError(
			"local baseline {} and Vex HTTPS {} do not name the same {}".format(
				baseline, origin_display, refname
			)
		)

	samples = {"vex_https": [], "local_disk": []}
	sample_order: list[list[str]] = []
	for iteration in range(runs):
		order = ["vex_https", "local_disk"]
		if iteration % 2:
			order.reverse()
		sample_order.append(order)
		for kind in order:
			source = origin if kind == "vex_https" else baseline
			samples[kind].append(clone_once(kind, source, iteration, expected_tip))

	if remote_tip() != expected_tip:
		raise BenchmarkError(
			"Vex benchmark ref {} changed while samples were collected".format(refname)
		)

	def summary(values: list[dict[str, object]]) -> dict[str, object]:
		latencies = [float(value["elapsed_ms"]) for value in values]
		return {
			"samples": values,
			"p50_ms": round(percentile(latencies, 0.50), 3),
			"p90_ms": round(percentile(latencies, 0.90), 3),
		}

	vex_https = summary(samples["vex_https"])
	local_disk = summary(samples["local_disk"])
	local_p50 = float(local_disk["p50_ms"])
	local_p90 = float(local_disk["p90_ms"])
	if local_p50 <= 0 or local_p90 <= 0:
		raise BenchmarkError("local-disk percentile was not positive")
	payload = {
		"schema_version": 1,
		"benchmark": "vex-stock-git-https-clone-vs-local-disk",
		"measurement_only": True,
		"l4_success_claim": False,
		"stock_git": stock_git,
		"origin": origin_display,
		"baseline": baseline,
		"branch": branch,
		"runs": runs,
		"ref": {
			"name": refname,
			"oid": expected_tip,
			"stable_across_measurement": True,
		},
		"clone_mode": {
			"vex_https": "git clone --no-checkout --branch",
			"local_disk": "git clone --no-local --no-checkout --branch",
		},
		"sample_order": sample_order,
		"vex_https": vex_https,
		"local_disk": local_disk,
		"ratio_vex_to_local_disk": {
			"p50": round(float(vex_https["p50_ms"]) / local_p50, 4),
			"p90": round(float(vex_https["p90_ms"]) / local_p90, 4),
		},
	}
	temporary = output.with_name(output.name + ".tmp-{}".format(os.getpid()))
	try:
		temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
		os.replace(temporary, output)
	finally:
		temporary.unlink(missing_ok=True)
except (BenchmarkError, OSError) as error:
	print("vex clone benchmark: {}".format(error), file=sys.stderr)
	raise SystemExit(1)
PY

printf 'vex clone benchmark: result=%s\n' "$output"

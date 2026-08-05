#!/usr/bin/env bash

# Measure a stock-Git Vex HTTPS clone against a matched disk control. This is
# deliberately an evidence harness: it records every warmup and trial, the
# source/Compose identity, and the policy result. Passing its individual gates
# never claims that L4 is complete; the git/git fixture and the other L4
# functional gates remain independently required.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

usage() {
	cat <<'EOF'
usage: scripts/vex/bench-clone.sh --origin HTTPS_URL \
       (--disk-origin HTTPS_URL | --baseline LOCAL_REPOSITORY) \
       --branch BRANCH --output RESULT.json [options]

Run fresh stock-Git --no-checkout clones from a Vex HTTPS repository and a
matched disk control. --disk-origin is reserved for the L4-shaped control:
it must be exactly
https://<same-host-and-port>/__vex_bench_disk__/git-git-disk.git. --baseline
is retained for small local measurement smoke tests.

Core options:
  --runs N                         measured clones per source (default: 5)
  --warmups N                      recorded warmups per source (default: 1)
  --git-config FILE                private Git config (CA/auth, never persisted)
  --compose-file FILE              Compose file to identify in the artifact
  --compose-project NAME           Compose project identity (default: vex)
  --seed-manifest FILE             immutable seed evidence to hash and attach
  --fixture NAME                   fixture identity, e.g. git-git

Gate options:
  --require-disk-gate              require fixed same-frontend HTTPS disk and Vex p50 <= 1.5x
  --max-disk-ratio R               disk threshold (default: 1.5)
  --require-stable                 fail if fewer than five trials or CV > 15%
  --max-cv-percent P               CV threshold (default: 15)
  --include-github                 also measure GitHub (or BENCH_INCLUDE_GITHUB=1)
  --github-origin HTTPS_URL        GitHub control (default: https://github.com/git/git)
  --require-github                 require GitHub gate (also requires disk + stability)
  --max-github-ratio R             GitHub threshold (default: 1.5)

Fetch-after-push (mutates --branch briefly, then restores it by lease):
  --fetch-after-push               measure N small pushes and reader fetches
  --fetch-runs N                   fetch trials per source (default: --runs)
  --require-fetch-after-push-gate  fail if Vex fetch p50 > 2x disk p50
  --max-fetch-ratio R              fetch threshold (default: 2.0)
  --no-restore-fetch-branch        leave the benchmark commits on --branch

The normal L4 invocation uses a git/git seed manifest, an HTTPS disk control,
--require-disk-gate, --require-github, and --require-fetch-after-push-gate.
EOF
}

die() {
	printf 'vex clone benchmark: %s\n' "$*" >&2
	exit 2
}

origin=''
baseline=''
disk_origin=''
branch=''
output=''
runs=5
warmups=1
git_config=''
compose_file="$repo_root/vex/docker/compose.yaml"
compose_project="${COMPOSE_PROJECT_NAME:-vex}"
seed_manifest=''
fixture='unspecified'
include_github=0
github_origin="${BENCH_GITHUB_ORIGIN:-https://github.com/git/git}"
require_disk_gate=0
require_github=0
require_stable=0
max_disk_ratio=1.5
max_github_ratio=1.5
max_cv_percent=15
fetch_after_push=0
fetch_runs=''
require_fetch_gate=0
max_fetch_ratio=2.0
restore_fetch_branch=1

case "${BENCH_INCLUDE_GITHUB:-}" in
1|true|yes|on) include_github=1 ;;
esac
case "${BENCH_REQUIRE_DISK_GATE:-}" in
1|true|yes|on) require_disk_gate=1 ;;
esac
case "${BENCH_REQUIRE_GITHUB:-}" in
1|true|yes|on) require_github=1 ;;
esac
case "${BENCH_REQUIRE_STABLE:-}" in
1|true|yes|on) require_stable=1 ;;
esac

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
	--disk-origin)
		[ "$#" -ge 2 ] || die '--disk-origin requires a value'
		disk_origin=$2
		shift 2
		;;
	--disk-origin=*)
		disk_origin=${1#--disk-origin=}
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
	--warmups)
		[ "$#" -ge 2 ] || die '--warmups requires an integer'
		warmups=$2
		shift 2
		;;
	--warmups=*)
		warmups=${1#--warmups=}
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
	--compose-file)
		[ "$#" -ge 2 ] || die '--compose-file requires a path'
		compose_file=$2
		shift 2
		;;
	--compose-file=*)
		compose_file=${1#--compose-file=}
		shift
		;;
	--compose-project)
		[ "$#" -ge 2 ] || die '--compose-project requires a name'
		compose_project=$2
		shift 2
		;;
	--compose-project=*)
		compose_project=${1#--compose-project=}
		shift
		;;
	--seed-manifest)
		[ "$#" -ge 2 ] || die '--seed-manifest requires a path'
		seed_manifest=$2
		shift 2
		;;
	--seed-manifest=*)
		seed_manifest=${1#--seed-manifest=}
		shift
		;;
	--fixture)
		[ "$#" -ge 2 ] || die '--fixture requires a name'
		fixture=$2
		shift 2
		;;
	--fixture=*)
		fixture=${1#--fixture=}
		shift
		;;
	--include-github)
		include_github=1
		shift
		;;
	--github-origin)
		[ "$#" -ge 2 ] || die '--github-origin requires a URL'
		github_origin=$2
		shift 2
		;;
	--github-origin=*)
		github_origin=${1#--github-origin=}
		shift
		;;
	--require-disk-gate)
		require_disk_gate=1
		shift
		;;
	--max-disk-ratio)
		[ "$#" -ge 2 ] || die '--max-disk-ratio requires a number'
		max_disk_ratio=$2
		shift 2
		;;
	--max-disk-ratio=*)
		max_disk_ratio=${1#--max-disk-ratio=}
		shift
		;;
	--require-stable)
		require_stable=1
		shift
		;;
	--max-cv-percent)
		[ "$#" -ge 2 ] || die '--max-cv-percent requires a number'
		max_cv_percent=$2
		shift 2
		;;
	--max-cv-percent=*)
		max_cv_percent=${1#--max-cv-percent=}
		shift
		;;
	--require-github)
		require_github=1
		include_github=1
		shift
		;;
	--max-github-ratio)
		[ "$#" -ge 2 ] || die '--max-github-ratio requires a number'
		max_github_ratio=$2
		shift 2
		;;
	--max-github-ratio=*)
		max_github_ratio=${1#--max-github-ratio=}
		shift
		;;
	--fetch-after-push)
		fetch_after_push=1
		shift
		;;
	--fetch-runs)
		[ "$#" -ge 2 ] || die '--fetch-runs requires an integer'
		fetch_runs=$2
		shift 2
		;;
	--fetch-runs=*)
		fetch_runs=${1#--fetch-runs=}
		shift
		;;
	--require-fetch-after-push-gate)
		require_fetch_gate=1
		fetch_after_push=1
		shift
		;;
	--max-fetch-ratio)
		[ "$#" -ge 2 ] || die '--max-fetch-ratio requires a number'
		max_fetch_ratio=$2
		shift 2
		;;
	--max-fetch-ratio=*)
		max_fetch_ratio=${1#--max-fetch-ratio=}
		shift
		;;
	--no-restore-fetch-branch)
		restore_fetch_branch=0
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
[ -n "$branch" ] || die '--branch is required'
[ -n "$output" ] || die '--output is required'
case "$origin" in
https://*) ;;
*) die "--origin must be an HTTPS URL: $origin" ;;
esac
case "$github_origin" in
https://*) ;;
*) die "--github-origin must be an HTTPS URL: $github_origin" ;;
esac
if [ -n "$baseline" ] && [ -n "$disk_origin" ]; then
	die 'use exactly one of --baseline and --disk-origin'
fi
if [ -z "$baseline" ] && [ -z "$disk_origin" ]; then
	die 'one of --baseline and --disk-origin is required'
fi
if [ -n "$baseline" ]; then
	[ -d "$baseline" ] || die "--baseline is not a local directory: $baseline"
	baseline=$(CDPATH= cd -- "$baseline" && pwd -P)
	disk_source=$baseline
	disk_kind=local_path
else
	case "$disk_origin" in
	https://*) ;;
	*) die "--disk-origin must be an HTTPS URL: $disk_origin" ;;
	esac
	disk_source=$disk_origin
	disk_kind=https
fi
if [ -z "$fetch_runs" ]; then
	fetch_runs=$runs
fi
case "$runs" in
''|*[!0-9]*) die "--runs must be a positive integer: $runs" ;;
esac
case "$warmups" in
''|*[!0-9]*) die "--warmups must be a non-negative integer: $warmups" ;;
esac
case "$fetch_runs" in
''|*[!0-9]*) die "--fetch-runs must be a positive integer: $fetch_runs" ;;
esac
if [ "$runs" -lt 1 ] || [ "$runs" -gt 1000 ]; then
	die "--runs must be between 1 and 1000: $runs"
fi
if [ "$warmups" -gt 1000 ]; then
	die "--warmups must be between 0 and 1000: $warmups"
fi
if [ "$fetch_runs" -lt 1 ] || [ "$fetch_runs" -gt 1000 ]; then
	die "--fetch-runs must be between 1 and 1000: $fetch_runs"
fi
if [ -n "$git_config" ]; then
	[ -f "$git_config" ] || die "--git-config is not a file: $git_config"
	git_config=$(CDPATH= cd -- "$(dirname -- "$git_config")" && pwd -P)/$(basename -- "$git_config")
	export GIT_CONFIG_GLOBAL="$git_config"
	export GIT_CONFIG_NOSYSTEM=1
fi
if [ ! -f "$compose_file" ]; then
	die "--compose-file is not a file: $compose_file"
fi
compose_file=$(CDPATH= cd -- "$(dirname -- "$compose_file")" && pwd -P)/$(basename -- "$compose_file")
if [ -n "$seed_manifest" ]; then
	[ -f "$seed_manifest" ] || die "--seed-manifest is not a file: $seed_manifest"
	seed_manifest=$(CDPATH= cd -- "$(dirname -- "$seed_manifest")" && pwd -P)/$(basename -- "$seed_manifest")
fi
case "$output" in
/*) ;;
*) output="$PWD/$output" ;;
esac
mkdir -p -- "$(dirname -- "$output")"

if [ "$require_github" -eq 1 ]; then
	require_disk_gate=1
	require_stable=1
fi
if [ "$require_disk_gate" -eq 1 ] || [ "$require_fetch_gate" -eq 1 ]; then
	require_stable=1
fi

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

python3 - "$stock_git" "$origin" "$disk_source" "$disk_kind" "$branch" \
	"$runs" "$warmups" "$output" "$scratch" "$repo_root" "$compose_file" \
	"$compose_project" "$seed_manifest" "$fixture" "$include_github" \
	"$github_origin" "$require_disk_gate" "$require_github" "$require_stable" \
	"$max_disk_ratio" "$max_github_ratio" "$max_cv_percent" "$fetch_after_push" \
	"$fetch_runs" "$require_fetch_gate" "$max_fetch_ratio" "$restore_fetch_branch" <<'PY'
import hashlib
import json
import math
import os
import platform
import shutil
import socket
import statistics
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional
from urllib.parse import urlsplit, urlunsplit


(
	stock_git,
	origin,
	disk_source,
	disk_kind,
	branch,
	runs_text,
	warmups_text,
	output_text,
	scratch_text,
	repo_root_text,
	compose_file_text,
	compose_project,
	seed_manifest_text,
	fixture,
	include_github_text,
	github_origin,
	require_disk_text,
	require_github_text,
	require_stable_text,
	max_disk_ratio_text,
	max_github_ratio_text,
	max_cv_percent_text,
	fetch_after_push_text,
	fetch_runs_text,
	require_fetch_text,
	max_fetch_ratio_text,
	restore_fetch_text,
) = sys.argv[1:]

runs = int(runs_text)
warmups_count = int(warmups_text)
include_github = include_github_text == "1"
require_disk_gate = require_disk_text == "1"
require_github = require_github_text == "1"
require_stable = require_stable_text == "1"
fetch_after_push = fetch_after_push_text == "1"
fetch_runs = int(fetch_runs_text)
require_fetch_gate = require_fetch_text == "1"
restore_fetch_branch = restore_fetch_text == "1"
max_disk_ratio = float(max_disk_ratio_text)
max_github_ratio = float(max_github_ratio_text)
max_cv_percent = float(max_cv_percent_text)
max_fetch_ratio = float(max_fetch_ratio_text)
output = Path(output_text)
scratch = Path(scratch_text)
repo_root = Path(repo_root_text)
compose_file = Path(compose_file_text)
seed_manifest = Path(seed_manifest_text) if seed_manifest_text else None
refname = "refs/heads/{}".format(branch)


class BenchmarkError(RuntimeError):
	pass


DISK_CONTROL_PATH = "/__vex_bench_disk__/git-git-disk.git"
DISK_CONTROL_TEMPLATE = "https://<same-host-and-port>{}".format(DISK_CONTROL_PATH)


def parse_https_endpoint(value: str, option: str) -> dict[str, object]:
	"""Reject URL forms that can hide a different frontend or credential."""
	try:
		parsed = urlsplit(value)
	except ValueError as error:
		raise BenchmarkError("{} must be a valid HTTPS URL: {}".format(option, error))
	if parsed.scheme != "https" or not parsed.netloc or not parsed.hostname:
		raise BenchmarkError("{} must be a valid HTTPS URL".format(option))
	if parsed.username is not None or parsed.password is not None:
		raise BenchmarkError("{} must not include credentials".format(option))
	if parsed.query:
		raise BenchmarkError("{} must not include a query string".format(option))
	if parsed.fragment:
		raise BenchmarkError("{} must not include a fragment".format(option))
	try:
		explicit_port = parsed.port
	except ValueError as error:
		raise BenchmarkError("{} has an invalid port: {}".format(option, error))
	# urlsplit accepts an empty `host:` port and reports it as None. Do not
	# silently normalize that malformed authority to port 443.
	hostport = parsed.netloc.rsplit("@", 1)[-1]
	if hostport.endswith(":"):
		raise BenchmarkError("{} has an empty port".format(option))
	return {
		"host": parsed.hostname.lower(),
		"port": explicit_port if explicit_port is not None else 443,
		"path": parsed.path,
	}


def format_frontend(endpoint: dict[str, object]) -> str:
	host = str(endpoint["host"])
	if ":" in host:
		host = "[{}]".format(host)
	return "{}:{}".format(host, endpoint["port"])


try:
	origin_endpoint = parse_https_endpoint(origin, "--origin")
	if disk_kind == "https":
		disk_endpoint = parse_https_endpoint(disk_source, "--disk-origin")
		if disk_endpoint["path"] != DISK_CONTROL_PATH:
			raise BenchmarkError(
				"--disk-origin must be exactly {}"
				.format(DISK_CONTROL_TEMPLATE)
			)
		if (
			disk_endpoint["host"] != origin_endpoint["host"]
			or disk_endpoint["port"] != origin_endpoint["port"]
		):
			raise BenchmarkError(
				"--disk-origin must use the same HTTPS frontend as --origin "
				"(expected {}, got {})".format(
					format_frontend(origin_endpoint),
					format_frontend(disk_endpoint),
				)
			)
		disk_control_is_l4_shape = True
	else:
		disk_endpoint = None
		disk_control_is_l4_shape = False
except BenchmarkError as error:
	print("vex clone benchmark: {}".format(error), file=sys.stderr)
	raise SystemExit(2)


def now_utc() -> str:
	return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def redact_url(value: str) -> str:
	parsed = urlsplit(value)
	if not parsed.scheme:
		return value
	host = parsed.hostname or ""
	if ":" in host and not host.startswith("["):
		host = "[{}]".format(host)
	if parsed.port is not None:
		host = "{}:{}".format(host, parsed.port)
	# A Git endpoint does not need a query or fragment. Leaving either in an
	# artifact risks retaining a bearer-style query credential.
	return urlunsplit((parsed.scheme, host, parsed.path, "", ""))


origin_display = redact_url(origin)
disk_display = redact_url(disk_source) if disk_kind == "https" else disk_source
github_display = redact_url(github_origin)
redactions = {
	origin: origin_display,
	disk_source: disk_display,
	github_origin: github_display,
}


def redact_text(value: str) -> str:
	for raw, display in redactions.items():
		value = value.replace(raw, display)
	return value


def command_display(args: list[str]) -> list[str]:
	return [redact_text(str(arg)) for arg in args]


def run_checked(args: list[str], action: str, *, capture: bool = False) -> str:
	completed = subprocess.run(
		args,
		stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
		stderr=subprocess.PIPE,
		text=True,
		check=False,
	)
	if completed.returncode:
		stderr = redact_text(completed.stderr.strip())
		raise BenchmarkError(
			"{} failed with exit {}{}".format(
				action,
				completed.returncode,
				": " + stderr if stderr else "",
			)
		)
	return completed.stdout if capture else ""


def optional_output(args: list[str]) -> Optional[str]:
	try:
		completed = subprocess.run(
			args,
			stdout=subprocess.PIPE,
			stderr=subprocess.DEVNULL,
			text=True,
			check=False,
			timeout=5,
		)
	except (OSError, subprocess.TimeoutExpired):
		return None
	if completed.returncode:
		return None
	return completed.stdout.strip() or None


def sha256_file(path: Path) -> str:
	digest = hashlib.sha256()
	with path.open("rb") as handle:
		for block in iter(lambda: handle.read(1024 * 1024), b""):
			digest.update(block)
	return digest.hexdigest()


def artifact_path(path: Path) -> str:
	try:
		return str(path.resolve().relative_to(repo_root.resolve()))
	except ValueError:
		return path.name


def source_identity() -> dict[str, object]:
	commit = optional_output([stock_git, "-C", str(repo_root), "rev-parse", "HEAD"])
	dirty = optional_output([stock_git, "-C", str(repo_root), "status", "--porcelain"])
	return {
		"repository_root": repo_root.name,
		"git_commit": commit,
		"dirty": bool(dirty) if dirty is not None else None,
	}


def manifest_identity() -> Optional[dict[str, object]]:
	if seed_manifest is None:
		return None
	try:
		parsed = json.loads(seed_manifest.read_text(encoding="utf-8"))
	except (OSError, json.JSONDecodeError) as error:
		raise BenchmarkError("read seed manifest {}: {}".format(artifact_path(seed_manifest), error))
	if not isinstance(parsed, dict):
		raise BenchmarkError("seed manifest {} must be a JSON object".format(artifact_path(seed_manifest)))
	# Keep only reproducibility fields. The seed script owns its schema and may
	# include endpoint or operator details that do not belong in the artifact.
	declared = {}
	for key in ("upstream", "head", "default_branch", "commit_count", "object_count", "pack_bytes", "created_at"):
		value = parsed.get(key)
		if isinstance(value, (str, int, float, bool)) or value is None:
			if key in parsed:
				declared[key] = value
	return {
		"path": artifact_path(seed_manifest),
		"sha256": sha256_file(seed_manifest),
		"declared": declared,
	}


def environment_identity() -> dict[str, object]:
	return {
		"host": socket.gethostname(),
		"platform": platform.platform(),
		"python": sys.version.split()[0],
		"cpu_count": os.cpu_count(),
		"git_version": optional_output([stock_git, "--version"]),
		"docker_compose_version": optional_output(["docker", "compose", "version", "--short"]),
	}


def remote_tip(source: str, label: str) -> str:
	result = run_checked(
		[stock_git, "ls-remote", source, refname],
		"read {} benchmark ref".format(label),
		capture=True,
	).strip().splitlines()
	if len(result) != 1:
		raise BenchmarkError(
			"{} benchmark ref {} returned {} rows".format(label, refname, len(result))
		)
	fields = result[0].split()
	if len(fields) != 2 or fields[1] != refname:
		raise BenchmarkError("{} benchmark ref response is malformed: {}".format(label, redact_text(result[0])))
	return fields[0]


def local_tip(path: str) -> str:
	return run_checked(
		[stock_git, "-C", path, "rev-parse", "--verify", refname + "^{commit}"],
		"read local disk-control ref",
		capture=True,
	).strip()


def source_tip(source: str, kind: str, label: str) -> str:
	if kind == "local_path":
		return local_tip(source)
	return remote_tip(source, label)


def percentile(samples: list[float], fraction: float) -> float:
	ordered = sorted(samples)
	return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def directory_bytes(path: Path) -> int:
	total = 0
	if not path.exists():
		return total
	for root, _, filenames in os.walk(path):
		for filename in filenames:
			try:
				total += (Path(root) / filename).stat().st_size
			except OSError:
				pass
	return total


trials: list[dict[str, object]] = []


def append_trial(record: dict[str, object]) -> None:
	trials.append(record)


def clone_command(source: str, source_kind: str, destination: Path) -> list[str]:
	command = [stock_git, "clone", "--quiet", "--no-checkout", "--branch", branch]
	if source_kind == "local_path":
		# Avoid hardlinks/references so the control measures Git reading a real
		# local object database rather than a metadata shortcut.
		command.append("--no-local")
	command.extend([source, str(destination)])
	return command


def clone_once(
	series: str,
	source: str,
	source_kind: str,
	iteration: int,
	expected_tip: str,
	phase: str,
) -> dict[str, object]:
	destination = scratch / "{}-{}-{}".format(phase, series, iteration + 1)
	command = clone_command(source, source_kind, destination)
	record: dict[str, object] = {
		"trial_id": "{}-{}-{}".format(phase, series, iteration + 1),
		"phase": phase,
		"operation": "clone",
		"series": series,
		"iteration": iteration + 1,
		"source": redact_url(source) if source_kind == "https" else source,
		"argv": command_display(command),
		"started_at_utc": now_utc(),
		"status": "running",
	}
	started = time.perf_counter_ns()
	try:
		run_checked(command, "{} clone {}".format(series, iteration + 1))
		record["elapsed_ms"] = round((time.perf_counter_ns() - started) / 1_000_000, 3)
		actual_tip = run_checked(
			[stock_git, "-C", str(destination), "rev-parse", "HEAD"],
			"verify {} clone {} tip".format(series, iteration + 1),
			capture=True,
		).strip()
		record["tip"] = actual_tip
		if actual_tip != expected_tip:
			raise BenchmarkError(
				"{} clone {} reached {}, expected {}".format(
					series, iteration + 1, actual_tip, expected_tip
				)
			)
		run_checked(
			[stock_git, "-C", str(destination), "fsck", "--no-progress"],
			"fsck {} clone {}".format(series, iteration + 1),
		)
		record["fsck"] = "clean"
		record["status"] = "passed"
	except BenchmarkError as error:
		record.setdefault("elapsed_ms", round((time.perf_counter_ns() - started) / 1_000_000, 3))
		record["status"] = "failed"
		record["error"] = str(error)
		raise
	finally:
		record["object_store_bytes"] = directory_bytes(destination / ".git" / "objects")
		record["completed_at_utc"] = now_utc()
		append_trial(record)
		shutil.rmtree(destination, ignore_errors=True)
	return record


def summary(records: list[dict[str, object]]) -> dict[str, object]:
	if not records or any(record.get("status") != "passed" for record in records):
		raise BenchmarkError("cannot summarize incomplete trial series")
	latencies = [float(record["elapsed_ms"]) for record in records]
	mean = statistics.fmean(latencies)
	stdev = statistics.pstdev(latencies) if len(latencies) > 1 else 0.0
	return {
		"n": len(records),
		"raw_trial_ids": [record["trial_id"] for record in records],
		"samples": records,
		"min_ms": round(min(latencies), 3),
		"max_ms": round(max(latencies), 3),
		"mean_ms": round(mean, 3),
		"stdev_ms": round(stdev, 3),
		"cv_percent": round((stdev / mean) * 100 if mean > 0 else math.inf, 4),
		"p50_ms": round(percentile(latencies, 0.50), 3),
		"p90_ms": round(percentile(latencies, 0.90), 3),
		"p95_ms": round(percentile(latencies, 0.95), 3),
		"p99_ms": round(percentile(latencies, 0.99), 3),
	}


def timed_command(
	*,
	trial_id: str,
	operation: str,
	series: str,
	iteration: int,
	command: list[str],
	action: str,
	phase: str,
) -> dict[str, object]:
	record: dict[str, object] = {
		"trial_id": trial_id,
		"phase": phase,
		"operation": operation,
		"series": series,
		"iteration": iteration,
		"argv": command_display(command),
		"started_at_utc": now_utc(),
		"status": "running",
	}
	started = time.perf_counter_ns()
	try:
		run_checked(command, action)
		record["elapsed_ms"] = round((time.perf_counter_ns() - started) / 1_000_000, 3)
		record["status"] = "passed"
	except BenchmarkError as error:
		record["elapsed_ms"] = round((time.perf_counter_ns() - started) / 1_000_000, 3)
		record["status"] = "failed"
		record["error"] = str(error)
		raise
	finally:
		record["completed_at_utc"] = now_utc()
		append_trial(record)
	return record


def clone_for_fetch(
	series: str,
	source: str,
	source_kind: str,
	expected_tip: str,
) -> Path:
	destination = scratch / "fetch-reader-{}".format(series)
	command = clone_command(source, source_kind, destination)
	run_checked(command, "prepare {} fetch reader".format(series))
	actual = run_checked(
		[stock_git, "-C", str(destination), "rev-parse", "HEAD"],
		"verify {} fetch reader tip".format(series),
		capture=True,
	).strip()
	if actual != expected_tip:
		raise BenchmarkError("{} fetch reader reached {}, expected {}".format(series, actual, expected_tip))
	return destination


def measure_fetch_after_push(
	*,
	expected_tip: str,
	vex_source: str,
	disk_control_source: str,
	disk_control_kind: str,
	benchmark_id: str,
) -> dict[str, object]:
	"""Push matching commits to both controls, then time readers fetching them."""
	writer = scratch / "fetch-writer"
	readers: dict[str, Path] = {}
	remotes = {
		"vex_https": vex_source,
		"disk_control": disk_control_source,
	}
	remote_kinds = {
		"vex_https": "https",
		"disk_control": disk_control_kind,
	}
	last_tips = {
		"vex_https": expected_tip,
		"disk_control": expected_tip,
	}
	result: dict[str, object] = {
		"enabled": True,
		"branch": branch,
		"original_tip": expected_tip,
		"runs": fetch_runs,
		"restore_requested": restore_fetch_branch,
		"pushes": [],
		"restoration": {},
	}
	error: Optional[BenchmarkError] = None
	try:
		writer_command = [stock_git, "clone", "--quiet", "--branch", branch]
		if disk_control_kind == "local_path":
			writer_command.append("--no-local")
		writer_command.extend([disk_control_source, str(writer)])
		run_checked(writer_command, "prepare fetch-after-push writer")
		writer_tip = run_checked(
			[stock_git, "-C", str(writer), "rev-parse", "HEAD"],
			"verify fetch-after-push writer tip",
			capture=True,
		).strip()
		if writer_tip != expected_tip:
			raise BenchmarkError(
				"fetch-after-push writer reached {}, expected {}".format(writer_tip, expected_tip)
			)
		for series, source in remotes.items():
			readers[series] = clone_for_fetch(
				series,
				source,
				remote_kinds[series],
				expected_tip,
			)
			run_checked(
				[stock_git, "-C", str(writer), "remote", "add", "bench-{}".format(series), source],
				"configure {} push remote".format(series),
			)

		marker = writer / ".vex-benchmark-fetch-after-push"
		for iteration in range(fetch_runs):
			marker.write_text(
				"benchmark={} iteration={}\n".format(benchmark_id, iteration + 1),
				encoding="utf-8",
			)
			run_checked(
				[stock_git, "-C", str(writer), "add", "--", marker.name],
				"stage fetch-after-push {} marker".format(iteration + 1),
			)
			run_checked(
				[
					stock_git,
					"-C",
					str(writer),
					"-c",
					"user.name=Vex benchmark",
					"-c",
					"user.email=vex-benchmark@example.invalid",
					"-c",
					"commit.gpgSign=false",
					"commit",
					"--quiet",
					"-m",
					"vex benchmark fetch-after-push {}".format(iteration + 1),
				],
				"commit fetch-after-push {}".format(iteration + 1),
			)
			new_tip = run_checked(
				[stock_git, "-C", str(writer), "rev-parse", "HEAD"],
				"read fetch-after-push {} tip".format(iteration + 1),
				capture=True,
			).strip()
			# Push disk first. If it cannot accept the commit, Vex remains
			# untouched. If Vex later fails, restoration below uses a lease.
			for series in ("disk_control", "vex_https"):
				push_command = [
					stock_git,
					"-C",
					str(writer),
					"push",
					"--quiet",
					"bench-{}".format(series),
					"{}:{}".format(new_tip, refname),
				]
				record = timed_command(
					trial_id="fetch-push-{}-{}".format(series, iteration + 1),
					operation="push",
					series=series,
					iteration=iteration + 1,
					command=push_command,
					action="push fetch-after-push {} {}".format(series, iteration + 1),
					phase="fetch_after_push",
				)
				result["pushes"].append(record)
				last_tips[series] = new_tip

			fetch_order = ["vex_https", "disk_control"]
			if iteration % 2:
				fetch_order.reverse()
			for series in fetch_order:
				reader = readers[series]
				fetch_command = [
					stock_git,
					"-C",
					str(reader),
					"fetch",
					"--quiet",
					"origin",
					"{}:refs/remotes/origin/{}".format(refname, branch),
				]
				record = timed_command(
					trial_id="fetch-{}-{}".format(series, iteration + 1),
					operation="fetch",
					series=series,
					iteration=iteration + 1,
					command=fetch_command,
					action="fetch-after-push {} {}".format(series, iteration + 1),
					phase="fetch_after_push",
				)
				actual = run_checked(
					[
						stock_git,
						"-C",
						str(reader),
						"rev-parse",
						"refs/remotes/origin/{}".format(branch),
					],
					"verify fetch-after-push {} {}".format(series, iteration + 1),
					capture=True,
				).strip()
				if actual != new_tip:
					raise BenchmarkError(
						"fetch-after-push {} {} reached {}, expected {}".format(
							series, iteration + 1, actual, new_tip
						)
					)
				run_checked(
					[stock_git, "-C", str(reader), "fsck", "--no-progress"],
					"fsck fetch-after-push {} {}".format(series, iteration + 1),
				)
				record["tip"] = actual
				record["fsck"] = "clean"
	except BenchmarkError as caught:
		error = caught
	finally:
		if restore_fetch_branch:
			for series in ("vex_https", "disk_control"):
				current_tip = last_tips[series]
				if current_tip == expected_tip:
					result["restoration"][series] = {
						"attempted": False,
						"status": "not_needed",
					}
					continue
				restore_command = [
					stock_git,
					"-C",
					str(writer),
					"push",
					"--quiet",
					"--force-with-lease={}:{}".format(refname, current_tip),
					"bench-{}".format(series),
					"{}:{}".format(expected_tip, refname),
				]
				try:
					record = timed_command(
						trial_id="fetch-restore-{}".format(series),
						operation="restore_ref",
						series=series,
						iteration=0,
						command=restore_command,
						action="restore fetch-after-push {}".format(series),
						phase="fetch_after_push",
					)
					result["restoration"][series] = {
						"attempted": True,
						"status": "passed",
						"trial_id": record["trial_id"],
					}
				except BenchmarkError as restore_error:
					result["restoration"][series] = {
						"attempted": True,
						"status": "failed",
						"error": str(restore_error),
					}
					if error is None:
						error = BenchmarkError(
							"fetch-after-push could not safely restore {}: {}".format(
								series, restore_error
							)
						)
		else:
			result["restoration"] = {
				"status": "not_requested",
				"warning": "benchmark commits remain on {}".format(refname),
			}
		for path in [writer, *readers.values()]:
			shutil.rmtree(path, ignore_errors=True)

	if error is not None:
		raise error

	fetch_records = {
		series: [
			record
			for record in trials
			if record.get("phase") == "fetch_after_push"
			and record.get("operation") == "fetch"
			and record.get("series") == series
		]
		for series in ("vex_https", "disk_control")
	}
	result["vex_https"] = summary(fetch_records["vex_https"])
	result["disk_control"] = summary(fetch_records["disk_control"])
	disk_p50 = float(result["disk_control"]["p50_ms"])
	if disk_p50 <= 0:
		raise BenchmarkError("fetch disk-control p50 was not positive")
	result["ratio_vex_to_disk_control"] = {
		"p50": round(float(result["vex_https"]["p50_ms"]) / disk_p50, 4),
	}
	return result


def empty_gate(required: bool, name: str) -> dict[str, object]:
	return {
		"name": name,
		"required": required,
		"evaluated": False,
		"pass": None,
	}


benchmark_id = "vex-clone-{}".format(uuid.uuid4())
payload: dict[str, object] = {
	"schema_version": 2,
	"benchmark": "vex-stock-git-https-clone-controls",
	"benchmark_id": benchmark_id,
	"timestamp_utc": now_utc(),
	"measurement_only": not (require_disk_gate or require_github or require_fetch_gate),
	# A benchmark cannot certify the full L4 checklist (P0/P1, git/git
	# fixture, pack path, and review evidence). Keep this false even if every
	# numeric threshold passes.
	"l4_success_claim": False,
	"l4_claim_reason": "numeric benchmark evidence is necessary but not sufficient for L4",
	"stock_git": stock_git,
	"fixture": {
		"name": fixture,
		"seed_manifest": None,
	},
	"identity": {
		"source": source_identity(),
		"compose": {
			"project": compose_project,
			"file": artifact_path(compose_file),
			"sha256": sha256_file(compose_file),
		},
		"environment": environment_identity(),
	},
	"policy": {
		"clone_trials": runs,
		"warmups_per_source": warmups_count,
		"minimum_trials_for_stability": 5,
		"max_cv_percent": max_cv_percent,
		"max_disk_ratio": max_disk_ratio,
		"max_github_ratio": max_github_ratio,
		"max_fetch_ratio": max_fetch_ratio,
		"require_disk_gate": require_disk_gate,
		"require_github": require_github,
		"require_stable": require_stable,
		"require_fetch_after_push_gate": require_fetch_gate,
	},
	"sources": {
		"vex_https": {"url": origin_display, "transport": "https"},
		"disk_control": {
			"source": disk_display,
			"transport": "https" if disk_kind == "https" else "local_path",
			"same_https_frontend": disk_control_is_l4_shape,
			"fixed_l4_path": disk_control_is_l4_shape,
		},
		"github": {
			"included": include_github,
			"url": github_display if include_github else None,
		},
	},
	"trials": trials,
	"gates": {
		"stability": empty_gate(require_stable, "coefficient_of_variation"),
		"disk_control": empty_gate(require_disk_gate, "vex_p50_le_1.5x_disk_p50"),
		"github": empty_gate(require_github, "vex_p50_le_1.5x_github_p50"),
		"fetch_after_push": empty_gate(require_fetch_gate, "vex_fetch_p50_le_2x_disk_fetch_p50"),
	},
}


def write_payload() -> None:
	temporary = output.with_name(output.name + ".tmp-{}".format(os.getpid()))
	try:
		temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
		os.replace(temporary, output)
	finally:
		temporary.unlink(missing_ok=True)


def gate_failure(name: str) -> None:
	payload.setdefault("result", {}).setdefault("enforced_failures", []).append(name)


try:
	payload["fixture"]["seed_manifest"] = manifest_identity()
	run_checked(
		[stock_git, "check-ref-format", "--branch", branch],
		"validate benchmark branch",
	)
	if not math.isfinite(max_disk_ratio) or max_disk_ratio <= 0:
		raise BenchmarkError("max disk ratio must be positive")
	if not math.isfinite(max_github_ratio) or max_github_ratio <= 0:
		raise BenchmarkError("max GitHub ratio must be positive")
	if not math.isfinite(max_fetch_ratio) or max_fetch_ratio <= 0:
		raise BenchmarkError("max fetch ratio must be positive")
	if not math.isfinite(max_cv_percent) or max_cv_percent < 0:
		raise BenchmarkError("max CV percent must be non-negative")

	expected_tip = source_tip(disk_source, disk_kind, "disk-control")
	vex_tip = remote_tip(origin, "Vex")
	if vex_tip != expected_tip:
		raise BenchmarkError(
			"disk-control {} and Vex HTTPS {} do not name the same {} ({} != {})".format(
				disk_display, origin_display, refname, expected_tip, vex_tip
			)
		)
	payload["ref"] = {
		"name": refname,
		"oid": expected_tip,
		"stable_across_clone_measurement": False,
	}
	payload["sources"]["vex_https"]["ref_oid"] = vex_tip
	payload["sources"]["disk_control"]["ref_oid"] = expected_tip

	series_sources: dict[str, tuple[str, str]] = {
		"vex_https": (origin, "https"),
		"disk_control": (disk_source, disk_kind),
	}
	if include_github:
		github_tip = remote_tip(github_origin, "GitHub")
		payload["sources"]["github"]["ref_oid"] = github_tip
		if github_tip != expected_tip:
			raise BenchmarkError(
				"GitHub {} is at {}, but Vex/disk control are at {}; refresh the seed before comparing".format(
					refname, github_tip, expected_tip
				)
			)
		series_sources["github"] = (github_origin, "https")

	warmups: list[dict[str, object]] = []
	for iteration in range(warmups_count):
		for series, (source, source_kind) in series_sources.items():
			warmups.append(
				clone_once(series, source, source_kind, iteration, expected_tip, "warmup")
			)
	payload["warmups"] = warmups

	measured: dict[str, list[dict[str, object]]] = {series: [] for series in series_sources}
	series_order = list(series_sources)
	sample_order: list[list[str]] = []
	for iteration in range(runs):
		order = series_order[iteration % len(series_order):] + series_order[:iteration % len(series_order)]
		sample_order.append(order)
		for series in order:
			source, source_kind = series_sources[series]
			measured[series].append(
				clone_once(series, source, source_kind, iteration, expected_tip, "measured")
			)

	if remote_tip(origin, "Vex") != expected_tip:
		raise BenchmarkError("Vex benchmark ref {} changed while clone samples were collected".format(refname))
	if source_tip(disk_source, disk_kind, "disk-control") != expected_tip:
		raise BenchmarkError("disk-control benchmark ref {} changed while clone samples were collected".format(refname))
	if include_github and remote_tip(github_origin, "GitHub") != expected_tip:
		raise BenchmarkError("GitHub benchmark ref {} changed while clone samples were collected".format(refname))
	payload["ref"]["stable_across_clone_measurement"] = True
	payload["sample_order"] = sample_order

	vex_https = summary(measured["vex_https"])
	disk_control = summary(measured["disk_control"])
	payload["vex_https"] = vex_https
	payload["disk_control"] = disk_control
	disk_p50 = float(disk_control["p50_ms"])
	if disk_p50 <= 0:
		raise BenchmarkError("disk-control p50 was not positive")
	payload["ratio_vex_to_disk_control"] = {
		"p50": round(float(vex_https["p50_ms"]) / disk_p50, 4),
		"p90": round(float(vex_https["p90_ms"]) / float(disk_control["p90_ms"]), 4),
		"p95": round(float(vex_https["p95_ms"]) / float(disk_control["p95_ms"]), 4),
	}

	stability_series = {
		series: {
			"n": values["n"],
			"cv_percent": values["cv_percent"],
			"pass": values["n"] >= 5 and float(values["cv_percent"]) <= max_cv_percent,
		}
		for series, values in {"vex_https": vex_https, "disk_control": disk_control}.items()
	}
	if include_github:
		github_summary = summary(measured["github"])
		payload["github"] = github_summary
		github_p50 = float(github_summary["p50_ms"])
		if github_p50 <= 0:
			raise BenchmarkError("GitHub p50 was not positive")
		payload["ratio_vex_to_github"] = {
			"p50": round(float(vex_https["p50_ms"]) / github_p50, 4),
			"p90": round(float(vex_https["p90_ms"]) / float(github_summary["p90_ms"]), 4),
			"p95": round(float(vex_https["p95_ms"]) / float(github_summary["p95_ms"]), 4),
		}
		stability_series["github"] = {
			"n": github_summary["n"],
			"cv_percent": github_summary["cv_percent"],
			"pass": github_summary["n"] >= 5
			and float(github_summary["cv_percent"]) <= max_cv_percent,
		}

	stability_pass = all(item["pass"] for item in stability_series.values())
	payload["gates"]["stability"] = {
		"name": "coefficient_of_variation",
		"required": require_stable,
		"evaluated": True,
		"minimum_trials": 5,
		"max_cv_percent": max_cv_percent,
		"series": stability_series,
		"pass": stability_pass,
	}
	disk_ratio = float(payload["ratio_vex_to_disk_control"]["p50"])
	payload["gates"]["disk_control"] = {
		"name": "vex_p50_le_1.5x_disk_p50",
		"required": require_disk_gate,
		"evaluated": True,
		"threshold_ratio": max_disk_ratio,
		"observed_ratio_p50": disk_ratio,
		"disk_transport": "https" if disk_kind == "https" else "local_path",
		"same_https_frontend": disk_control_is_l4_shape,
		"fixed_l4_path": disk_control_is_l4_shape,
		"threshold_pass": disk_ratio <= max_disk_ratio,
		# A local-path comparison remains useful for smoke measurements, but the
		# PRD's structural acceptance control is the fixed same-frontend route.
		"l4_eligible_control": disk_control_is_l4_shape,
		"pass": disk_ratio <= max_disk_ratio and disk_control_is_l4_shape,
	}
	if include_github:
		github_ratio = float(payload["ratio_vex_to_github"]["p50"])
		payload["gates"]["github"] = {
			"name": "vex_p50_le_1.5x_github_p50",
			"required": require_github,
			"evaluated": True,
			"threshold_ratio": max_github_ratio,
			"observed_ratio_p50": github_ratio,
			"pass": github_ratio <= max_github_ratio,
		}

	if fetch_after_push:
		fetch_result = measure_fetch_after_push(
			expected_tip=expected_tip,
			vex_source=origin,
			disk_control_source=disk_source,
			disk_control_kind=disk_kind,
			benchmark_id=benchmark_id,
		)
		payload["fetch_after_push"] = fetch_result
		fetch_ratio = float(fetch_result["ratio_vex_to_disk_control"]["p50"])
		fetch_stability = {
			series: {
				"n": fetch_result[series]["n"],
				"cv_percent": fetch_result[series]["cv_percent"],
				"pass": fetch_result[series]["n"] >= 5
				and float(fetch_result[series]["cv_percent"]) <= max_cv_percent,
			}
			for series in ("vex_https", "disk_control")
		}
		payload["gates"]["fetch_after_push"] = {
			"name": "vex_fetch_p50_le_2x_disk_fetch_p50",
			"required": require_fetch_gate,
			"evaluated": True,
			"threshold_ratio": max_fetch_ratio,
			"observed_ratio_p50": fetch_ratio,
			"stability": fetch_stability,
			"pass": fetch_ratio <= max_fetch_ratio
			and all(item["pass"] for item in fetch_stability.values()),
		}

	payload["result"] = {
		"status": "measured",
		"enforced_failures": [],
	}
	if require_stable and not payload["gates"]["stability"]["pass"]:
		gate_failure("stability")
	if require_disk_gate and not payload["gates"]["disk_control"]["pass"]:
		gate_failure("disk_control")
	if require_github and not payload["gates"]["github"]["pass"]:
		gate_failure("github")
	if require_fetch_gate and not payload["gates"]["fetch_after_push"]["pass"]:
		gate_failure("fetch_after_push")
	if payload["result"]["enforced_failures"]:
		payload["result"]["status"] = "gate_failed"
		write_payload()
		print(
			"vex clone benchmark: required gates failed: {}".format(
				", ".join(payload["result"]["enforced_failures"])
			),
			file=sys.stderr,
		)
		raise SystemExit(1)
except (BenchmarkError, OSError) as error:
	payload["result"] = {
		"status": "error",
		"error": redact_text(str(error)),
		"enforced_failures": payload.get("result", {}).get("enforced_failures", []),
	}
	try:
		write_payload()
	except OSError as write_error:
		print("vex clone benchmark: could not write result: {}".format(write_error), file=sys.stderr)
	print("vex clone benchmark: {}".format(error), file=sys.stderr)
	raise SystemExit(1)

write_payload()
print("vex clone benchmark: result={}".format(output))
PY

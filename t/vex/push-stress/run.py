#!/usr/bin/env python3
"""Execute a deterministic push-stress plan using only stock Git commands.

This runner intentionally knows nothing about the Vex metadata or blob APIs.
It exercises the target exclusively through ``git push``, ``git ls-remote``,
and a fresh ``git fetch``/``git fsck`` reconciliation clone.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any

from plan import PlanError, build_plan, default_source_ref, load_plan, sha256_file, write_plan


class HarnessError(RuntimeError):
    """A target, artifact, or stock-Git harness failure."""


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def json_compact(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def write_json_atomic(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=str(path.parent), prefix=".tmp.", delete=False
    ) as handle:
        json.dump(value, handle, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
        temporary = Path(handle.name)
    os.replace(temporary, path)


def append_jsonl(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = (json_compact(value) + "\n").encode("utf-8")
    with path.open("ab") as handle:
        handle.write(encoded)
        handle.flush()
        os.fsync(handle.fileno())


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HarnessError("cannot read {}: {}".format(path, exc)) from exc
    if not isinstance(value, dict):
        raise HarnessError("{} must contain a JSON object".format(path))
    return value


def command_text(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def run_command(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def require_success(process: subprocess.CompletedProcess[str], what: str) -> str:
    if process.returncode:
        raise HarnessError(
            "{} failed ({}): {}".format(what, process.returncode, process.stderr.strip())
        )
    return process.stdout


def git_at(path: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return run_command(["git", "-C", str(path), *args])


def git_global(*args: str) -> subprocess.CompletedProcess[str]:
    return run_command(["git", *args])


def env_text(name: str, fallback: str | None = None) -> str | None:
    value = os.environ.get(name)
    return value if value not in (None, "") else fallback


def env_int(name: str, fallback: int) -> int:
    value = env_text(name)
    if value is None:
        return fallback
    try:
        return int(value)
    except ValueError as exc:
        raise HarnessError("{} must be an integer, got {!r}".format(name, value)) from exc


def parse_bool(value: str, name: str) -> bool:
    normalized = value.strip().lower()
    if normalized in ("1", "true", "yes", "on"):
        return True
    if normalized in ("0", "false", "no", "off"):
        return False
    raise HarnessError("{} must be a boolean, got {!r}".format(name, value))


def env_bool(name: str, fallback: bool) -> bool:
    value = env_text(name)
    return fallback if value is None else parse_bool(value, name)


def option_text(value: str | None, env_name: str, fallback: str | None = None) -> str | None:
    return value if value not in (None, "") else env_text(env_name, fallback)


def option_int(value: int | None, env_name: str, fallback: int) -> int:
    return value if value is not None else env_int(env_name, fallback)


def option_bool(value: bool | None, env_name: str, fallback: bool) -> bool:
    return value if value is not None else env_bool(env_name, fallback)


def truncated(text: str, limit: int = 1200) -> str:
    compact = text.strip()
    if len(compact) <= limit:
        return compact
    return compact[:limit] + "…[truncated]"


def remote_tip(origin: str, branch: str) -> tuple[str | None, str | None]:
    process = git_global("ls-remote", origin, "refs/heads/{}".format(branch))
    if process.returncode:
        return None, truncated(process.stderr)
    expected_ref = "refs/heads/{}".format(branch)
    for line in process.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] == expected_ref:
            return fields[0], None
    return None, None


def is_ancestor(driver: Path, ancestor: str, descendant: str) -> bool:
    process = git_at(driver, "merge-base", "--is-ancestor", ancestor, descendant)
    if process.returncode not in (0, 1):
        raise HarnessError("cannot compare plan ancestry: {}".format(process.stderr.strip()))
    return process.returncode == 0


def estimate_bytes(driver: Path, base_tip: str | None, want_tip: str) -> int | None:
    """Estimate raw new-object payload; this is deliberately not wire bytes."""

    revision = want_tip
    if base_tip and is_ancestor(driver, base_tip, want_tip):
        revision = "{}..{}".format(base_tip, want_tip)
    listed = git_at(driver, "rev-list", "--objects", revision)
    if listed.returncode:
        return None
    oids: list[str] = []
    seen: set[str] = set()
    for line in listed.stdout.splitlines():
        oid = line.split(" ", 1)[0]
        if oid and oid not in seen:
            seen.add(oid)
            oids.append(oid)
    if not oids:
        return 0
    process = subprocess.Popen(
        ["git", "-C", str(driver), "cat-file", "--batch-check=%(objectsize)"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, _stderr = process.communicate("".join("{}\n".format(oid) for oid in oids))
    if process.returncode:
        return None
    total = 0
    for line in stdout.splitlines():
        try:
            total += int(line)
        except ValueError:
            return None
    return total


def checkpoint_value(
    *,
    plan_sha256: str,
    origin: str,
    source: str,
    branch: str,
    source_ref: str,
    next_i: int,
    last_ok_tip: str | None,
    started_at: str,
) -> dict[str, object]:
    return {
        "branch": branch,
        "last_ok_tip": last_ok_tip,
        "next_i": next_i,
        "origin": origin,
        "plan_sha256": plan_sha256,
        "source": source,
        "source_ref": source_ref,
        "started_at": started_at,
        "updated_at": utc_now(),
    }


def ensure_driver(driver: Path, origin: str, source: str) -> None:
    if driver.exists():
        process = git_at(driver, "rev-parse", "--git-dir")
        if process.returncode:
            raise HarnessError(
                "driver path exists but is not a Git repository: {}".format(driver)
            )
        for remote, expected in (("origin", origin), ("src", source)):
            current = git_at(driver, "remote", "get-url", remote)
            if current.returncode or current.stdout.strip() != expected:
                raise HarnessError(
                    "existing driver {} has a different {} remote; use a new --work-dir".format(
                        driver, remote
                    )
                )
        return

    driver.parent.mkdir(parents=True, exist_ok=True)
    require_success(git_global("init", "-q", str(driver)), "git init driver")
    require_success(git_at(driver, "remote", "add", "origin", origin), "configure origin")
    require_success(git_at(driver, "remote", "add", "src", source), "configure source")
    require_success(
        git_at(driver, "config", "user.name", "push-stress harness"), "configure driver identity"
    )
    require_success(
        git_at(driver, "config", "user.email", "push-stress@invalid"),
        "configure driver identity",
    )


def bootstrap_source(driver: Path, source_ref: str, wanted_tip: str) -> float:
    """Fetch the advertised source ref once, outside timed target pushes."""

    start = time.perf_counter_ns()
    destination = "refs/push-stress/source-tip"
    process = git_at(driver, "fetch", "--no-tags", "src", "+{}:{}".format(source_ref, destination))
    elapsed = (time.perf_counter_ns() - start) / 1_000_000
    require_success(process, "bootstrap source fetch")
    available = git_at(driver, "cat-file", "-e", "{}^{{commit}}".format(wanted_tip))
    require_success(available, "validate source tip in driver")
    return elapsed


def run_push(
    command: list[str], kill_after_ms: int | None
) -> tuple[subprocess.CompletedProcess[str], bool]:
    if kill_after_ms is None:
        return run_command(command), False
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(max(0, kill_after_ms) / 1000)
    killed = False
    if process.poll() is None:
        process.kill()
        killed = True
    stdout, stderr = process.communicate()
    return (
        subprocess.CompletedProcess(command, process.returncode, stdout=stdout, stderr=stderr),
        killed,
    )


class ReaderGroup:
    """Best-effort stock-Git fetch readers; their failures are diagnostic only."""

    def __init__(self, count: int, work_dir: Path, origin: str, branch: str, interval_ms: int, out: Path):
        self.count = count
        self.work_dir = work_dir
        self.origin = origin
        self.branch = branch
        self.interval_seconds = max(0, interval_ms) / 1000
        self.out = out
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.threads: list[threading.Thread] = []

    def start(self) -> None:
        for index in range(self.count):
            thread = threading.Thread(target=self._loop, args=(index,), daemon=True)
            thread.start()
            self.threads.append(thread)

    def _emit(self, value: dict[str, object]) -> None:
        with self.lock:
            append_jsonl(self.out, value)

    def _loop(self, index: int) -> None:
        reader = self.work_dir / "readers" / str(index)
        try:
            reader.parent.mkdir(parents=True, exist_ok=True)
            require_success(git_global("init", "-q", str(reader)), "init reader")
            require_success(git_at(reader, "remote", "add", "origin", self.origin), "configure reader")
        except HarnessError as exc:
            self._emit({"error": str(exc), "reader": index, "t": utc_now()})
            return
        while not self.stop_event.is_set():
            start = time.perf_counter_ns()
            process = git_at(
                reader,
                "fetch",
                "--no-tags",
                "origin",
                "refs/heads/{}:refs/remotes/origin/{}".format(self.branch, self.branch),
            )
            self._emit(
                {
                    "dt_ms": round((time.perf_counter_ns() - start) / 1_000_000, 3),
                    "error": None if process.returncode == 0 else truncated(process.stderr),
                    "fetch_exit": process.returncode,
                    "reader": index,
                    "t": utc_now(),
                }
            )
            self.stop_event.wait(self.interval_seconds)

    def stop(self) -> None:
        self.stop_event.set()
        for thread in self.threads:
            thread.join(timeout=10)


def run_server_recon_command(
    command: str | None,
    *,
    origin: str,
    branch: str,
    expected_tip: str,
    plan_sha256: str,
) -> dict[str, object]:
    if not command:
        return {
            "configured": False,
            "note": "No server recon command configured; stock-Git recon cannot inspect canonical blob storage directly.",
            "status": "not-configured",
        }
    environment = os.environ.copy()
    environment.update(
        {
            "GIT_VEX_PUSH_STRESS_BRANCH": branch,
            "GIT_VEX_PUSH_STRESS_EXPECTED_TIP": expected_tip,
            "GIT_VEX_PUSH_STRESS_ORIGIN": origin,
            "GIT_VEX_PUSH_STRESS_PLAN_SHA256": plan_sha256,
        }
    )
    process = subprocess.run(
        command,
        shell=True,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
    )
    return {
        "configured": True,
        "exit": process.returncode,
        "status": "pass" if process.returncode == 0 else "fail",
        "stderr": truncated(process.stderr),
        "stdout": truncated(process.stdout),
    }


def reconcile(
    *,
    origin: str,
    branch: str,
    expected_tip: str,
    work_dir: Path,
    plan_sha256: str,
    server_recon_command: str | None,
) -> dict[str, object]:
    """Freshly fetch the target and fsck it, without looking behind the protocol."""

    record: dict[str, object] = {
        "branch": branch,
        "expected_tip": expected_tip,
        "origin": origin,
        "plan_sha256": plan_sha256,
        "started_at": utc_now(),
    }
    direct_tip, direct_error = remote_tip(origin, branch)
    record["ls_remote_error"] = direct_error
    record["server_tip"] = direct_tip
    record["tip_match"] = direct_tip == expected_tip

    recon_dir = work_dir / "recon" / "{}-{}".format(int(time.time() * 1000), os.getpid())
    record["fresh_repo"] = str(recon_dir)
    try:
        recon_dir.parent.mkdir(parents=True, exist_ok=True)
        require_success(git_global("init", "-q", str(recon_dir)), "init reconciliation repository")
        require_success(git_at(recon_dir, "remote", "add", "origin", origin), "configure reconciliation remote")
        fetched_ref = "refs/remotes/origin/{}".format(branch)
        fetch = git_at(
            recon_dir,
            "fetch",
            "--no-tags",
            "origin",
            "+refs/heads/{0}:{1}".format(branch, fetched_ref),
        )
        require_success(fetch, "fresh target fetch")
        fetched_tip = require_success(
            git_at(recon_dir, "rev-parse", "--verify", "{}^{{commit}}".format(fetched_ref)),
            "resolve freshly fetched tip",
        ).strip()
        object_check = git_at(recon_dir, "cat-file", "-e", "{}^{{commit}}".format(expected_tip))
        fsck = git_at(recon_dir, "fsck", "--full")
        record.update(
            {
                "fresh_fetch_exit": fetch.returncode,
                "fresh_tip": fetched_tip,
                "fresh_tip_match": fetched_tip == expected_tip,
                "object_check_exit": object_check.returncode,
                "fsck_exit": fsck.returncode,
                "fsck_stderr": truncated(fsck.stderr),
                "stock_git_recon_pass": fetched_tip == expected_tip
                and object_check.returncode == 0
                and fsck.returncode == 0,
            }
        )
    except HarnessError as exc:
        record.update({"error": str(exc), "stock_git_recon_pass": False})

    canonical = run_server_recon_command(
        server_recon_command,
        origin=origin,
        branch=branch,
        expected_tip=expected_tip,
        plan_sha256=plan_sha256,
    )
    record["canonical_blob_recon"] = canonical
    record["pass"] = bool(record.get("stock_git_recon_pass")) and canonical.get("status") != "fail"
    record["finished_at"] = utc_now()
    return record


def quantile(samples: list[float], percentile: float) -> float | None:
    if not samples:
        return None
    ordered = sorted(samples)
    index = max(0, math.ceil(percentile * len(ordered)) - 1)
    return round(ordered[index], 3)


def write_summary(
    *,
    out_dir: Path,
    plan: list[dict[str, object]],
    plan_sha256: str,
    checkpoint: dict[str, Any],
    recon_record: dict[str, object] | None,
) -> dict[str, object]:
    metrics_path = out_dir / "metrics.jsonl"
    metrics: list[dict[str, Any]] = []
    if metrics_path.exists():
        for line in metrics_path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                metrics.append(json.loads(line))
    latencies = [float(item["dt_ms"]) for item in metrics if isinstance(item.get("dt_ms"), (int, float))]
    accepted = [item for item in metrics if item.get("accepted") is True]
    grouped: dict[str, list[float]] = {}
    for item in accepted:
        if isinstance(item.get("dt_ms"), (int, float)):
            grouped.setdefault(str(item.get("batch")), []).append(float(item["dt_ms"]))
    by_batch = {
        batch: {"p50_ms": quantile(values, 0.50), "p90_ms": quantile(values, 0.90), "p99_ms": quantile(values, 0.99)}
        for batch, values in sorted(grouped.items(), key=lambda item: int(item[0]))
    }
    planned_tip = str(plan[-1]["want_tip"])
    summary: dict[str, object] = {
        "attempts": len(metrics),
        "by_batch": by_batch,
        "commits_planned": sum(int(item["n_commits"]) for item in plan),
        "failures": sum(1 for item in metrics if item.get("accepted") is not True),
        "final_checkpoint_tip": checkpoint.get("last_ok_tip"),
        "final_planned_tip": planned_tip,
        "latency_ms": {
            "p50": quantile(latencies, 0.50),
            "p90": quantile(latencies, 0.90),
            "p99": quantile(latencies, 0.99),
        },
        "metrics_lines_with_push_exit": sum(1 for item in metrics if "push_exit" in item),
        "plan_sha256": plan_sha256,
        "steps_completed": int(checkpoint.get("next_i", 0)),
        "steps_planned": len(plan),
        "tip_match_final": checkpoint.get("last_ok_tip") == planned_tip,
    }
    if recon_record is not None:
        summary["recon_pass"] = recon_record.get("pass")
    write_json_atomic(out_dir / "summary.json", summary)
    return summary


def config_from_args(args: argparse.Namespace) -> dict[str, Any]:
    source = option_text(args.source, "GIT_VEX_PUSH_STRESS_SOURCE", env_text("GIT_VEX_MIRROR"))
    origin = option_text(args.origin, "GIT_VEX_PUSH_STRESS_ORIGIN")
    work_default = "/tmp/git-vex-push-stress"
    work_dir = Path(option_text(args.work_dir, "GIT_VEX_PUSH_STRESS_WORK_DIR", work_default) or work_default)
    out_default = str(work_dir / "out")
    out_dir = Path(option_text(args.out_dir, "GIT_VEX_PUSH_STRESS_OUT_DIR", out_default) or out_default)
    plan_default = str(out_dir / "plan.jsonl")
    source_ref = option_text(args.source_ref, "GIT_VEX_PUSH_STRESS_SOURCE_REF")
    return {
        "allow_nonempty_origin": option_bool(args.allow_nonempty_origin, "GIT_VEX_PUSH_STRESS_ALLOW_NONEMPTY_ORIGIN", False),
        "batch_list": option_text(args.batch_list, "GIT_VEX_PUSH_STRESS_BATCH_LIST", "1") or "1",
        "batch_max": option_int(args.batch_max, "GIT_VEX_PUSH_STRESS_BATCH_MAX", 64),
        "batch_min": option_int(args.batch_min, "GIT_VEX_PUSH_STRESS_BATCH_MIN", 1),
        "batch_mode": option_text(args.batch_mode, "GIT_VEX_PUSH_STRESS_BATCH_MODE", "fixed") or "fixed",
        "batch_size": option_int(args.batch_size, "GIT_VEX_PUSH_STRESS_BATCH_SIZE", 1),
        "branch": option_text(args.branch, "GIT_VEX_PUSH_STRESS_BRANCH", "main") or "main",
        "concurrency_readers": option_int(args.concurrency_readers, "GIT_VEX_PUSH_STRESS_CONCURRENCY_READERS", 0),
        "explicit_file": option_text(args.explicit_file, "GIT_VEX_PUSH_STRESS_EXPLICIT_FILE"),
        "fail_fast": option_bool(args.fail_fast, "GIT_VEX_PUSH_STRESS_FAIL_FAST", True),
        "fsck_every": option_int(args.fsck_every, "GIT_VEX_PUSH_STRESS_FSCK_EVERY", 0),
        "kill_delay_ms": option_int(args.kill_delay_ms, "GIT_VEX_PUSH_STRESS_KILL_DELAY_MS", 50),
        "kill_every": option_int(args.kill_every, "GIT_VEX_PUSH_STRESS_KILL_EVERY", 0),
        "max_commits": option_int(args.max_commits, "GIT_VEX_PUSH_STRESS_MAX_COMMITS", 0),
        "max_pushes": option_int(args.max_pushes, "GIT_VEX_PUSH_STRESS_MAX_PUSHES", 0),
        "max_retries": option_int(args.max_retries, "GIT_VEX_PUSH_STRESS_MAX_RETRIES", 1),
        "order": option_text(args.order, "GIT_VEX_PUSH_STRESS_ORDER", "first-parent") or "first-parent",
        "origin": origin,
        "out_dir": out_dir,
        "pause_ms": option_int(args.pause_ms, "GIT_VEX_PUSH_STRESS_PAUSE_MS", 0),
        "plan_file": Path(option_text(args.plan_file, "GIT_VEX_PUSH_STRESS_PLAN_FILE", plan_default) or plan_default),
        "random_seed": option_int(args.random_seed, "GIT_VEX_PUSH_STRESS_RANDOM_SEED", 1),
        "reader_interval_ms": option_int(args.reader_interval_ms, "GIT_VEX_PUSH_STRESS_READER_INTERVAL_MS", 1000),
        "recon_command": option_text(args.recon_command, "GIT_VEX_PUSH_STRESS_RECON_COMMAND"),
        "resume": option_bool(args.resume, "GIT_VEX_PUSH_STRESS_RESUME", False),
        "reuse_plan": bool(args.reuse_plan),
        "source": source,
        "source_ref": source_ref,
        "verify_every": option_int(args.verify_every, "GIT_VEX_PUSH_STRESS_VERIFY_EVERY", 50),
        "work_dir": work_dir,
    }


def validate_config(config: dict[str, Any], *, needs_origin: bool) -> None:
    if not config["source"]:
        raise HarnessError("--source or GIT_VEX_PUSH_STRESS_SOURCE/GIT_VEX_MIRROR is required")
    if needs_origin and not config["origin"]:
        raise HarnessError("--origin or GIT_VEX_PUSH_STRESS_ORIGIN is required")
    if config["order"] not in ("first-parent", "topo", "explicit"):
        raise HarnessError("unsupported --order: {}".format(config["order"]))
    if config["batch_mode"] not in ("fixed", "ramp", "random", "list"):
        raise HarnessError("unsupported --batch-mode: {}".format(config["batch_mode"]))
    for key in (
        "concurrency_readers",
        "fsck_every",
        "kill_every",
        "kill_delay_ms",
        "max_commits",
        "max_pushes",
        "max_retries",
        "pause_ms",
        "reader_interval_ms",
        "verify_every",
    ):
        if config[key] < 0:
            raise HarnessError("{} cannot be negative".format(key.replace("_", "-")))


def make_or_load_plan(config: dict[str, Any], *, run_only: bool) -> tuple[list[dict[str, object]], str, str]:
    plan_file: Path = config["plan_file"]
    source_ref = config["source_ref"] or default_source_ref(str(config["source"]))
    if plan_file.exists():
        if not (run_only or config["resume"] or config["reuse_plan"]):
            raise HarnessError(
                "{} already exists; use --resume, --run-only, or --reuse-plan".format(plan_file)
            )
        plan = load_plan(plan_file)
        return plan, sha256_file(plan_file), source_ref
    if run_only:
        raise HarnessError("--run-only requested but no plan exists at {}".format(plan_file))
    try:
        plan, _metadata = build_plan(
            source=str(config["source"]),
            source_ref=source_ref,
            order=config["order"],
            explicit_file=config["explicit_file"],
            batch_mode=config["batch_mode"],
            batch_size=config["batch_size"],
            batch_min=config["batch_min"],
            batch_max=config["batch_max"],
            batch_list=config["batch_list"],
            random_seed=config["random_seed"],
            max_commits=config["max_commits"],
            max_pushes=config["max_pushes"],
        )
    except PlanError as exc:
        raise HarnessError(str(exc)) from exc
    plan_sha256 = write_plan(plan_file, plan)
    return plan, plan_sha256, source_ref


def initial_checkpoint(
    config: dict[str, Any], plan: list[dict[str, object]], plan_sha256: str, source_ref: str
) -> dict[str, Any]:
    checkpoint_path: Path = config["out_dir"] / "checkpoint.json"
    if config["resume"]:
        if not checkpoint_path.exists():
            raise HarnessError("--resume requested but {} is missing".format(checkpoint_path))
        checkpoint = load_json(checkpoint_path)
        if checkpoint.get("plan_sha256") != plan_sha256:
            raise HarnessError("resume refused: checkpoint plan_sha256 does not match plan.jsonl")
        for key in ("origin", "source", "branch"):
            if checkpoint.get(key) != config[key]:
                raise HarnessError("resume refused: checkpoint {} does not match current run".format(key))
        next_i = checkpoint.get("next_i")
        if not isinstance(next_i, int) or next_i < 0 or next_i > len(plan):
            raise HarnessError("resume refused: invalid checkpoint next_i")
        expected = None if next_i == 0 else str(plan[next_i - 1]["want_tip"])
        if checkpoint.get("last_ok_tip") != expected:
            raise HarnessError("resume refused: checkpoint last_ok_tip does not match plan")
        actual, error = remote_tip(str(config["origin"]), str(config["branch"]))
        if error:
            raise HarnessError("resume refused: cannot read origin tip: {}".format(error))
        if actual != expected:
            raise HarnessError(
                "resume refused: origin tip {!r} does not equal checkpoint {!r}".format(actual, expected)
            )
        return checkpoint

    if checkpoint_path.exists():
        raise HarnessError(
            "{} exists; use --resume or choose a new --out-dir".format(checkpoint_path)
        )
    actual, error = remote_tip(str(config["origin"]), str(config["branch"]))
    if error:
        raise HarnessError("cannot read origin tip: {}".format(error))
    if actual is not None and not config["allow_nonempty_origin"]:
        raise HarnessError(
            "origin branch refs/heads/{} is not empty ({})".format(config["branch"], actual)
        )
    if actual is not None:
        raise HarnessError("nonempty origin support requires a matching resume checkpoint")
    checkpoint = checkpoint_value(
        plan_sha256=plan_sha256,
        origin=str(config["origin"]),
        source=str(config["source"]),
        branch=str(config["branch"]),
        source_ref=source_ref,
        next_i=0,
        last_ok_tip=None,
        started_at=utc_now(),
    )
    write_json_atomic(checkpoint_path, checkpoint)
    return checkpoint


def push_command(driver: Path, branch: str, base_tip: str | None, want_tip: str) -> tuple[list[str], bool]:
    command = [
        "git",
        "-C",
        str(driver),
        "push",
        "--porcelain",
        "origin",
    ]
    forced = False
    if base_tip and not is_ancestor(driver, base_tip, want_tip):
        command.append("--force-with-lease=refs/heads/{}:{}".format(branch, base_tip))
        forced = True
    command.append("refs/heads/{0}:refs/heads/{0}".format(branch))
    return command, forced


def execute(config: dict[str, Any], plan: list[dict[str, object]], plan_sha256: str, source_ref: str) -> int:
    out_dir: Path = config["out_dir"]
    out_dir.mkdir(parents=True, exist_ok=True)
    checkpoint = initial_checkpoint(config, plan, plan_sha256, source_ref)
    driver: Path = config["work_dir"] / "driver"
    ensure_driver(driver, str(config["origin"]), str(config["source"]))
    if checkpoint["next_i"] < len(plan):
        bootstrap_ms = bootstrap_source(driver, source_ref, str(plan[-1]["want_tip"]))
    else:
        bootstrap_ms = 0.0
    run_record = {
        "bootstrap_source_fetch_ms": round(bootstrap_ms, 3),
        "branch": config["branch"],
        "git_version": require_success(git_global("--version"), "git --version").strip(),
        "origin": config["origin"],
        "platform": platform.platform(),
        "plan": str(config["plan_file"]),
        "plan_sha256": plan_sha256,
        "source": config["source"],
        "source_ref": source_ref,
        "started_at": checkpoint["started_at"],
    }
    write_json_atomic(out_dir / "run.json", run_record)

    metrics_path = out_dir / "metrics.jsonl"
    reader_group: ReaderGroup | None = None
    failure: str | None = None
    recon_record: dict[str, object] | None = None
    try:
        for item in plan[int(checkpoint["next_i"]) :]:
            index = int(item["i"])
            base_tip = item["base_tip"]
            if base_tip is not None:
                base_tip = str(base_tip)
            want_tip = str(item["want_tip"])
            current, current_error = remote_tip(str(config["origin"]), str(config["branch"]))
            if current_error:
                raise HarnessError("cannot read origin before step {}: {}".format(index, current_error))
            if current != base_tip:
                raise HarnessError(
                    "plan/server divergence before step {}: expected {!r}, got {!r}".format(
                        index, base_tip, current
                    )
                )
            require_success(
                git_at(driver, "cat-file", "-e", "{}^{{commit}}".format(want_tip)),
                "source object availability",
            )
            require_success(
                git_at(driver, "update-ref", "refs/heads/{}".format(config["branch"]), want_tip),
                "advance local driver ref",
            )
            command, forced = push_command(driver, str(config["branch"]), base_tip, want_tip)
            byte_estimate = estimate_bytes(driver, base_tip, want_tip)
            attempts = 0
            logical_success = False
            while attempts <= config["max_retries"]:
                attempts += 1
                kill_after_ms = (
                    config["kill_delay_ms"]
                    if config["kill_every"] and (index + 1) % config["kill_every"] == 0
                    else None
                )
                started_at = utc_now()
                start = time.perf_counter_ns()
                process, killed = run_push(command, kill_after_ms)
                push_ms = (time.perf_counter_ns() - start) / 1_000_000
                server_tip, tip_error = remote_tip(str(config["origin"]), str(config["branch"]))
                accepted = tip_error is None and server_tip == want_tip
                safely_unchanged = tip_error is None and server_tip == base_tip
                if accepted:
                    error = None if process.returncode == 0 else "client ended after server accepted tip"
                elif tip_error:
                    error = "cannot read server tip: {}".format(tip_error)
                elif safely_unchanged:
                    error = truncated(process.stderr) or "push did not advance server tip"
                else:
                    error = "server tip changed unexpectedly to {!r}".format(server_tip)
                metric: dict[str, object] = {
                    "accepted": accepted,
                    "attempt": attempts,
                    "base_tip": base_tip,
                    "batch": int(item["batch"]),
                    "bytes_sent_est": byte_estimate,
                    "client_killed": killed,
                    "command": command_text(command),
                    "dt_ms": round(push_ms, 3),
                    "error": error,
                    "forced": forced,
                    "i": index,
                    "n_commits": int(item["n_commits"]),
                    "phase_fetch_src_ms": 0.0,
                    "phase_push_ms": round(push_ms, 3),
                    "push_exit": process.returncode,
                    "server_tip_after": server_tip,
                    "t_end": utc_now(),
                    "t_start": started_at,
                    "want_tip": want_tip,
                }
                append_jsonl(metrics_path, metric)
                print(
                    "push-stress step {}/{} attempt {}: {}{}".format(
                        index + 1,
                        len(plan),
                        attempts,
                        "accepted" if accepted else "not accepted",
                        " (client killed)" if killed else "",
                    ),
                    flush=True,
                )
                if accepted:
                    logical_success = True
                    checkpoint = checkpoint_value(
                        plan_sha256=plan_sha256,
                        origin=str(config["origin"]),
                        source=str(config["source"]),
                        branch=str(config["branch"]),
                        source_ref=source_ref,
                        next_i=index + 1,
                        last_ok_tip=want_tip,
                        started_at=str(checkpoint["started_at"]),
                    )
                    write_json_atomic(out_dir / "checkpoint.json", checkpoint)
                    if reader_group is None and config["concurrency_readers"]:
                        reader_group = ReaderGroup(
                            config["concurrency_readers"],
                            config["work_dir"],
                            str(config["origin"]),
                            str(config["branch"]),
                            config["reader_interval_ms"],
                            out_dir / "reader-metrics.jsonl",
                        )
                        reader_group.start()
                    break
                if not safely_unchanged:
                    break
                if config["fail_fast"]:
                    break
            if not logical_success:
                failure = "step {} did not reach its planned tip".format(index)
                break

            if config["verify_every"] and (index + 1) % config["verify_every"] == 0:
                verified_tip, verify_error = remote_tip(str(config["origin"]), str(config["branch"]))
                if verify_error or verified_tip != want_tip:
                    failure = "periodic tip verification failed at step {}".format(index)
                    break
            if config["fsck_every"] and (index + 1) % config["fsck_every"] == 0:
                periodic = reconcile(
                    origin=str(config["origin"]),
                    branch=str(config["branch"]),
                    expected_tip=want_tip,
                    work_dir=config["work_dir"],
                    plan_sha256=plan_sha256,
                    server_recon_command=config["recon_command"],
                )
                write_json_atomic(out_dir / "recon-step-{}.json".format(index), periodic)
                if not periodic["pass"]:
                    failure = "periodic full recon failed at step {}".format(index)
                    break
            if config["pause_ms"]:
                time.sleep(config["pause_ms"] / 1000)
    except KeyboardInterrupt:
        failure = "interrupted; resume with --resume"
    except HarnessError as exc:
        failure = str(exc)
    finally:
        if reader_group is not None:
            reader_group.stop()

    if int(checkpoint["next_i"]) == len(plan):
        recon_record = reconcile(
            origin=str(config["origin"]),
            branch=str(config["branch"]),
            expected_tip=str(plan[-1]["want_tip"]),
            work_dir=config["work_dir"],
            plan_sha256=plan_sha256,
            server_recon_command=config["recon_command"],
        )
        write_json_atomic(out_dir / "recon.json", recon_record)
        if not recon_record["pass"]:
            failure = failure or "post-run recon failed"

    summary = write_summary(
        out_dir=out_dir,
        plan=plan,
        plan_sha256=plan_sha256,
        checkpoint=checkpoint,
        recon_record=recon_record,
    )
    print(json_compact(summary), flush=True)
    if failure:
        print("push-stress: {}".format(failure), file=sys.stderr)
        return 1
    return 0


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", help="read-only source Git repository")
    parser.add_argument("--origin", help="empty target bare repository or smart-HTTP URL")
    parser.add_argument("--work-dir", help="owned harness work directory; never cleaned automatically")
    parser.add_argument("--out-dir", help="artifact directory; defaults to WORK_DIR/out")
    parser.add_argument("--plan-file", help="plan artifact; defaults to OUT_DIR/plan.jsonl")
    parser.add_argument("--source-ref", help="source ref; defaults to source symbolic HEAD")
    parser.add_argument("--branch", help="target branch; default main")
    parser.add_argument("--order", choices=("first-parent", "topo", "explicit"))
    parser.add_argument("--explicit-file")
    parser.add_argument("--batch-mode", choices=("fixed", "ramp", "random", "list"))
    parser.add_argument("--batch-size", type=int)
    parser.add_argument("--batch-min", type=int)
    parser.add_argument("--batch-max", type=int)
    parser.add_argument("--batch-list")
    parser.add_argument("--random-seed", type=int)
    parser.add_argument("--max-commits", type=int)
    parser.add_argument("--max-pushes", type=int)
    parser.add_argument("--verify-every", type=int)
    parser.add_argument("--fsck-every", type=int)
    parser.add_argument("--pause-ms", type=int)
    parser.add_argument("--kill-every", type=int)
    parser.add_argument("--kill-delay-ms", type=int)
    parser.add_argument("--concurrency-readers", type=int)
    parser.add_argument("--reader-interval-ms", type=int)
    parser.add_argument("--max-retries", type=int)
    parser.add_argument("--recon-command", help="optional server-owned canonical blob reconciliation command")
    parser.add_argument("--resume", action="store_const", const=True, default=None)
    parser.add_argument("--no-resume", dest="resume", action="store_const", const=False)
    parser.add_argument("--fail-fast", action="store_const", const=True, default=None)
    parser.add_argument("--no-fail-fast", dest="fail_fast", action="store_const", const=False)
    parser.add_argument("--allow-nonempty-origin", action="store_const", const=True, default=None)
    parser.add_argument("--plan-only", action="store_true")
    parser.add_argument("--run-only", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--reuse-plan", action="store_true")
    return parser


def main(argv: list[str]) -> int:
    args = make_parser().parse_args(argv)
    if sum(bool(value) for value in (args.plan_only, args.run_only, args.verify_only)) > 1:
        print("push-stress: choose at most one of --plan-only/--run-only/--verify-only", file=sys.stderr)
        return 2
    try:
        config = config_from_args(args)
        validate_config(config, needs_origin=not args.plan_only)
        plan, plan_sha256, source_ref = make_or_load_plan(config, run_only=args.run_only or args.verify_only)
        if args.plan_only:
            print(
                json_compact(
                    {
                        "plan": str(config["plan_file"]),
                        "plan_sha256": plan_sha256,
                        "steps": len(plan),
                    }
                )
            )
            return 0
        if args.verify_only:
            recon_record = reconcile(
                origin=str(config["origin"]),
                branch=str(config["branch"]),
                expected_tip=str(plan[-1]["want_tip"]),
                work_dir=config["work_dir"],
                plan_sha256=plan_sha256,
                server_recon_command=config["recon_command"],
            )
            write_json_atomic(config["out_dir"] / "recon.json", recon_record)
            print(json_compact(recon_record))
            return 0 if recon_record["pass"] else 1
        return execute(config, plan, plan_sha256, source_ref)
    except (HarnessError, OSError, PlanError) as exc:
        print("push-stress: {}".format(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

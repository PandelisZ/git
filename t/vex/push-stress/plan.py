#!/usr/bin/env python3
"""Build deterministic, stock-Git push-stress plans.

The plan is deliberately independent of the target server.  It is a compact,
reviewable record of the ref tips that a later runner will push with ordinary
``git push`` commands.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
from typing import Iterable


class PlanError(RuntimeError):
    """A malformed plan request or source repository."""


def git(source: str, *args: str) -> str:
    process = subprocess.run(
        ["git", "-C", source, *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if process.returncode:
        raise PlanError(
            "git -C {} {} failed ({}): {}".format(
                source,
                " ".join(args),
                process.returncode,
                process.stderr.strip(),
            )
        )
    return process.stdout


def default_source_ref(source: str) -> str:
    process = subprocess.run(
        ["git", "-C", source, "symbolic-ref", "-q", "HEAD"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if process.returncode == 0 and process.stdout.strip():
        return process.stdout.strip()
    return "HEAD"


def source_commits(
    source: str,
    source_ref: str | None,
    order: str,
    explicit_file: str | None,
) -> tuple[list[str], str, str]:
    """Return oldest-to-newest commit OIDs, source ref, and resolved tip."""

    resolved_ref = source_ref or default_source_ref(source)
    tip = git(source, "rev-parse", "--verify", "{}^{{commit}}".format(resolved_ref)).strip()

    if order == "explicit":
        if not explicit_file:
            raise PlanError("--explicit-file is required with --order explicit")
        commits: list[str] = []
        for raw_line in Path(explicit_file).read_text(encoding="utf-8").splitlines():
            oid = raw_line.strip()
            if not oid or oid.startswith("#"):
                continue
            commits.append(
                git(source, "rev-parse", "--verify", "{}^{{commit}}".format(oid)).strip()
            )
        if not commits:
            raise PlanError("explicit plan file contains no commit OIDs")
        return commits, resolved_ref, tip

    args = ["rev-list", "--reverse"]
    if order == "first-parent":
        args.append("--first-parent")
    elif order == "topo":
        args.append("--topo-order")
    else:
        raise PlanError("unsupported order: {}".format(order))
    args.append(tip)
    commits = [line for line in git(source, *args).splitlines() if line]
    if not commits:
        raise PlanError("source ref {} has no commits".format(resolved_ref))
    return commits, resolved_ref, tip


def parse_batch_list(raw: str) -> list[int]:
    try:
        values = [int(item.strip()) for item in raw.split(",") if item.strip()]
    except ValueError as exc:
        raise PlanError("invalid --batch-list: {}".format(raw)) from exc
    if not values or any(value <= 0 for value in values):
        raise PlanError("--batch-list must contain positive integers")
    return values


def batch_schedule(
    mode: str,
    batch_size: int,
    batch_min: int,
    batch_max: int,
    batch_list: str,
    seed: int,
) -> Iterable[int]:
    if batch_size <= 0:
        raise PlanError("--batch-size must be positive")
    if batch_min <= 0 or batch_max <= 0 or batch_min > batch_max:
        raise PlanError("batch min/max must be positive and ordered")

    if mode == "fixed":
        while True:
            yield batch_size
    elif mode == "ramp":
        if batch_size > batch_max:
            raise PlanError("--batch-size cannot exceed --batch-max in ramp mode")
        current = batch_size
        while True:
            yield current
            current = min(current * 2, batch_max)
    elif mode == "random":
        rng = random.Random(seed)
        while True:
            yield rng.randint(batch_min, batch_max)
    elif mode == "list":
        values = parse_batch_list(batch_list)
        index = 0
        while True:
            yield values[index % len(values)]
            index += 1
    else:
        raise PlanError("unsupported batch mode: {}".format(mode))


def build_plan(
    *,
    source: str,
    source_ref: str | None,
    order: str,
    explicit_file: str | None,
    batch_mode: str,
    batch_size: int,
    batch_min: int,
    batch_max: int,
    batch_list: str,
    random_seed: int,
    max_commits: int,
    max_pushes: int,
) -> tuple[list[dict[str, object]], dict[str, object]]:
    commits, resolved_ref, tip = source_commits(source, source_ref, order, explicit_file)
    if max_commits < 0 or max_pushes < 0:
        raise PlanError("max-commits and max-pushes cannot be negative")
    if max_commits:
        commits = commits[:max_commits]
    if not commits:
        raise PlanError("commit cap selected no commits")

    schedule = batch_schedule(
        batch_mode, batch_size, batch_min, batch_max, batch_list, random_seed
    )
    plan: list[dict[str, object]] = []
    base_tip: str | None = None
    offset = 0
    while offset < len(commits) and (not max_pushes or len(plan) < max_pushes):
        requested = next(schedule)
        batch_commits = commits[offset : offset + requested]
        want_tip = batch_commits[-1]
        plan.append(
            {
                "base_tip": base_tip,
                "batch": len(batch_commits),
                "batch_mode": batch_mode,
                "commit_oids": batch_commits,
                "i": len(plan),
                "mode": order,
                "n_commits": len(batch_commits),
                "want_tip": want_tip,
            }
        )
        base_tip = want_tip
        offset += len(batch_commits)

    metadata: dict[str, object] = {
        "commit_count": len(commits),
        "plan_commit_count": sum(int(item["n_commits"]) for item in plan),
        "source_ref": resolved_ref,
        "source_tip": tip,
        "steps": len(plan),
    }
    return plan, metadata


def encode_plan(plan: list[dict[str, object]]) -> bytes:
    return b"".join(
        (
            json.dumps(item, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
            + "\n"
        ).encode("ascii")
        for item in plan
    )


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_plan(path: Path, plan: list[dict[str, object]]) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    content = encode_plan(plan)
    with tempfile.NamedTemporaryFile(
        mode="wb", dir=str(path.parent), prefix=".plan.", delete=False
    ) as handle:
        handle.write(content)
        handle.flush()
        os.fsync(handle.fileno())
        temporary = Path(handle.name)
    os.replace(temporary, path)
    return sha256_bytes(content)


def load_plan(path: Path) -> list[dict[str, object]]:
    plan: list[dict[str, object]] = []
    expected_index = 0
    with path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as exc:
                raise PlanError("invalid JSON at {}:{}".format(path, line_number)) from exc
            if not isinstance(item, dict) or item.get("i") != expected_index:
                raise PlanError("plan indices must be contiguous from zero")
            if not isinstance(item.get("want_tip"), str) or not isinstance(
                item.get("commit_oids"), list
            ):
                raise PlanError("plan line {} lacks required fields".format(line_number))
            plan.append(item)
            expected_index += 1
    if not plan:
        raise PlanError("plan is empty: {}".format(path))
    return plan


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--source", required=True, help="read-only source Git repository")
    result.add_argument("--output", required=True, help="plan.jsonl destination")
    result.add_argument("--source-ref", help="source ref; defaults to symbolic HEAD")
    result.add_argument("--order", default="first-parent", choices=("first-parent", "topo", "explicit"))
    result.add_argument("--explicit-file", help="one commit OID per line for explicit order")
    result.add_argument("--batch-mode", default="fixed", choices=("fixed", "ramp", "random", "list"))
    result.add_argument("--batch-size", type=int, default=1)
    result.add_argument("--batch-min", type=int, default=1)
    result.add_argument("--batch-max", type=int, default=64)
    result.add_argument("--batch-list", default="1")
    result.add_argument("--random-seed", type=int, default=1)
    result.add_argument("--max-commits", type=int, default=0)
    result.add_argument("--max-pushes", type=int, default=0)
    return result


def main(argv: list[str]) -> int:
    args = parser().parse_args(argv)
    try:
        plan, metadata = build_plan(
            source=args.source,
            source_ref=args.source_ref,
            order=args.order,
            explicit_file=args.explicit_file,
            batch_mode=args.batch_mode,
            batch_size=args.batch_size,
            batch_min=args.batch_min,
            batch_max=args.batch_max,
            batch_list=args.batch_list,
            random_seed=args.random_seed,
            max_commits=args.max_commits,
            max_pushes=args.max_pushes,
        )
        plan_sha256 = write_plan(Path(args.output), plan)
    except (OSError, PlanError) as exc:
        print("push-stress plan: {}".format(exc), file=sys.stderr)
        return 2

    metadata["plan"] = str(Path(args.output))
    metadata["plan_sha256"] = plan_sha256
    print(json.dumps(metadata, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

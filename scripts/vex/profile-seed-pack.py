#!/usr/bin/env python3

"""Measure the host-side pack work on the Vex git/git seed critical path.

This deliberately does not contact Compose, MySQL, or S3.  It answers a
narrow planning question before an expensive real seed: how much of a
five-minute budget would the current post-receive pack builder consume even if
all of its input were already a local packed mirror?

The ``current_builder_repack`` phase exactly matches the object-selection and
pack flags used by scripts/vex/build-pack.sh.  ``reuse_pack_candidate`` uses
Git's normal pack reuse behavior on the same set of object IDs.  The latter is
not a Vex result; it is a practical upper-bound candidate for a direct
incoming-pack architecture that retains and validates the received pack rather
than re-inflating every object and making a new pack.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MIRROR = REPO_ROOT / "t" / "vex" / "out" / "git-git-mirror.git"


class ProfileError(RuntimeError):
    """An invalid input or failed Git command."""


def fail(message: str) -> "None":
    raise ProfileError(message)


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def resolve_stock_git(value: str | None) -> str:
    candidates = [value, os.environ.get("VEX_STOCK_GIT"), "/usr/bin/git", shutil.which("git")]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return os.path.realpath(candidate)
    fail("stock Git executable is unavailable; pass --git or set VEX_STOCK_GIT")


def checked(args: list[str], action: str, **kwargs: Any) -> subprocess.CompletedProcess[bytes]:
    completed = subprocess.run(args, check=False, **kwargs)
    if completed.returncode:
        stderr = getattr(completed, "stderr", b"")
        if isinstance(stderr, bytes):
            detail = stderr.decode("utf-8", "replace").strip()
        else:
            detail = str(stderr).strip()
        fail("{} failed with exit {}{}".format(action, completed.returncode, ": " + detail if detail else ""))
    return completed


def git_command(git: str, mirror: Path, *arguments: str) -> list[str]:
    return [git, "--git-dir={}".format(mirror), *arguments]


def elapsed_phase(name: str, started: float, **fields: Any) -> dict[str, Any]:
    return {"name": name, "seconds": round(time.monotonic() - started, 6), **fields}


def stream_pack(git: str, mirror: Path, oids: Path, name: str, arguments: list[str]) -> dict[str, Any]:
    """Run pack-objects without retaining a second full pack on disk."""
    command = git_command(git, mirror, "pack-objects", "--stdout", "--revs", *arguments)
    started = time.monotonic()
    with oids.open("rb") as stdin:
        process = subprocess.Popen(
            command,
            stdin=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert process.stdout is not None
        transferred = 0
        while True:
            block = process.stdout.read(1024 * 1024)
            if not block:
                break
            transferred += len(block)
        assert process.stderr is not None
        stderr = process.stderr.read()
        returncode = process.wait()
    if returncode:
        fail(
            "{} failed with exit {}{}".format(
                name,
                returncode,
                ": " + stderr.decode("utf-8", "replace").strip() if stderr else "",
            )
        )
    return elapsed_phase(
        name,
        started,
        bytes=transferred,
        mib_per_second=round((transferred / (1024 * 1024)) / max(time.monotonic() - started, 0.000001), 3),
        command=command,
    )


def stream_existing_packs(mirror: Path) -> dict[str, Any]:
    """Read and hash the existing source packs: an I/O-only direct-pack floor."""
    pack_files = sorted((mirror / "objects" / "pack").glob("*.pack"))
    if not pack_files:
        fail("mirror has no packed objects under objects/pack")
    started = time.monotonic()
    total = 0
    files: list[dict[str, Any]] = []
    for pack_file in pack_files:
        digest = hashlib.sha256()
        size = 0
        with pack_file.open("rb") as handle:
            while True:
                block = handle.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
                size += len(block)
        total += size
        files.append({"name": pack_file.name, "bytes": size, "sha256": digest.hexdigest()})
    seconds = time.monotonic() - started
    return {
        "name": "existing_pack_read_hash",
        "seconds": round(seconds, 6),
        "bytes": total,
        "mib_per_second": round((total / (1024 * 1024)) / max(seconds, 0.000001), 3),
        "files": files,
    }


def write_json_atomically(output: Path, payload: dict[str, Any]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=output.parent, prefix=output.name + ".tmp-", delete=False
    ) as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temporary = Path(handle.name)
    os.replace(temporary, output)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Profile pack lower bounds for the Vex git/git seed critical path."
    )
    result.add_argument("--mirror", type=Path, default=DEFAULT_MIRROR, help="bare source mirror")
    result.add_argument("--output", type=Path, required=True, help="JSON result artifact")
    result.add_argument("--git", help="stock Git executable (default: VEX_STOCK_GIT or /usr/bin/git)")
    return result


def main() -> int:
    options = parser().parse_args()
    git = resolve_stock_git(options.git)
    mirror = options.mirror.resolve()
    output = options.output.resolve()
    if not mirror.is_dir():
        fail("--mirror is not a directory: {}".format(mirror))

    bare = checked(
        git_command(git, mirror, "rev-parse", "--is-bare-repository"),
        "inspect source mirror",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout.decode("utf-8", "replace").strip()
    if bare != "true":
        fail("--mirror is not a bare Git repository: {}".format(mirror))
    git_version = checked([git, "--version"], "read Git version", stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    phases: list[dict[str, Any]] = []
    phases.append(stream_existing_packs(mirror))
    with tempfile.TemporaryDirectory(prefix="vex-seed-pack-profile-") as temporary_directory:
        oids = Path(temporary_directory) / "reachable.oids"
        started = time.monotonic()
        with oids.open("wb") as handle:
            checked(
                git_command(git, mirror, "rev-list", "--objects", "--no-object-names", "--all"),
                "enumerate reachable objects",
                stdout=handle,
                stderr=subprocess.PIPE,
            )
        with oids.open("rb") as handle:
            object_count = sum(1 for _ in handle)
        phases.append(elapsed_phase("enumerate_reachable", started, objects=object_count))
        if not object_count:
            fail("source mirror has no reachable objects")
        phases.append(
            stream_pack(
                git,
                mirror,
                oids,
                "current_builder_repack",
                ["--no-reuse-object", "--no-reuse-delta", "--threads=1"],
            )
        )
        phases.append(stream_pack(git, mirror, oids, "reuse_pack_candidate", []))

    current = next(phase for phase in phases if phase["name"] == "current_builder_repack")
    reuse = next(phase for phase in phases if phase["name"] == "reuse_pack_candidate")
    payload: dict[str, Any] = {
        "schema": "vex-seed-pack-profile/v1",
        "captured_at": utc_now(),
        "scope": {
            "included": [
                "host mirror packed-byte scan",
                "reachable-object enumeration",
                "the exact pack-objects flags used by scripts/vex/build-pack.sh",
                "a normal Git reuse-pack comparison",
            ],
            "excluded": [
                "stock-Git HTTPS upload-pack negotiation and client pack generation",
                "Vex receive-pack/unpack-objects work",
                "S3/MinIO transfer latency and request count",
                "MySQL metadata transactions",
                "disk-control copy/repack, manifest generation, and Compose restarts",
            ],
            "interpretation": (
                "current_builder_repack is an optimistic lower bound for the current post-receive builder, "
                "because it reads a local packed mirror rather than Vex objects in S3. "
                "reuse_pack_candidate is a planning comparison only; a direct incoming-pack design should "
                "retain/validate the receive pack and its index instead of depending on this rebuild."
            ),
        },
        "source": {
            "mirror": str(mirror),
            "git": git,
            "git_version": git_version.stdout.decode("utf-8", "replace").strip(),
            "reachable_objects": object_count,
        },
        "phases": phases,
        "comparison": {
            "current_builder_seconds": current["seconds"],
            "reuse_candidate_seconds": reuse["seconds"],
            "current_to_reuse_ratio": round(
                float(current["seconds"]) / max(float(reuse["seconds"]), 0.000001), 3
            ),
            "five_minute_seed_budget_seconds": 300,
            "remaining_budget_after_current_builder_seconds": round(300 - float(current["seconds"]), 6),
        },
    }
    write_json_atomically(output, payload)
    print(
        "vex seed-pack-profile: PASS objects={} current_builder_seconds={} "
        "reuse_candidate_seconds={} output={}".format(
            object_count, current["seconds"], reuse["seconds"], output
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ProfileError as error:
        print("vex seed-pack-profile: {}".format(error), file=sys.stderr)
        raise SystemExit(2)

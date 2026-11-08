#!/usr/bin/env python3
"""Fail when a document references a benchmark archive that is not checked in.

The repeated documentation failure in this repository is a cited
``bench-results/<stamp>_slo`` directory that does not exist in the checkout,
or exists only on the author's disk.  A number whose archive is missing from
git cannot be audited by someone who clones the repository, so this gate scans
the prose (README, docs/, resume) and rejects both dangling paths and archives
that were never staged/committed.

Usage:
  python3 scripts/check_bench_refs.py
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

ARCHIVE_RE = re.compile(r"bench-results/[0-9]{8}_[0-9]{6}_slo")


def tracked_files(root: Path, ref: str) -> list[str]:
    """Return the git-tracked files under *ref* (empty when untracked)."""
    if not shutil.which("git") or not (root / ".git").exists():
        return []
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--", ref],
        check=False,
        capture_output=True,
        text=True,
    )
    return [line for line in result.stdout.splitlines() if line.strip()]


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    sources = [root / "README.md", root / "resume.md"]
    sources.extend(sorted((root / "docs").glob("*.md")))

    missing: list[tuple[Path, str, str]] = []
    seen: set[str] = set()
    for path in sources:
        if not path.is_file():
            continue
        for match in ARCHIVE_RE.finditer(path.read_text(encoding="utf-8")):
            ref = match.group(0)
            if ref in seen:
                continue
            seen.add(ref)
            if not (root / ref).is_dir():
                missing.append((path.relative_to(root), ref, "missing"))
            elif (root / ".git").exists() and not tracked_files(root, ref):
                missing.append((path.relative_to(root), ref, "untracked"))

    if not missing:
        print(f"bench archive references: OK ({len(seen)} unique path(s), tracked)")
        return 0

    for doc, ref, kind in missing:
        if kind == "missing":
            print(f"{doc}: missing archive {ref}", file=sys.stderr)
        else:
            print(
                f"{doc}: archive {ref} exists on disk but is not in git; "
                f"run `git add {ref}`",
                file=sys.stderr,
            )
    print(
        "Either add the archive to git or remove the citation; "
        "numbers without a committed archive are not evidence.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())

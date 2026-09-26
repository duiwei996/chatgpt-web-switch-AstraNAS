#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def fail(message: str) -> None:
    raise SystemExit(f"version check failed: {message}")


def parse_version(text: str, pattern: str, label: str) -> str:
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        fail(f"cannot find {label} version")
    version = match.group(1)
    if not SEMVER.fullmatch(version):
        fail(f"invalid {label} semantic version: {version}")
    return version


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def version_tuple(value: str):
    match = SEMVER.fullmatch(value)
    assert match
    return tuple(int(part) for part in match.groups())


def allowed_next(previous: str, current: str) -> bool:
    pmaj, pmin, ppatch = version_tuple(previous)
    cmaj, cmin, cpatch = version_tuple(current)
    return (
        (cmaj, cmin, cpatch) == (pmaj, pmin, ppatch + 1)
        or (cmaj, cmin, cpatch) == (pmaj, pmin + 1, 0)
        or (cmaj, cmin, cpatch) == (pmaj + 1, 0, 0)
    )


def current_version() -> str:
    cmake = parse_version(
        read("CMakeLists.txt"),
        r"project\(astranas VERSION ([0-9]+\.[0-9]+\.[0-9]+)",
        "CMake",
    )
    constants = parse_version(
        read("source/app/constants.hpp"),
        r'kVersion\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"',
        "runtime",
    )
    workflow = parse_version(
        read(".github/workflows/build-nro.yml"),
        r'ASTRANAS_VERSION:\s*"([0-9]+\.[0-9]+\.[0-9]+)"',
        "workflow",
    )
    readme = parse_version(
        read("README.md"), r"^# AstraNAS v([0-9]+\.[0-9]+\.[0-9]+)$", "README"
    )
    config = parse_version(
        read("config.example.ini"),
        r"^# AstraNAS v([0-9]+\.[0-9]+\.[0-9]+)$",
        "config example",
    )
    versions = {cmake, constants, workflow, readme, config}
    if len(versions) != 1:
        fail("version strings are inconsistent: " + ", ".join(sorted(versions)))
    return cmake


def git_output(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=ROOT, text=True, stderr=subprocess.DEVNULL
    ).strip()


def parent_version() -> str | None:
    try:
        fields = git_output("rev-list", "--parents", "-n", "1", "HEAD").split()
        if len(fields) < 2:
            return None
        text = subprocess.check_output(
            ["git", "show", f"{fields[1]}:CMakeLists.txt"],
            cwd=ROOT,
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    return parse_version(
        text,
        r"project\(astranas VERSION ([0-9]+\.[0-9]+\.[0-9]+)",
        "parent CMake",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--require-parent-bump", action="store_true")
    parser.add_argument(
        "--initial-version",
        help="allow this exact version only when HEAD is a parentless initial commit",
    )
    args = parser.parse_args()
    current = current_version()
    if args.require_parent_bump:
        previous = parent_version()
        if previous is None:
            if not args.initial_version:
                fail("previous version is unavailable")
            if not SEMVER.fullmatch(args.initial_version):
                fail(f"invalid initial version: {args.initial_version}")
            if current != args.initial_version:
                fail(
                    f"parentless initial commit must use {args.initial_version}; "
                    f"found {current}"
                )
            parents = git_output("rev-list", "--parents", "-n", "1", "HEAD").split()
            if len(parents) != 1:
                fail("initial version requires a verified parentless HEAD")
            print(f"initial version: {current}")
            return
        if not allowed_next(previous, current):
            fail(
                f"{previous} -> {current} is not a single semantic-version step; "
                "use patch +1 for fixes, minor +1 with patch reset for compatible features, "
                "or major +1 with minor/patch reset for breaking changes"
            )
        print(f"version bump: {previous} -> {current}")
    else:
        print(f"version consistency: {current}")


if __name__ == "__main__":
    main()

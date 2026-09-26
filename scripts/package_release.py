#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create AstraNAS delivery metadata and the corresponding source archive."""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import zipfile
from pathlib import Path

SKIP_DIRS = {".git", "build", "dist", "release", "__pycache__", ".pytest_cache"}
SKIP_SUFFIXES = {".pyc", ".o", ".d", ".elf", ".nro", ".nacp", ".map", ".log", ".pem", ".key", ".p12", ".pfx"}
SKIP_NAMES = {"config.ini", "config.local.ini"}


def should_include(root: Path, path: Path) -> bool:
    rel = path.relative_to(root)
    if any(part in SKIP_DIRS for part in rel.parts):
        return False
    if path.name == ".git" or path.name in SKIP_NAMES or path.name.startswith(".env"):
        return False
    if path.name.startswith("id_rsa") or path.name.startswith("id_ed25519"):
        return False
    if path.suffix.lower() in SKIP_SUFFIXES:
        return False
    return path.is_file()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_sums(output: Path, root: Path, files: list[Path]) -> None:
    lines = [f"{sha256_file(path)}  {path.relative_to(root).as_posix()}" for path in files]
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    dist = args.dist.resolve()
    dist.mkdir(parents=True, exist_ok=True)

    main_nro = dist / "switch/AstraNAS/AstraNAS.nro"
    netdiag_nro = dist / "switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro"
    for path in (main_nro, netdiag_nro):
        if not path.is_file() or path.stat().st_size == 0:
            raise SystemExit(f"missing build output: {path}")

    source_zip = dist / "AstraNAS-source.zip"
    source_files = sorted(
        (path for path in root.rglob("*") if should_include(root, path)),
        key=lambda path: path.relative_to(root).as_posix(),
    )
    with zipfile.ZipFile(source_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        prefix = Path(f"AstraNAS-v{args.version}-source")
        for path in source_files:
            archive.write(path, (prefix / path.relative_to(root)).as_posix())

    config_copy = dist / "config.example.ini"
    shutil.copy2(root / "config.example.ini", config_copy)

    manifest = dist / "BUILD-MANIFEST.txt"
    manifest_lines = [
        "project=AstraNAS",
        f"version={args.version}",
        f"commit={args.commit}",
        f"source_files={len(source_files)}",
        "source_archive=AstraNAS-source.zip",
        f"toolchain_bridge_repo={os.environ.get('TOOLCHAIN_BRIDGE_REPO', '')}",
        f"toolchain_bridge_tag={os.environ.get('TOOLCHAIN_BRIDGE_TAG', '')}",
        f"toolchain_archive_sha256={os.environ.get('TOOLCHAIN_ARCHIVE_SHA256', '')}",
        f"libsmb2_commit={os.environ.get('LIBSMB2_COMMIT', '')}",
        f"libusbhsfs_commit={os.environ.get('LIBUSBHSFS_COMMIT', '')}",
    ]
    manifest.write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")

    write_sums(dist / "NRO-SHA256SUMS.txt", dist, [main_nro, netdiag_nro])
    write_sums(
        dist / "SHA256SUMS.txt",
        dist,
        [main_nro, netdiag_nro, source_zip, config_copy, manifest],
    )
    print(f"release package metadata: PASS ({len(source_files)} source files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

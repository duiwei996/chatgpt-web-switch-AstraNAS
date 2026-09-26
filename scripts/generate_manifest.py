#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate library.json from filenames: Name [16-hex-title-id][v123].ext"""
import argparse
import hashlib
import json
import re
from pathlib import Path

PATTERN = re.compile(r"^\s*(.*?)\s*\[([0-9A-Fa-f]{16})\](?:\[v([0-9]+)\])?.*$")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path, help="Folder to scan")
    parser.add_argument("-o", "--output", type=Path, default=Path("library.json"))
    parser.add_argument("--sha256", action="store_true", help="Include SHA-256 for every matched file (slower on large libraries)")
    args = parser.parse_args()
    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f"not a directory: {root}")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    items = []
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        if path.resolve() == output:
            continue
        match = PATTERN.match(path.name)
        if not match:
            continue
        name, title_id, version = match.groups()
        item = {
            "title_id": title_id.upper(),
            "name": name.strip() or path.stem,
            "version": int(version or 0),
            "size": path.stat().st_size,
            "type": "content",
            "path": path.relative_to(root).as_posix(),
        }
        if args.sha256:
            item["sha256"] = sha256_file(path)
        items.append(item)
    output.write_text(json.dumps({"items": items}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(items)} items to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

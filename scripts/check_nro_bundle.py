#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate AstraNAS' ready-to-copy hbmenu layout and embedded NRO metadata."""
from __future__ import annotations

import argparse
from pathlib import Path

NRO_MAGIC = b"NRO0"
ASET_MAGIC = b"ASET"
NACP_SIZE = 0x4000
LANG_ENTRY_SIZE = 0x300
LANG_COUNT = 16
NAME_SIZE = 0x200
VERSION_OFFSET = 0x3060
VERSION_SIZE = 0x10


def c_string(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("utf-8", "strict")


def parse_nro(path: Path, expected_version: str, expected_title: str) -> None:
    data = path.read_bytes()
    if len(data) < 0x80:
        raise ValueError(f"{path}: NRO 文件过小")
    if data[0x10:0x14] != NRO_MAGIC:
        raise ValueError(f"{path}: 缺少 NRO0 头")

    nro_size = int.from_bytes(data[0x18:0x1C], "little")
    if nro_size <= 0 or nro_size + 0x38 > len(data):
        raise ValueError(f"{path}: NRO size/ASET 偏移无效")
    asset = data[nro_size : nro_size + 0x38]
    if asset[:4] != ASET_MAGIC:
        raise ValueError(f"{path}: 缺少 ASET 资源头")

    nacp_offset = int.from_bytes(asset[0x18:0x20], "little")
    nacp_size = int.from_bytes(asset[0x20:0x28], "little")
    if nacp_size != NACP_SIZE:
        raise ValueError(f"{path}: NACP 大小不是 0x4000")
    start = nro_size + nacp_offset
    end = start + nacp_size
    if start < nro_size + 0x38 or end > len(data):
        raise ValueError(f"{path}: NACP 范围越界")
    nacp = data[start:end]

    names = []
    for index in range(LANG_COUNT):
        off = index * LANG_ENTRY_SIZE
        name = c_string(nacp[off : off + NAME_SIZE])
        if not name:
            raise ValueError(f"{path}: NACP 语言槽 {index} 的应用标题为空")
        names.append(name)
    if expected_title not in names:
        raise ValueError(f"{path}: NACP 未包含期望标题 {expected_title!r}")

    version = c_string(nacp[VERSION_OFFSET : VERSION_OFFSET + VERSION_SIZE])
    if version != expected_version:
        raise ValueError(f"{path}: NACP 版本 {version!r} != {expected_version!r}")


def main() -> None:
    parser = argparse.ArgumentParser(description="检查 AstraNAS hbmenu 发布布局")
    parser.add_argument("root", type=Path, help="包含 switch/ 的发布根目录")
    parser.add_argument("version")
    args = parser.parse_args()

    apps = [
        ("AstraNAS", "AstraNAS.nro", "Switch游戏安装"),
        ("AstraNAS-NetDiag", "AstraNAS-NetDiag.nro", "Switch网络测速"),
    ]
    for folder, filename, title in apps:
        app_dir = args.root / "switch" / folder
        expected = app_dir / filename
        if not expected.is_file():
            raise SystemExit(f"发布布局缺少 {expected.relative_to(args.root)}")
        # hbmenu 会优先检查 <目录>/<目录>.nro；标准入口旁边存在额外 NRO
        # 不会阻止该入口被识别，因此发布门禁不得拒绝用户附加工具或旧版备份。
        if expected.stem != folder:
            raise SystemExit(f"{expected.relative_to(args.root)} 文件名必须与目录名一致")
        parse_nro(expected, args.version, title)

    print("hbmenu/NRO bundle check: PASS")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fill empty NACP language-title slots, either in a raw NACP or an NRO asset section."""
from __future__ import annotations

import argparse
from pathlib import Path

NACP_SIZE = 0x4000
LANG_ENTRY_SIZE = 0x300
LANG_COUNT = 16
NAME_SIZE = 0x200
NRO_MAGIC = b"NRO0"
ASET_MAGIC = b"ASET"


def entry_name(data: bytes | bytearray, index: int) -> bytes:
    off = index * LANG_ENTRY_SIZE
    return bytes(data[off : off + NAME_SIZE]).split(b"\0", 1)[0]


def fill_languages(data: bytes) -> bytes:
    if len(data) != NACP_SIZE:
        raise ValueError(f"NACP 大小必须为 0x{NACP_SIZE:x}，实际为 0x{len(data):x}")

    fallback = None
    for index in range(LANG_COUNT):
        if entry_name(data, index):
            off = index * LANG_ENTRY_SIZE
            fallback = data[off : off + LANG_ENTRY_SIZE]
            break
    if fallback is None:
        raise ValueError("NACP 没有任何可用的应用标题")

    out = bytearray(data)
    for index in range(LANG_COUNT):
        if entry_name(out, index):
            continue
        off = index * LANG_ENTRY_SIZE
        out[off : off + LANG_ENTRY_SIZE] = fallback

    missing = [index for index in range(LANG_COUNT) if not entry_name(out, index)]
    if missing:
        raise ValueError(f"NACP 仍有空标题语言槽：{missing}")
    return bytes(out)


def fill_nro_languages(data: bytes) -> bytes:
    if len(data) < 0x80 or data[0x10:0x14] != NRO_MAGIC:
        raise ValueError("不是有效的 NRO0 文件")
    nro_size = int.from_bytes(data[0x18:0x1C], "little")
    if nro_size <= 0 or nro_size + 0x38 > len(data):
        raise ValueError("NRO size/ASET 偏移无效")
    asset = data[nro_size : nro_size + 0x38]
    if asset[:4] != ASET_MAGIC:
        raise ValueError("NRO 缺少 ASET 资源头")
    nacp_offset = int.from_bytes(asset[0x18:0x20], "little")
    nacp_size = int.from_bytes(asset[0x20:0x28], "little")
    if nacp_size != NACP_SIZE:
        raise ValueError(f"NRO 内 NACP 大小不是 0x{NACP_SIZE:x}")
    start = nro_size + nacp_offset
    end = start + nacp_size
    if start < nro_size + 0x38 or end > len(data):
        raise ValueError("NRO 内 NACP 范围越界")
    out = bytearray(data)
    out[start:end] = fill_languages(data[start:end])
    if len(out) != len(data):
        raise AssertionError("补全语言槽不得改变 NRO 文件长度")
    return bytes(out)


def main() -> None:
    parser = argparse.ArgumentParser(description="补全 NACP 的空语言标题槽")
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path, nargs="?")
    parser.add_argument("--nro", action="store_true", help="处理 NRO 中嵌入的 NACP")
    args = parser.parse_args()

    raw = args.source.read_bytes()
    result = fill_nro_languages(raw) if args.nro else fill_languages(raw)
    destination = args.destination or args.source
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)


if __name__ == "__main__":
    main()

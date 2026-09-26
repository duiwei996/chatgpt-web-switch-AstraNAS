#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""回归测试：额外 NRO 不得让 AstraNAS 的标准 hbmenu 入口失效。"""
from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

NRO_MAGIC = b"NRO0"
ASET_MAGIC = b"ASET"
NACP_SIZE = 0x4000
LANG_ENTRY_SIZE = 0x300
NAME_SIZE = 0x200
VERSION_OFFSET = 0x3060

ROOT = Path(__file__).resolve().parents[1]


def make_nro(path: Path, title: str, version: str) -> None:
    nro_size = 0x100
    nacp_offset = 0x40
    data = bytearray(nro_size + nacp_offset + NACP_SIZE)
    data[0x10:0x14] = NRO_MAGIC
    data[0x18:0x1C] = nro_size.to_bytes(4, "little")
    data[nro_size:nro_size + 4] = ASET_MAGIC
    data[nro_size + 0x18:nro_size + 0x20] = nacp_offset.to_bytes(8, "little")
    data[nro_size + 0x20:nro_size + 0x28] = NACP_SIZE.to_bytes(8, "little")
    start = nro_size + nacp_offset
    title_bytes = title.encode("utf-8")
    for index in range(16):
        off = start + index * LANG_ENTRY_SIZE
        data[off:off + len(title_bytes)] = title_bytes
    version_bytes = version.encode("utf-8")
    data[start + VERSION_OFFSET:start + VERSION_OFFSET + len(version_bytes)] = version_bytes
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


with tempfile.TemporaryDirectory() as td:
    dist = Path(td)
    make_nro(dist / "switch/AstraNAS/AstraNAS.nro", "Switch游戏安装", "1.1.9")
    make_nro(dist / "switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro", "Switch网络测速", "1.1.9")

    # 这是旧门禁会错误拒绝的布局；标准 AstraNAS.nro 仍存在，
    # 因此额外 NRO 应当被允许。
    (dist / "switch/AstraNAS/AstraNAS-NetDiag.nro").write_bytes(b"extra")
    (dist / "switch/AstraNAS/AstraNAS-v1.1.2.nro").write_bytes(b"backup")

    subprocess.run(
        ["python3", str(ROOT / "scripts/check_nro_bundle.py"), str(dist), "1.1.9"],
        check=True,
    )

print("hbmenu multi-NRO layout regression: PASS")

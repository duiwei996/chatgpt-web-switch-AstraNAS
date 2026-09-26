#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fill_nacp_languages", ROOT / "scripts" / "fill_nacp_languages.py")
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

name = "Switch游戏安装".encode("utf-8")
author = "AstraNAS Project".encode("utf-8")
nacp = bytearray(module.NACP_SIZE)
nacp[0:len(name)] = name
nacp[module.NAME_SIZE:module.NAME_SIZE + len(author)] = author
filled = module.fill_languages(bytes(nacp))
assert len(filled) == module.NACP_SIZE
for index in range(module.LANG_COUNT):
    off = index * module.LANG_ENTRY_SIZE
    assert filled[off:off + module.NAME_SIZE].split(b"\0", 1)[0] == name

nro_size = 0x100
nacp_offset = 0x40
nro = bytearray(nro_size + nacp_offset + module.NACP_SIZE)
nro[0x10:0x14] = module.NRO_MAGIC
nro[0x18:0x1C] = nro_size.to_bytes(4, "little")
nro[nro_size:nro_size + 4] = module.ASET_MAGIC
nro[nro_size + 0x18:nro_size + 0x20] = nacp_offset.to_bytes(8, "little")
nro[nro_size + 0x20:nro_size + 0x28] = module.NACP_SIZE.to_bytes(8, "little")
start = nro_size + nacp_offset
nro[start:start + module.NACP_SIZE] = nacp
patched = module.fill_nro_languages(bytes(nro))
assert len(patched) == len(nro)
for index in range(module.LANG_COUNT):
    off = start + index * module.LANG_ENTRY_SIZE
    assert patched[off:off + module.NAME_SIZE].split(b"\0", 1)[0] == name
print("NACP language fallback test: PASS")

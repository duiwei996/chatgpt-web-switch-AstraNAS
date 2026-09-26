#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static guardrails for AstraNAS' Chinese graphical and touch UI."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
paths = [
    ROOT / 'source/main.cpp', ROOT / 'source/app/constants.hpp', ROOT / 'source/app/runtime.cpp', ROOT / 'source/app/runtime_services.cpp', ROOT / 'source/app/runtime_files.cpp', ROOT / 'source/app/runtime_library.cpp', ROOT / 'source/app/action_selection.cpp', ROOT / 'source/app/action_transfer.cpp', ROOT / 'source/app/action_install.cpp',
    ROOT / 'source/ui/gui.cpp', ROOT / 'source/ui/gui.hpp', ROOT / 'source/ui/input.cpp', ROOT / 'source/ui/input.hpp',
    ROOT / 'source/ui/pages.cpp', ROOT / 'source/ui/dialogs.cpp', ROOT / 'source/installed/catalog.cpp',
    ROOT / 'source/netdiag/main.cpp', ROOT / 'source/netdiag/ui.cpp', ROOT / 'source/netdiag/ui.hpp',
    *sorted((ROOT / 'source/netdiag/ui_parts').glob('ui_part_*.inc')),
]
errors=[]
texts={}
for path in paths:
    if not path.is_file():
        errors.append(f'missing UI source: {path.relative_to(ROOT)}'); continue
    raw=path.read_bytes()
    try: text=raw.decode('utf-8','strict')
    except UnicodeDecodeError as exc:
        errors.append(f'invalid UTF-8 in {path.relative_to(ROOT)}: {exc}'); continue
    if '\x00' in text or '\ufffd' in text: errors.append(f'invalid text marker in {path.relative_to(ROOT)}')
    for risky in ['•','✓']:
        if risky in text: errors.append(f'unsupported-risk glyph {risky!r} in {path.relative_to(ROOT)}')
    texts[path]=text
all_text='\n'.join(texts.values())
for needle,reason in {
    'consoleInit(':'must not use libnx text console for the main UI',
    'consoleClear(':'must not use libnx text console for the main UI',
    'consoleUpdate(':'must not use libnx text console for the main UI',
    '重新读取配置':'obsolete settings action',
    '保存并重新连接':'obsolete settings action',
    'ftp://':'FTP must not be exposed',
    'ftps://':'FTP must not be exposed',
}.items():
    if needle in all_text: errors.append(f'forbidden UI/runtime marker: {reason}: {needle}')
for needle in ['Switch游戏安装','NAS 文件','本机文件','安装缓存','已安装','设置','测试连接','自动发现','选择“保存 SMB 配置”后才写入','正在下载到本机当前目录','上传到 NAS','Switch网络测速','开始选中测试','一键基础诊断','清空结果','纯 RAM 分层诊断','安装日志上传目录','测速日志上传目录']:
    if needle not in all_text: errors.append(f'missing Chinese UI marker: {needle}')
for needle in ['framebufferCreate','PlSharedFontType_ChineseSimplified','PlSharedFontType_ExtChineseSimplified','PlSharedFontType_Standard','FT_New_Memory_Face','FT_Get_Char_Index','image_rgb']:
    if needle not in texts.get(ROOT/'source/ui/gui.cpp','') + texts.get(ROOT/'source/ui/gui.hpp',''): errors.append(f'missing graphical marker: {needle}')
input_text=texts.get(ROOT/'source/ui/input.cpp','') + texts.get(ROOT/'source/ui/input.hpp','')
for needle in ['padGetStickPos','stick_directions(pad,0)','stick_directions(pad,1)','hidGetTouchScreenStates','PadRepeater','stick_down']:
    if needle not in input_text: errors.append(f'missing input marker: {needle}')
runtime_text=texts.get(ROOT/'source/app/runtime.cpp','')
for needle in ['frame.down & HidNpadButton_L','frame.down & HidNpadButton_R']:
    if needle not in runtime_text: errors.append(f'missing shoulder tab switch marker: {needle}')
for needle in ['frame.nav & HidNpadButton_Left','frame.nav & HidNpadButton_Right']:
    if needle in runtime_text: errors.append(f'left/right navigation must not switch tabs: {needle}')
for needle in ['last_updated','NsApplicationControlData','load_icon_rgb']:
    if needle not in texts.get(ROOT/'source/installed/catalog.cpp',''): errors.append(f'missing installed cache marker: {needle}')
main_lines=len(texts.get(ROOT/'source/main.cpp','').splitlines())
if main_lines > 80: errors.append(f'source/main.cpp should stay a thin entrypoint, got {main_lines} lines')
runtime_header=(ROOT/'source/app/runtime_internal.hpp').read_text(encoding='utf-8') if (ROOT/'source/app/runtime_internal.hpp').is_file() else ''
if 'struct ActionContext;' not in runtime_header: errors.append('runtime internal header must forward-declare ActionContext')
runtime_helpers=(ROOT/'source/app/runtime_helpers.hpp').read_text(encoding='utf-8') if (ROOT/'source/app/runtime_helpers.hpp').is_file() else ''
for needle in ['inline std::vector<LocalLocation> local_locations','inline std::string local_location_label','inline void clamp_scroll','inline std::size_t visible_begin','inline bool path_looks_like_address_only']:
    if needle not in runtime_helpers: errors.append(f'runtime helper in header must be inline: {needle}')
action_install=texts.get(ROOT/'source/app/action_install.cpp','')
for needle in ['using astranas::ui::Gui;','namespace palette = astranas::ui::palette;']:
    if needle not in action_install: errors.append(f'malformed action install UI alias: missing {needle}')
for needle in ['void initialize_input();','void loop();','void shutdown();','bool connect_remote(bool test_only = false);']:
    if needle not in runtime_header: errors.append(f'malformed runtime declaration: missing {needle}')
for rel in ['source/app/runtime.cpp','source/app/runtime_services.cpp','source/app/runtime_files.cpp','source/app/runtime_library.cpp']:
    if 'private:' in texts.get(ROOT/rel,''): errors.append(f'malformed runtime implementation leaked class syntax: {rel}')
for rel in ['source/app/runtime.cpp','source/app/runtime_services.cpp','source/app/runtime_files.cpp','source/app/runtime_library.cpp','source/app/action_selection.cpp','source/app/action_transfer.cpp','source/app/action_install.cpp','source/ui/pages.cpp','source/ui/dialogs.cpp']:
    count=len(texts.get(ROOT/rel,'').splitlines())
    if count > 1100: errors.append(f'{rel} exceeds modularity soft gate: {count} lines')
if errors:
    print('UI localization check: FAIL', file=sys.stderr)
    for error in errors: print(f'- {error}', file=sys.stderr)
    raise SystemExit(1)
print('UI localization check: PASS')

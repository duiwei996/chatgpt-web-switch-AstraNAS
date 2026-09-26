#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
CXX="${CXX:-g++}"
netdiag_sources=(source/netdiag/ui.cpp source/netdiag/ui_parts/*.inc)
python3 scripts/check_ui_localization.py
python3 scripts/check_version.py
python3 tests/nacp_language_test.py
python3 tests/hbmenu_layout_test.py
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/batch_queue_touch_test.cpp -Isource -o /tmp/AstraNAS-batch-queue-touch-test
/tmp/AstraNAS-batch-queue-touch-test
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/batch_install_order_test.cpp -Isource -o /tmp/AstraNAS-batch-install-order-test
/tmp/AstraNAS-batch-install-order-test
# Direct-install policy: enabled means real direct install only. Do not silently
# fall back to cache, and do not force SMB signing at the client.
! grep -q 'smb2_set_sign(ctx_,1)' source/remote/smb_client.cpp
! grep -q '网络直装回退缓存' source/app/action_install.cpp
grep -q 'if (!config.network_direct_install || kind == PackageContainerKind::HomebrewNro)' source/app/action_install.cpp
grep -q '服务器未提供强 ETag' source/remote/curl_client.cpp source/remote/curl_stream.cpp
grep -q '服务器忽略 Range 请求' source/remote/curl_client.cpp source/remote/curl_stream.cpp
grep -q 'padGetButtons(&ctx.pad)' source/app/action_install.cpp
grep -q 'padGetButtons(&ctx.pad)' source/app/action_transfer.cpp
grep -q 'kInstallReadAheadSlots = 128u' source/title_backend/buffered_stream.hpp
grep -q 'kInstallWriteSliceSize = 1u \* 1024u \* 1024u' source/title_backend/buffered_stream.hpp
grep -q 'std::chrono::milliseconds(25)' source/title_backend/buffered_stream.cpp
grep -q 'appletGetAppletType() == AppletType_LibraryApplet' source/app/runtime.cpp
grep -q 'Applet 模式不允许运行 AstraNAS' source/app/runtime.cpp
grep -q '1 GiB 环形预读缓冲' source/app/runtime.cpp
grep -q '无法分配 1 GiB 网络直装预读缓冲' source/title_backend/buffered_stream.cpp
grep -q 'smb2_pread_async' source/remote/smb_client.cpp
grep -q 'stream_contiguous' source/title_backend/buffered_stream.cpp
grep -q 'stream_buffered_read_ahead' source/title_backend/buffered_stream.cpp source/atmo_xl/source/install/sdmc_nsp.cpp source/atmo_xl/source/install/sdmc_xci.cpp
grep -q 'kSmbPipelineWindow = 8u' source/remote/smb_client.cpp
grep -q '0x100000, 0x100000, 0x400000, 0x400000, 8, 4' source/network_runtime.cpp
grep -q 'AppletWirelessPriorityMode_OptimizedForWlan' source/network_runtime.cpp
grep -q 'ApmCpuBoostMode_FastLoad' source/network_runtime.cpp
grep -q 'CURLOPT_BUFFERSIZE, 1024L \* 1024L' source/remote/curl_client.cpp
grep -q 'socket_recv_buffer' source/remote/remote_client.hpp source/remote/curl_stream.cpp source/remote/smb_client.cpp
grep -q 'receive_callback_count' source/remote/remote_client.hpp source/remote/curl_stream.cpp
grep -q 'source_backpressure_ns' source/install_performance.hpp source/install_performance.cpp
grep -q 'config_version=3' config.example.ini
grep -q 'AstraNAS-NetDiag' CMakeLists.txt
netdiag_link_block="$(sed -n '/target_link_libraries(AstraNAS-NetDiag PRIVATE/,/^)/p' CMakeLists.txt)"
for lib in freetype harfbuzz jpeg png bz2 z; do
  grep -q "    ${lib}" <<<"$netdiag_link_block"
done
grep -q 'Switch网络测速' CMakeLists.txt "${netdiag_sources[@]}"
grep -q 'HTTP 连续 Range' "${netdiag_sources[@]}"
grep -q 'frame.nav_once & HidNpadButton_Left' "${netdiag_sources[@]}"
grep -q 'frame.nav_once & HidNpadButton_Right' "${netdiag_sources[@]}"
! grep -q 'HidNpadButton_B' "${netdiag_sources[@]}"
! grep -Eq 'HidNpadButton_(X|Y|R)([^A-Za-z_]|$)' "${netdiag_sources[@]}"
grep -q 'frame.nav_once&HidNpadButton_Up' source/ui/dialogs.cpp
grep -q 'frame.nav_once&HidNpadButton_Down' source/ui/dialogs.cpp
test -s assets/netdiag_icon.jpg
main_nro_block="$(sed -n '/nx_create_nro(AstraNAS$/,/^)/p' CMakeLists.txt)"
netdiag_nro_block="$(sed -n '/nx_create_nro(AstraNAS-NetDiag$/,/^)/p' CMakeLists.txt)"
grep -q 'assets/icon.jpg' <<<"$main_nro_block"
grep -q 'assets/netdiag_icon.jpg' <<<"$netdiag_nro_block"
! grep -q 'assets/icon.jpg' <<<"$netdiag_nro_block"
grep -q 'dist/switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro' scripts/build_switch.sh scripts/build_in_devkitpro_docker.sh .github/workflows/build-nro.yml
! grep -q 'dist/switch/AstraNAS/AstraNAS-NetDiag.nro' scripts/build_switch.sh scripts/build_in_devkitpro_docker.sh .github/workflows/build-nro.yml
! grep -R -q 'consoleInit\|consoleClear\|consoleUpdate' source/netdiag
# v1.2.5/v1.2.6 install, download and log-policy guardrails.
grep -q 'kMaxNczSections = 0xFFFFULL' source/atmo_xl/source/nx/nca_writer.cpp
! grep -q 'kMaxNczSections = 64' source/atmo_xl/source/nx/nca_writer.cpp
grep -q 'cryptoType != 3 && cryptoType != 4' source/atmo_xl/source/nx/nca_writer.cpp
grep -q 'GetPreparedStorageInfo' source/atmo_xl/include/install/install.hpp source/atmo_xl/source/install/install.cpp source/title_backend/backend_provider.cpp
grep -q 'move_local_entry' source/local_fs.hpp source/local_fs.cpp source/app/runtime_files.cpp
grep -q 'kDownloadReadAheadSlots = 16u' source/title_backend/buffered_stream.hpp
grep -q 'active_read_ns' source/title_backend/buffered_stream.hpp source/title_backend/buffered_stream.cpp
grep -q 'maybe_save_install_log' source/app/runtime.cpp source/app/runtime_internal.hpp
! grep -q 'maybe_upload_install_log' source/app/runtime.cpp source/app/runtime_internal.hpp
grep -q 'manual_upload_results_log' "${netdiag_sources[@]}" source/netdiag/ui.hpp
grep -q '测速日志已保存本地；需在网络配置中手动上传' "${netdiag_sources[@]}"
! grep -q '测速日志已自动上传' "${netdiag_sources[@]}"
! grep -q '安装日志已自动上传' source/app/runtime.cpp source/app/runtime_library.cpp
grep -q '上传最近安装日志' source/app/runtime_library.cpp
grep -q '上传最近测速日志' "${netdiag_sources[@]}"
grep -q 'workflow_dispatch:' .github/workflows/build-nro.yml .github/workflows/build-nro-self-hosted.yml
grep -Eq '^[[:space:]]+push:' .github/workflows/build-nro.yml
grep -q "github.event.before == 'd0cd5b428d94eb3db6b626e7a81161902fe1fcd6'" .github/workflows/build-nro.yml
! grep -q '2aaad5171cd154e7b89bb16b7b4d4f9cea340bb5' .github/workflows/build-nro.yml
! grep -q '7d6b77d4ee8844643242ae65fe415563898878fb' .github/workflows/build-nro.yml
! grep -q 'd3e2c76b97d86c6578386fccddf212dcfb88ff05' .github/workflows/build-nro.yml
! grep -q '44c7d5cf4f3ad4b804517539c2f5c6bef0bc6a5d' .github/workflows/build-nro.yml
! grep -q 'e537cc9437f436bf81ec384fd825dd82d8704094' .github/workflows/build-nro.yml
! grep -Eq '^[[:space:]]+pull_request:' .github/workflows/build-nro.yml
! grep -Eq '^[[:space:]]+(push|pull_request):' .github/workflows/build-nro-self-hosted.yml
grep -q 'InstallAndReadCnmtWithRepair' source/atmo_xl/include/install/install.hpp source/atmo_xl/source/install/install_nsp.cpp source/atmo_xl/source/install/install_xci.cpp
grep -q 'ReinstallNcaTracked' source/atmo_xl/include/install/install.hpp source/atmo_xl/source/install/install_parts/install_part_03.inc
grep -q 'verifyNcaContentHashes = true' source/atmo_xl/source/install/install_parts/install_part_03.inc
grep -q 'if (existed && !m_forceReinstall)' source/atmo_xl/source/install/install_parts/install_part_03.inc
grep -q 'a healthy registered CNMT must be reusable without touching' source/atmo_xl/source/install/install_parts/install_part_03.inc
grep -q 'Compressed CNMT entries may intentionally enter with size=0' source/atmo_xl/source/install/install_parts/install_part_03.inc
grep -q 'ReadValidatedNcaHeader' source/atmo_xl/include/install/nca_header_probe.hpp source/atmo_xl/source/install/install_nsp.cpp source/atmo_xl/source/install/install_xci.cpp
grep -q 'reread=%s' source/atmo_xl/include/install/nca_header_probe.hpp
grep -q 'raw_magic=0x%08x' source/atmo_xl/include/install/nca_header_probe.hpp
grep -q 'decrypted_magic=0x%08x' source/atmo_xl/include/install/nca_header_probe.hpp
grep -q 'header_mode=%s' source/atmo_xl/include/install/nca_header_probe.hpp
grep -q 'key_self_test=%s' source/atmo_xl/include/install/nca_header_probe.hpp
grep -q 'NcaHeaderKeySelfTest' source/atmo_xl/include/install/nca_header_probe.hpp
grep -q 'NcmStorageId_BuiltInSystem' source/atmo_xl/include/install/nca_header_probe.hpp
grep -q 'm_prefixSize = std::min<u64>(NCA_HEADER_SIZE, m_ncaSize)' source/atmo_xl/source/nx/nca_writer.cpp
grep -q 'legal small NCAs (for example 0xE00 CNMTs)' source/atmo_xl/source/nx/nca_writer.cpp
grep -q 'm_headerPlaintext' source/atmo_xl/include/nx/nca_writer.h source/atmo_xl/source/nx/nca_writer.cpp
grep -q 'Failed to derive NCA header KEK' source/atmo_xl/source/util/crypto.cpp
grep -q 'AuditFileEntry' source/atmo_xl/include/install/nsp.hpp source/atmo_xl/include/install/xci.hpp source/atmo_xl/source/install/nsp.cpp source/atmo_xl/source/install/xci.cpp
grep -q 'content_id_match=' source/atmo_xl/source/install/nsp.cpp source/atmo_xl/source/install/xci.cpp
grep -q 'entry_sha256=' source/atmo_xl/source/install/nsp.cpp source/atmo_xl/source/install/xci.cpp
grep -q 'table_bounds=' source/atmo_xl/source/install/nsp.cpp source/atmo_xl/source/install/xci.cpp
grep -q 'table_overlap=' source/atmo_xl/source/install/nsp.cpp source/atmo_xl/source/install/xci.cpp
grep -q 'not_applicable_compressed' source/atmo_xl/source/install/nsp.cpp source/atmo_xl/source/install/xci.cpp
grep -q 'source_audit=\[%s\]' source/atmo_xl/source/install/install_nsp.cpp source/atmo_xl/source/install/install_xci.cpp
grep -q 'Put the verdict first' source/atmo_xl/source/install/nsp.cpp source/atmo_xl/source/install/xci.cpp
grep -q 'throw_formatted_error' source/atmo_xl/include/util/error.hpp
! grep -q 'formatted_msg\[640\]' source/atmo_xl/include/util/error.hpp
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/error_format_test.cpp -Isource/atmo_xl/include -o /tmp/AstraNAS-error-format-test
/tmp/AstraNAS-error-format-test
grep -q 'provesContentIdMismatch' source/atmo_xl/include/install/source_entry_audit.hpp source/atmo_xl/source/install/install_nsp.cpp source/atmo_xl/source/install/install_xci.cpp
grep -q '安装包 CNMT 内容与 Content ID 不一致' source/atmo_xl/source/install/install_nsp.cpp source/atmo_xl/source/install/install_xci.cpp
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/source_entry_audit_test.cpp -Isource/atmo_xl/include -o /tmp/AstraNAS-source-entry-audit-test
/tmp/AstraNAS-source-entry-audit-test
grep -q 'add_source_read' source/atmo_xl/source/install/sdmc_nsp.cpp source/atmo_xl/source/install/sdmc_xci.cpp
grep -q 'AstraNAS-source.zip' .github/workflows/build-nro.yml .github/workflows/build-nro-self-hosted.yml
grep -q 'BUILD-MANIFEST.txt' .github/workflows/build-nro.yml .github/workflows/build-nro-self-hosted.yml
grep -q 'SHA256SUMS.txt' .github/workflows/build-nro.yml .github/workflows/build-nro-self-hosted.yml

"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/v05_config_test.cpp source/config.cpp -Isource -o /tmp/AstraNAS-v05-config-test
/tmp/AstraNAS-v05-config-test
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/remote/curl_client.cpp -Isource $(pkg-config --cflags libcurl) -o /tmp/AstraNAS-curl-client.o
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/remote/curl_stream.cpp -Isource $(pkg-config --cflags libcurl) -o /tmp/AstraNAS-curl-stream.o
rm -f /tmp/AstraNAS-webdav-put.bin /tmp/AstraNAS-webdav-port
python3 tests/webdav_put_server.py /tmp/AstraNAS-webdav-put.bin /tmp/AstraNAS-webdav-port &
WEBDAV_PID=$!
trap 'kill "$WEBDAV_PID" 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do [ -s /tmp/AstraNAS-webdav-port ] && break; sleep 0.02; done
[ -s /tmp/AstraNAS-webdav-port ]
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/remote_upload_test.cpp source/remote/curl_client.cpp source/remote/curl_stream.cpp source/remote/curl_delete.cpp -Isource $(pkg-config --cflags --libs libcurl) -o /tmp/AstraNAS-remote-upload-test
/tmp/AstraNAS-remote-upload-test "$(cat /tmp/AstraNAS-webdav-port)"
wait "$WEBDAV_PID"
trap - EXIT
cmp /tmp/AstraNAS-remote-upload-source.bin /tmp/AstraNAS-webdav-put.bin

rm -f /tmp/AstraNAS-webdav-range-port
python3 tests/webdav_range_server.py /tmp/AstraNAS-webdav-range-port &
RANGE_PID=$!
trap 'kill "$RANGE_PID" 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do [ -s /tmp/AstraNAS-webdav-range-port ] && break; sleep 0.02; done
[ -s /tmp/AstraNAS-webdav-range-port ]
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  tests/remote_direct_read_test.cpp source/remote/curl_client.cpp source/remote/curl_stream.cpp source/remote/curl_delete.cpp \
  source/remote/remote_package_source.cpp source/title_backend/package_source.cpp \
  -Isource $(pkg-config --cflags --libs libcurl) -o /tmp/AstraNAS-remote-direct-read-test
/tmp/AstraNAS-remote-direct-read-test "$(cat /tmp/AstraNAS-webdav-range-port)"
kill "$RANGE_PID" 2>/dev/null || true
wait "$RANGE_PID" 2>/dev/null || true
trap - EXIT
./scripts/run_install_edge_tests.sh
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/network_runtime.cpp -Isource -o /tmp/AstraNAS-network-runtime.o
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/network_runtime_test.cpp source/network_runtime.cpp -Isource -o /tmp/AstraNAS-network-runtime-test
/tmp/AstraNAS-network-runtime-test
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/network_diagnostics.cpp -Isource -o /tmp/AstraNAS-network-diagnostics.o
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/network_diagnostics_test.cpp source/network_diagnostics.cpp -Isource -o /tmp/AstraNAS-network-diagnostics-test
/tmp/AstraNAS-network-diagnostics-test
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/remote/factory.cpp -Isource -o /tmp/AstraNAS-factory.o
"$CXX" -std=c++17 -Wall -Wextra -Werror -c source/remote/smb_client.cpp -Isource -Ilibs/libsmb2/include -o /tmp/AstraNAS-smb-client.o
"$CXX" -std=c++17 -Wall -Wextra -Werror -c source/remote/smb_upload.cpp -Isource -Ilibs/libsmb2/include -o /tmp/AstraNAS-smb-upload.o
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/remote/remote_package_source.cpp -Isource -o /tmp/AstraNAS-remote-package-source.o
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/package_inspect.cpp -Isource -o /tmp/AstraNAS-package-inspect.o
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/installer.cpp -Isource -o /tmp/AstraNAS-installer.o
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/title_backend/install_session.cpp -Isource -o /tmp/AstraNAS-install-session.o
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/title_backend/backend_provider.cpp -Isource -Isource/atmo_xl/include -Isource/atmo_xl/include/util -o /tmp/AstraNAS-backend-provider.o
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -c source/main.cpp -Isource -o /tmp/AstraNAS-main.o
python3 -m py_compile scripts/check_ui_localization.py scripts/check_nro_bundle.py scripts/fill_nacp_languages.py scripts/generate_manifest.py scripts/package_release.py tests/nacp_language_test.py tests/hbmenu_layout_test.py tests/webdav_put_server.py tests/webdav_range_server.py
echo "host regression checks: PASS"

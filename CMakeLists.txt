cmake_minimum_required(VERSION 3.16)
project(astranas VERSION 1.0.3 LANGUAGES C CXX)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR
        "AstraNAS is a Nintendo Switch target. Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/Switch.cmake")
endif()

add_executable(AstraNAS
    source/main.cpp
    source/app/runtime.cpp
    source/app/runtime_services.cpp
    source/app/runtime_files.cpp
    source/app/runtime_library.cpp
    source/app/action_selection.cpp
    source/app/action_transfer.cpp
    source/app/action_install.cpp
    source/app/util.cpp
    source/ui/gui.cpp
    source/ui/input.cpp
    source/ui/pages.cpp
    source/ui/dialogs.cpp
    source/installed/catalog.cpp
    source/config.cpp
    source/network_runtime.cpp
    source/network_diagnostics.cpp
    source/log_upload.cpp
    source/download_cache.cpp
    source/completion_sound.cpp
    source/install_cleanup.cpp
    source/install_performance.cpp
    source/local_fs.cpp
    source/installer.cpp
    source/user_title_backend.cpp
    source/title_backend/package_source.cpp
    source/title_backend/buffered_stream.cpp
    source/title_backend/backend_provider.cpp
    source/title_backend/install_session.cpp
    source/title_backend/package_archive.cpp
    source/atmo_xl/source/data/byte_buffer.cpp
    source/atmo_xl/source/install/install.cpp
    source/atmo_xl/source/install/ticket_compat.cpp
    source/atmo_xl/source/install/install_nsp.cpp
    source/atmo_xl/source/install/install_xci.cpp
    source/atmo_xl/source/install/nsp.cpp
    source/atmo_xl/source/install/xci.cpp
    source/atmo_xl/source/install/sdmc_nsp.cpp
    source/atmo_xl/source/install/sdmc_xci.cpp
    source/atmo_xl/source/install/simple_filesystem.cpp
    source/atmo_xl/source/nx/content_meta.cpp
    source/atmo_xl/source/nx/fs.cpp
    source/usb_storage.cpp
    source/atmo_xl/source/nx/nca_writer.cpp
    source/atmo_xl/source/nx/ncm.cpp
    source/atmo_xl/source/nx/ipc/es.c
    source/atmo_xl/source/nx/ipc/ns_ext.c
    source/atmo_xl/source/util/crypto.cpp
    source/atmo_xl/source/util/debug.c
    source/atmo_xl/source/util/file_util.cpp
    source/atmo_xl/source/util/title_util.cpp
    source/atmo_xl/source/bridge/atmo_xl_bridge.cpp
    source/package_inspect.cpp
    source/sha256.cpp
    source/remote/factory.cpp
    source/remote/remote_package_source.cpp
    source/remote/smb_client.cpp
    source/remote/smb_benchmark_tuning.cpp
    source/remote/smb_upload.cpp
    source/remote/smb_delete.cpp
    source/remote/curl_client.cpp
    source/remote/curl_stream.cpp
    source/remote/curl_delete.cpp
)

target_compile_features(AstraNAS PRIVATE cxx_std_17)
target_compile_definitions(AstraNAS PRIVATE __SWITCH__)
target_compile_options(AstraNAS PRIVATE -Wall -Wextra -Wno-unused-parameter)

set(ASTRANAS_SWITCH_PORTLIBS "$ENV{DEVKITPRO}/portlibs/switch")
if(NOT EXISTS "${ASTRANAS_SWITCH_PORTLIBS}/include")
    message(FATAL_ERROR "Switch portlibs include directory not found: ${ASTRANAS_SWITCH_PORTLIBS}/include")
endif()

target_include_directories(AstraNAS PRIVATE
    source
    source/atmo_xl/include
    source/atmo_xl/include/util
    "${ASTRANAS_SWITCH_PORTLIBS}/include"
    "${ASTRANAS_SWITCH_PORTLIBS}/include/freetype2"
)
target_link_directories(AstraNAS PRIVATE
    "${ASTRANAS_SWITCH_PORTLIBS}/lib"
)

target_link_libraries(AstraNAS PRIVATE
    smb2
    curl
    freetype
    harfbuzz
    jpeg
    png
    bz2
    z
    zstd
    mbedtls
    mbedcrypto
    mbedx509
    usbhsfs
    ntfs-3g
    lwext4
)

nx_generate_nacp(astranas.nacp
    NAME "Switch游戏安装"
    AUTHOR "AstraNAS Project"
    VERSION "${PROJECT_VERSION}"
)

nx_create_nro(AstraNAS
    ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg"
    NACP astranas.nacp
    ROMFS "${CMAKE_CURRENT_SOURCE_DIR}/romfs"
)

add_executable(AstraNAS-NetDiag
    source/netdiag/main.cpp
    source/netdiag/ui.cpp
    source/ui/gui.cpp
    source/ui/input.cpp
    source/ui/dialogs.cpp
    source/config.cpp
    source/network_runtime.cpp
    source/network_diagnostics.cpp
    source/log_upload.cpp
    source/remote/factory.cpp
    source/remote/smb_client.cpp
    source/remote/smb_benchmark_tuning.cpp
    source/remote/smb_upload.cpp
    source/remote/smb_delete.cpp
    source/remote/curl_client.cpp
    source/remote/curl_stream.cpp
    source/remote/curl_delete.cpp
)

target_compile_features(AstraNAS-NetDiag PRIVATE cxx_std_17)
target_compile_definitions(AstraNAS-NetDiag PRIVATE __SWITCH__)
target_compile_options(AstraNAS-NetDiag PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(AstraNAS-NetDiag PRIVATE
    source
    "${ASTRANAS_SWITCH_PORTLIBS}/include"
    "${ASTRANAS_SWITCH_PORTLIBS}/include/freetype2"
)
target_link_directories(AstraNAS-NetDiag PRIVATE
    "${ASTRANAS_SWITCH_PORTLIBS}/lib"
)
target_link_libraries(AstraNAS-NetDiag PRIVATE
    smb2
    curl
    freetype
    harfbuzz
    jpeg
    png
    bz2
    z
    mbedtls
    mbedcrypto
    mbedx509
)

nx_generate_nacp(astranas_netdiag.nacp
    NAME "Switch网络测速"
    AUTHOR "AstraNAS Project"
    VERSION "${PROJECT_VERSION}"
)

nx_create_nro(AstraNAS-NetDiag
    ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/netdiag_icon.jpg"
    NACP astranas_netdiag.nacp
)

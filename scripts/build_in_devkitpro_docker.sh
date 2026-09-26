#!/usr/bin/env bash
set -euo pipefail
IMAGE="devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282"
LIBSMB2_COMMIT="0b2aa4310c7a0510c666fe7da0eebce0f7e93824"
LIBUSBHSFS_COMMIT="3b897ed6a79c910aa3f13693f17b80afb802b3b5"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

docker run --rm \
  -e LIBSMB2_COMMIT="$LIBSMB2_COMMIT" \
  -e LIBUSBHSFS_COMMIT="$LIBUSBHSFS_COMMIT" \
  -v "$ROOT:/work" \
  -w /work \
  "$IMAGE" bash -lc '
    set -euo pipefail
    ASTRANAS_VERSION="$(sed -n "s/^project(astranas VERSION \([^ ]*\) LANGUAGES.*/\1/p" CMakeLists.txt)"
    test -n "$ASTRANAS_VERSION"
    libsmb2_tmp="$(mktemp -d /tmp/astranas-libsmb2.XXXXXX)"
    git init -q "$libsmb2_tmp"
    git -C "$libsmb2_tmp" remote add origin https://github.com/sahlberg/libsmb2.git
    git -C "$libsmb2_tmp" fetch --depth 1 origin "$LIBSMB2_COMMIT"
    git -C "$libsmb2_tmp" checkout --detach -q FETCH_HEAD
    test "$(git -C "$libsmb2_tmp" rev-parse HEAD)" = "$LIBSMB2_COMMIT"
    make -C "$libsmb2_tmp" -f Makefile.platform switch_install
    if command -v dkp-pacman >/dev/null 2>&1; then
      dkp-pacman -S --needed --noconfirm switch-ntfs-3g switch-lwext4
    else
      pacman -S --needed --noconfirm switch-ntfs-3g switch-lwext4
    fi
    usbhsfs_tmp="$(mktemp -d /tmp/astranas-usbhsfs.XXXXXX)"
    git init -q "$usbhsfs_tmp"
    git -C "$usbhsfs_tmp" remote add origin https://github.com/DarkMatterCore/libusbhsfs.git
    git -C "$usbhsfs_tmp" fetch --depth 1 origin "$LIBUSBHSFS_COMMIT"
    git -C "$usbhsfs_tmp" checkout --detach -q FETCH_HEAD
    test "$(git -C "$usbhsfs_tmp" rev-parse HEAD)" = "$LIBUSBHSFS_COMMIT"
    make -C "$usbhsfs_tmp" BUILD_TYPE=GPL install
    ./scripts/build_switch.sh
    test -s dist/switch/AstraNAS/AstraNAS.nro
    test -s dist/switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro
    head -c 64 dist/switch/AstraNAS/AstraNAS.nro | grep -a -q NRO0
    head -c 64 dist/switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro | grep -a -q NRO0
    python3 scripts/check_nro_bundle.py dist "$ASTRANAS_VERSION"
    sha256sum dist/switch/AstraNAS/AstraNAS.nro dist/switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro > dist/NRO-SHA256SUMS.txt
  '

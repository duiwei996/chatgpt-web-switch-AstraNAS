#!/usr/bin/env bash
set -euo pipefail

TOOLCHAIN_REPO="${ASTRANAS_TOOLCHAIN_REPO:-duiwei996/chatgpt-web-github-actions-DownloadBridge}"
TOOLCHAIN_TAG="${ASTRANAS_TOOLCHAIN_TAG:-astranas-devkitpro-v1}"
TOOLCHAIN_SHA256="${ASTRANAS_TOOLCHAIN_SHA256:-af4ec72ba7c20b7aedc79f5d63bffa842e2665e740a878cc78c13d34051791b9}"
CACHE_DIR="${ASTRANAS_TOOLCHAIN_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/astranas}"
ARCHIVE="${ASTRANAS_TOOLCHAIN_ARCHIVE:-$CACHE_DIR/devkitpro-switch-astranas-v1.tar.gz}"
PACKAGE_LIST="${ASTRANAS_TOOLCHAIN_PACKAGE_LIST:-$CACHE_DIR/devkitpro-packages-v1.txt}"
RELEASE_BASE="https://github.com/${TOOLCHAIN_REPO}/releases/download/${TOOLCHAIN_TAG}"

mkdir -p "$CACHE_DIR"

download_file() {
  local url="$1" dst="$2"
  local tmp="${dst}.tmp.$$"
  rm -f "$tmp"
  if ! curl --fail --location --retry 3 --retry-delay 2 --connect-timeout 20 \
    --output "$tmp" "$url"; then
    rm -f "$tmp"
    return 1
  fi
  mv "$tmp" "$dst"
}

valid_toolchain() {
  test -x /opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++ && \
  test -f /opt/devkitpro/cmake/Switch.cmake && \
  test -x /opt/devkitpro/tools/bin/elf2nro && \
  test -x /opt/devkitpro/tools/bin/nacptool && \
  test -f /opt/devkitpro/libnx/include/switch.h && \
  test -f /opt/devkitpro/portlibs/switch/include/curl/curl.h && \
  test -f /opt/devkitpro/portlibs/switch/include/smb2/smb2.h && \
  test -f /opt/devkitpro/portlibs/switch/lib/libsmb2.a && \
  test -f /opt/devkitpro/portlibs/switch/lib/libusbhsfs.a
}

if ! valid_toolchain || [[ "${ASTRANAS_TOOLCHAIN_FORCE_BUNDLE:-0}" == "1" ]]; then
  if [[ ! -s "$ARCHIVE" ]]; then
    echo "DownloadBridge: downloading ${TOOLCHAIN_TAG} toolchain archive..."
    download_file "$RELEASE_BASE/devkitpro-switch-astranas.tar.gz" "$ARCHIVE"
  else
    echo "DownloadBridge: reusing cached archive $ARCHIVE"
  fi

  printf '%s  %s\n' "$TOOLCHAIN_SHA256" "$ARCHIVE" | sha256sum -c -

  echo "DownloadBridge: extracting toolchain to /opt/devkitpro..."
  if [[ -w /opt ]]; then
    rm -rf /opt/devkitpro
    tar -xzf "$ARCHIVE" -C /opt
  elif command -v sudo >/dev/null 2>&1; then
    sudo rm -rf /opt/devkitpro
    sudo tar -xzf "$ARCHIVE" -C /opt
  else
    echo "Need write access to /opt or sudo to install the cached devkitPro bundle." >&2
    exit 2
  fi
fi

if ! valid_toolchain; then
  echo "Downloaded devkitPro bundle is incomplete." >&2
  exit 3
fi

if [[ "${ASTRANAS_TOOLCHAIN_FETCH_METADATA:-0}" == "1" ]]; then
  if [[ ! -s "$PACKAGE_LIST" ]]; then
    download_file "$RELEASE_BASE/devkitpro-packages.txt" "$PACKAGE_LIST" || true
  fi
  if [[ -s "$PACKAGE_LIST" ]]; then
    cp "$PACKAGE_LIST" /tmp/AstraNAS-dependencies.txt
  fi
fi

export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"

/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++ --version | head -n 1
echo "DownloadBridge toolchain ready: $TOOLCHAIN_TAG"

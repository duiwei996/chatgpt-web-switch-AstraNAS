#!/usr/bin/env bash
set -euo pipefail
: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO
if [[ ! -f "$DEVKITPRO/cmake/Switch.cmake" ]]; then
  echo "Missing $DEVKITPRO/cmake/Switch.cmake" >&2
  echo "Install devkitPro switch-dev + switch-cmake first." >&2
  exit 2
fi

# AstraNAS v1.2.7 intentionally reuses AtmoXL's original fertig.wav. Fetch from
# the pinned upstream commit and verify the Git blob identity before packaging.
bash scripts/prepare_atmoxl_audio.sh romfs/audio/fertig.wav

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
if [[ -f build/AstraNAS.nro && -f build/AstraNAS-NetDiag.nro ]]; then
  python3 scripts/fill_nacp_languages.py --nro build/AstraNAS.nro
  python3 scripts/fill_nacp_languages.py --nro build/AstraNAS-NetDiag.nro
  rm -rf dist/switch
  mkdir -p dist/switch/AstraNAS dist/switch/AstraNAS-NetDiag
  cp build/AstraNAS.nro dist/switch/AstraNAS/AstraNAS.nro
  cp build/AstraNAS-NetDiag.nro dist/switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro
  cp config.example.ini dist/switch/AstraNAS/config.ini
  echo "Built: dist/switch/AstraNAS/AstraNAS.nro"
  echo "Built: dist/switch/AstraNAS-NetDiag/AstraNAS-NetDiag.nro"
else
  echo "Build finished but one or both NRO outputs were not found." >&2
  ls -l build/*.nro 2>/dev/null || true
  exit 3
fi

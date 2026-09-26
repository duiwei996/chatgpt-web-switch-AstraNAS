#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
./scripts/prepare_devkitpro_from_bridge.sh
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
exec ./scripts/build_switch.sh

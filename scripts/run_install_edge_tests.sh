#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
CXX="${CXX:-g++}"

"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread \
  tests/install_edge_test.cpp source/install_cleanup.cpp source/local_fs.cpp \
  source/title_backend/package_source.cpp source/title_backend/buffered_stream.cpp \
  source/install_performance.cpp -Isource \
  -o tests/install_edge_test
./tests/install_edge_test
rm -f tests/install_edge_test
echo "install edge tests: PASS"

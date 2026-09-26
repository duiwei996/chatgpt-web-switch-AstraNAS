// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "remote/remote_client.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

namespace astranas::network {

struct DiagnosticResult {
    RemoteDirEntry candidate;
    RemoteBenchmarkStats stats;
};

bool run_remote_diagnostic(RemoteClient& remote,
                           const std::string& start_dir,
                           std::size_t benchmark_bytes,
                           DiagnosticResult& result,
                           std::string& error,
                           std::size_t max_scan_dirs = 12);

std::string format_diagnostic_summary(const std::string& protocol,
                                      const DiagnosticResult& result,
                                      const std::string& socket_profile = {});
std::string diagnostic_hint(const std::string& protocol, const DiagnosticResult& result);

} // namespace astranas::network

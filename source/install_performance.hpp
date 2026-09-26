// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace astranas::install_performance {

struct Snapshot {
    std::uint64_t source_bytes = 0;
    std::uint64_t source_read_ns = 0;
    std::uint64_t source_wait_ns = 0;
    std::uint64_t source_backpressure_ns = 0;
    std::uint64_t ncm_bytes = 0;
    std::uint64_t ncm_write_ns = 0;
    std::uint64_t zstd_output_bytes = 0;
    std::uint64_t zstd_ns = 0;
    std::uint64_t aes_bytes = 0;
    std::uint64_t aes_ns = 0;
};

void reset();
void add_source_read(std::uint64_t bytes, std::uint64_t ns);
void add_source_wait(std::uint64_t ns);
void add_source_backpressure(std::uint64_t ns);
void add_ncm_write(std::uint64_t bytes, std::uint64_t ns);
void add_zstd(std::uint64_t output_bytes, std::uint64_t ns);
void add_aes(std::uint64_t bytes, std::uint64_t ns);
Snapshot snapshot();
std::string summary(const Snapshot& stats, double total_seconds);
std::string compact_summary(const Snapshot& stats);

} // namespace astranas::install_performance

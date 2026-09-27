// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "package_source.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace astranas::title_backend {

inline constexpr std::size_t kInstallStreamChunkSize = 4u * 1024u * 1024u;
inline constexpr std::size_t kInstallReadAheadSlots = 2u; // 8 MiB install ring.
inline constexpr std::size_t kDownloadReadAheadSlots = 2u; // 8 MiB download ring.
inline constexpr std::size_t kInstallWriteSliceSize = 1u * 1024u * 1024u;
inline constexpr std::size_t kDownloadWriteSliceSize = 4u * 1024u * 1024u;

using BufferedStreamConsumer = std::function<void(const unsigned char* data,
                                                  std::size_t size,
                                                  std::uint64_t logical_offset)>;
using BufferedStreamHeartbeat = std::function<bool(std::uint64_t consumed,
                                                   std::uint64_t total)>;

struct BufferedStreamTelemetry {
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> active_read_ns{0};
    std::atomic<std::uint64_t> backpressure_ns{0};
    std::uint64_t capacity_bytes = 0;
};

bool stream_buffered_read_ahead(PackageSource& source,
                                std::uint64_t source_offset,
                                std::uint64_t total_size,
                                const BufferedStreamConsumer& consumer,
                                std::string& error,
                                const BufferedStreamHeartbeat& heartbeat = {},
                                std::size_t chunk_size = kInstallStreamChunkSize,
                                std::size_t slot_count = kInstallReadAheadSlots,
                                bool record_install_metrics = true,
                                BufferedStreamTelemetry* telemetry = nullptr);

} // namespace astranas::title_backend

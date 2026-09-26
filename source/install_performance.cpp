// SPDX-License-Identifier: GPL-3.0-or-later
#include "install_performance.hpp"

#include <atomic>
#include <iomanip>
#include <sstream>

namespace astranas::install_performance {
namespace {
std::atomic<std::uint64_t> g_source_bytes{0};
std::atomic<std::uint64_t> g_source_read_ns{0};
std::atomic<std::uint64_t> g_source_wait_ns{0};
std::atomic<std::uint64_t> g_source_backpressure_ns{0};
std::atomic<std::uint64_t> g_ncm_bytes{0};
std::atomic<std::uint64_t> g_ncm_write_ns{0};
std::atomic<std::uint64_t> g_zstd_output_bytes{0};
std::atomic<std::uint64_t> g_zstd_ns{0};
std::atomic<std::uint64_t> g_aes_bytes{0};
std::atomic<std::uint64_t> g_aes_ns{0};

double mib_per_second(std::uint64_t bytes, std::uint64_t ns) {
    if (bytes == 0 || ns == 0) return 0.0;
    const double seconds = static_cast<double>(ns) / 1'000'000'000.0;
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds;
}
}

void reset() {
    g_source_bytes.store(0);
    g_source_read_ns.store(0);
    g_source_wait_ns.store(0);
    g_source_backpressure_ns.store(0);
    g_ncm_bytes.store(0);
    g_ncm_write_ns.store(0);
    g_zstd_output_bytes.store(0);
    g_zstd_ns.store(0);
    g_aes_bytes.store(0);
    g_aes_ns.store(0);
}

void add_source_read(std::uint64_t bytes, std::uint64_t ns) {
    g_source_bytes.fetch_add(bytes);
    g_source_read_ns.fetch_add(ns);
}
void add_source_wait(std::uint64_t ns) { g_source_wait_ns.fetch_add(ns); }
void add_source_backpressure(std::uint64_t ns) { g_source_backpressure_ns.fetch_add(ns); }
void add_ncm_write(std::uint64_t bytes, std::uint64_t ns) {
    g_ncm_bytes.fetch_add(bytes);
    g_ncm_write_ns.fetch_add(ns);
}
void add_zstd(std::uint64_t output_bytes, std::uint64_t ns) {
    g_zstd_output_bytes.fetch_add(output_bytes);
    g_zstd_ns.fetch_add(ns);
}
void add_aes(std::uint64_t bytes, std::uint64_t ns) {
    g_aes_bytes.fetch_add(bytes);
    g_aes_ns.fetch_add(ns);
}

Snapshot snapshot() {
    Snapshot out;
    out.source_bytes = g_source_bytes.load();
    out.source_read_ns = g_source_read_ns.load();
    out.source_wait_ns = g_source_wait_ns.load();
    out.source_backpressure_ns = g_source_backpressure_ns.load();
    out.ncm_bytes = g_ncm_bytes.load();
    out.ncm_write_ns = g_ncm_write_ns.load();
    out.zstd_output_bytes = g_zstd_output_bytes.load();
    out.zstd_ns = g_zstd_ns.load();
    out.aes_bytes = g_aes_bytes.load();
    out.aes_ns = g_aes_ns.load();
    return out;
}

std::string summary(const Snapshot& stats, double total_seconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << "源读取=" << mib_per_second(stats.source_bytes, stats.source_read_ns) << " MiB/s"
        << " 源读取耗时_ms=" << (static_cast<double>(stats.source_read_ns) / 1'000'000.0)
        << " 等待源数据_ms=" << (static_cast<double>(stats.source_wait_ns) / 1'000'000.0)
        << " 源回压_ms=" << (static_cast<double>(stats.source_backpressure_ns) / 1'000'000.0)
        << " NCM写入=" << mib_per_second(stats.ncm_bytes, stats.ncm_write_ns) << " MiB/s"
        << " NCM耗时_ms=" << (static_cast<double>(stats.ncm_write_ns) / 1'000'000.0);
    if (stats.zstd_ns) {
        out << " Zstd解压=" << mib_per_second(stats.zstd_output_bytes, stats.zstd_ns) << " MiB/s"
            << " Zstd耗时_ms=" << (static_cast<double>(stats.zstd_ns) / 1'000'000.0);
    }
    if (stats.aes_ns) {
        out << " AES处理=" << mib_per_second(stats.aes_bytes, stats.aes_ns) << " MiB/s"
            << " AES耗时_ms=" << (static_cast<double>(stats.aes_ns) / 1'000'000.0);
    }
    if (total_seconds > 0.0) out << " 总耗时_s=" << total_seconds;
    return out.str();
}

std::string compact_summary(const Snapshot& stats) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    const double source = mib_per_second(stats.source_bytes, stats.source_read_ns);
    const double ncm = mib_per_second(stats.ncm_bytes, stats.ncm_write_ns);
    if (source > 0.0) out << "源 " << source << " MiB/s";
    if (ncm > 0.0) {
        if (source > 0.0) out << " · ";
        out << "NCM " << ncm << " MiB/s";
    }
    if (stats.source_wait_ns > 0) {
        if (source > 0.0 || ncm > 0.0) out << " · ";
        out << "等网 " << (stats.source_wait_ns / 1'000'000ull) << " ms";
    }
    if (stats.source_backpressure_ns > 0) {
        if (source > 0.0 || ncm > 0.0 || stats.source_wait_ns > 0) out << " · ";
        out << "回压 " << (stats.source_backpressure_ns / 1'000'000ull) << " ms";
    }
    return out.str();
}

} // namespace astranas::install_performance

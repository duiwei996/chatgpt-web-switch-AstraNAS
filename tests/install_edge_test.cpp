// SPDX-License-Identifier: GPL-3.0-or-later
#include "install_cleanup.hpp"
#include "install_performance.hpp"
#include "title_backend/buffered_stream.hpp"
#include "title_backend/package_source.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
class FailingPackageSource final : public astranas::title_backend::PackageSource {
public:
    bool open(const std::string&, std::string&) override { position_ = 0; return true; }
    bool read(void* buffer, std::size_t size, std::size_t& actual, std::string& error) override {
        actual = 0;
        if (position_ >= astranas::title_backend::kInstallStreamChunkSize) {
            error = "synthetic source failure";
            return false;
        }
        const std::size_t available = astranas::title_backend::kInstallStreamChunkSize -
                                      static_cast<std::size_t>(position_);
        const std::size_t count = std::min(size, available);
        std::memset(buffer, 0x5a, count);
        position_ += count;
        actual = count;
        return true;
    }
    bool seek(std::uint64_t offset, std::string& error) override {
        if (offset > size()) { error = "synthetic seek out of range"; return false; }
        position_ = offset;
        return true;
    }
    std::uint64_t size() const override {
        return static_cast<std::uint64_t>(astranas::title_backend::kInstallStreamChunkSize) * 2u;
    }
private:
    std::uint64_t position_ = 0;
};

class SlowPackageSource final : public astranas::title_backend::PackageSource {
public:
    bool open(const std::string&, std::string&) override { position_ = 0; return true; }
    bool read(void* buffer, std::size_t size, std::size_t& actual, std::string&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const std::size_t remaining = static_cast<std::size_t>(this->size() - position_);
        actual = std::min(size, remaining);
        std::memset(buffer, 0x7b, actual);
        position_ += actual;
        return true;
    }
    bool seek(std::uint64_t offset, std::string& error) override {
        if (offset > size()) { error = "slow synthetic seek out of range"; return false; }
        position_ = offset;
        return true;
    }
    std::uint64_t size() const override {
        return astranas::title_backend::kInstallStreamChunkSize;
    }
private:
    std::uint64_t position_ = 0;
};
}

int main() {
    namespace fs = std::filesystem;
    const std::string root = "/tmp/AstraNAS-install-edge-test";
    fs::remove_all(root);
    fs::create_directories(root);
    std::string error;
    std::vector<std::string> parts;

    const std::string contiguous = root + "/contiguous.nsp.";
    { std::ofstream out(contiguous + "00"); out << "zero"; }
    { std::ofstream out(contiguous + "01"); out << "one"; }
    assert(astranas::title_backend::resolve_package_parts(contiguous + "00", parts, error));
    assert(parts.size() == 2);

    const std::string gapped = root + "/gapped.nsp.";
    { std::ofstream out(gapped + "00"); out << "zero"; }
    { std::ofstream out(gapped + "02"); out << "two"; }
    assert(!astranas::title_backend::resolve_package_parts(gapped + "00", parts, error));
    assert(error.find("missing") != std::string::npos);

    const std::string ordinary = root + "/ordinary.nsp";
    fs::create_directories(ordinary);
    { std::ofstream out(ordinary + "/notes.txt"); out << "not a split package"; }
    assert(!astranas::title_backend::resolve_package_parts(ordinary, parts, error));

    const std::string mixed = root + "/mixed.xci";
    fs::create_directories(mixed);
    { std::ofstream out(mixed + "/00"); out << "zero"; }
    { std::ofstream out(mixed + "/01"); out << "one"; }
    { std::ofstream out(mixed + "/keep.txt"); out << "keep"; }
    assert(remove_installed_package_source(mixed, error));
    assert(fs::exists(mixed));
    assert(!fs::exists(mixed + "/00"));
    assert(!fs::exists(mixed + "/01"));
    assert(fs::exists(mixed + "/keep.txt"));

    const std::string buffered = root + "/buffered.nsp";
    const std::size_t test_chunk = 64u * 1024u;
    const std::size_t total = test_chunk *
        (astranas::title_backend::kInstallReadAheadSlots + 1u) + 12345u;
    std::vector<unsigned char> payload(total);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<unsigned char>((i * 17u + 3u) & 0xffu);
    {
        std::ofstream out(buffered, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        assert(out.good());
    }

    astranas::title_backend::FilePackageSource source;
    assert(source.open(buffered, error));
    astranas::install_performance::reset();
    std::size_t callbacks = 0;
    std::uint64_t consumed = 0;
    assert(astranas::title_backend::stream_buffered_read_ahead(
        source, 0, total,
        [&](const unsigned char* data, std::size_t size, std::uint64_t logical_offset) {
            assert(logical_offset == consumed);
            const std::size_t expected_size = callbacks < astranas::title_backend::kInstallReadAheadSlots + 1u
                ? test_chunk : 12345u;
            assert(size == expected_size);
            for (std::size_t i = 0; i < size; ++i)
                assert(data[i] == payload[static_cast<std::size_t>(logical_offset) + i]);
            consumed += size;
            ++callbacks;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }, error, {}, test_chunk));
    assert(callbacks == astranas::title_backend::kInstallReadAheadSlots + 2u);
    assert(consumed == total);
    const auto stats = astranas::install_performance::snapshot();
    assert(stats.source_bytes == total);
    assert(stats.source_backpressure_ns > 0);
    assert(astranas::title_backend::kInstallReadAheadSlots == 2u);
    assert(astranas::title_backend::kInstallWriteSliceSize <
           astranas::title_backend::kInstallStreamChunkSize);
    assert(astranas::title_backend::kInstallStreamChunkSize %
           astranas::title_backend::kInstallWriteSliceSize == 0);

    SlowPackageSource slow_source;
    assert(slow_source.open("synthetic", error));
    error.clear();
    std::size_t heartbeat_calls = 0;
    std::size_t cancelled_consumer_calls = 0;
    const bool cancelled_stream = astranas::title_backend::stream_buffered_read_ahead(
        slow_source, 0, slow_source.size(),
        [&](const unsigned char*, std::size_t, std::uint64_t) {
            ++cancelled_consumer_calls;
        }, error,
        [&](std::uint64_t consumed_bytes, std::uint64_t total_bytes) {
            assert(consumed_bytes == 0);
            assert(total_bytes == slow_source.size());
            ++heartbeat_calls;
            return false;
        });
    assert(!cancelled_stream);
    assert(error.find("cancel") != std::string::npos);
    assert(heartbeat_calls == 1);
    assert(cancelled_consumer_calls == 0);

    FailingPackageSource failing_source;
    error.clear();
    std::uint64_t failure_consumed = 0;
    const bool failed_stream = astranas::title_backend::stream_buffered_read_ahead(
        failing_source, 0, failing_source.size(),
        [&](const unsigned char*, std::size_t size, std::uint64_t logical_offset) {
            assert(logical_offset == failure_consumed);
            failure_consumed += size;
        }, error);
    assert(!failed_stream);
    assert(error.find("synthetic source failure") != std::string::npos);

    bool consumer_exception_seen = false;
    try {
        error.clear();
        (void)astranas::title_backend::stream_buffered_read_ahead(
            source, 0, total,
            [&](const unsigned char*, std::size_t, std::uint64_t) {
                throw std::runtime_error("synthetic consumer failure");
            }, error);
    } catch (const std::runtime_error& ex) {
        consumer_exception_seen = std::string(ex.what()) == "synthetic consumer failure";
    }
    assert(consumer_exception_seen);

    fs::remove_all(root);
    return 0;
}

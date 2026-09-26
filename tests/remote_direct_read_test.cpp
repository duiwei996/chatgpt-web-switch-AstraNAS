// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/curl_client.hpp"
#include "remote/remote_package_source.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr std::uint64_t kSize = 2ull * 1024ull * 1024ull + 123ull;
unsigned char expected_byte(std::uint64_t index) {
    return static_cast<unsigned char>((index * 31ull + 7ull) & 0xffull);
}

bool verify_range(const std::vector<unsigned char>& data, std::uint64_t offset) {
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (data[i] != expected_byte(offset + i)) return false;
    }
    return true;
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: remote_direct_read_test <port>\n";
        return 2;
    }
    const int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) return 3;

    AppConfig config;
    config.protocol = "webdav";
    config.server = "127.0.0.1";
    config.port = port;
    config.webdav_tls = false;
    config.url = "webdav://127.0.0.1:" + std::to_string(port);
    config.download_retries = 1;

    CurlRemoteClient client;
    std::string error;
    if (!client.connect(config, error)) {
        std::cerr << error << '\n';
        return 4;
    }

    RemoteDirEntry entry;
    entry.name = "package.nsp";
    entry.path = "/package.nsp";
    entry.size = kSize;
    entry.identity = "etag:\"astranas-range-v1\"";

    astranas::remote::RemotePackageSource source(client, entry);
    if (!source.open(entry.path, error)) {
        std::cerr << error << '\n';
        return 5;
    }

    constexpr std::uint64_t kOffset = 12345;
    std::vector<unsigned char> first(65537);
    std::size_t actual = 0;
    if (!source.read_at(kOffset, first.data(), first.size(), actual, error) ||
        actual != first.size() || !verify_range(first, kOffset)) {
        std::cerr << "direct range payload mismatch: " << error << '\n';
        return 6;
    }

    const std::uint64_t tail_offset = kSize - 257;
    std::vector<unsigned char> tail(257);
    if (!source.read_at(tail_offset, tail.data(), tail.size(), actual, error) ||
        actual != tail.size() || !verify_range(tail, tail_offset)) {
        std::cerr << "direct tail payload mismatch: " << error << '\n';
        return 7;
    }

    constexpr std::uint64_t kStreamOffset = 32768;
    constexpr std::uint64_t kStreamSize = 512 * 1024 + 333;
    std::uint64_t streamed = 0;
    if (!source.stream_contiguous(
            kStreamOffset, kStreamSize,
            [&](const unsigned char* data, std::size_t size, std::uint64_t logical_offset) {
                if (logical_offset != streamed) return false;
                for (std::size_t i = 0; i < size; ++i) {
                    if (data[i] != expected_byte(kStreamOffset + logical_offset + i)) return false;
                }
                streamed += size;
                return true;
            },
            {}, error) || streamed != kStreamSize) {
        std::cerr << "contiguous WebDAV stream mismatch: " << error << '\n';
        return 17;
    }

    if (!source.finish(error)) {
        std::cerr << "direct finish failed: " << error << '\n';
        return 8;
    }

    astranas::remote::RemotePackageSource cancel_source(client, entry);
    if (!cancel_source.open(entry.path, error)) {
        std::cerr << "cancel stream prepare failed: " << error << '\n';
        return 14;
    }
    std::uint64_t cancel_data_calls = 0;
    if (cancel_source.stream_contiguous(
            4096, 65537,
            [&](const unsigned char*, std::size_t, std::uint64_t) {
                ++cancel_data_calls;
                return true;
            },
            [](std::uint64_t, std::uint64_t) { return false; }, error)) {
        std::cerr << "cancelled WebDAV stream unexpectedly succeeded\n";
        client.close_direct_read();
        return 15;
    }
    if (error != "cancelled") {
        std::cerr << "cancelled WebDAV stream returned wrong error: " << error << '\n';
        client.close_direct_read();
        return 16;
    }
    if (cancel_data_calls != 0) {
        std::cerr << "cancelled WebDAV stream delivered payload before cancellation\n";
        client.close_direct_read();
        return 18;
    }
    client.close_direct_read();

    RemoteDirEntry weak = entry;
    weak.identity = "last-modified:Sun, 16 Aug 2026 12:00:00 GMT";
    if (client.prepare_direct_read(weak, error)) {
        std::cerr << "weak WebDAV identity unexpectedly allowed direct install\n";
        client.close_direct_read();
        return 9;
    }

    RemoteDirEntry wrong = entry;
    wrong.identity = "etag:\"wrong-etag\"";
    if (client.prepare_direct_read(wrong, error)) {
        std::cerr << "mismatched ETag unexpectedly passed the Range probe\n";
        client.close_direct_read();
        return 10;
    }

    RemoteDirEntry ignored = entry;
    ignored.name = "ignore-range.nsp";
    ignored.path = "/ignore-range.nsp";
    if (client.prepare_direct_read(ignored, error)) {
        std::cerr << "server ignoring Range unexpectedly allowed direct install\n";
        client.close_direct_read();
        return 11;
    }

    RemoteDirEntry changing = entry;
    changing.name = "changing.nsp";
    changing.path = "/changing.nsp";
    if (!client.prepare_direct_read(changing, error)) {
        std::cerr << "changing ETag probe unexpectedly failed: " << error << '\n';
        return 12;
    }
    std::vector<unsigned char> changed_probe(4096);
    if (client.read_range(changing, 8192, changed_probe.data(), changed_probe.size(), actual, error)) {
        std::cerr << "mid-session ETag change unexpectedly succeeded\n";
        client.close_direct_read();
        return 13;
    }
    client.close_direct_read();
    return 0;
}

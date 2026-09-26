// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#ifdef __SWITCH__
extern "C" {
#include <switch/crypto/sha256.h>
}
#endif

using HashProgressCallback = std::function<bool(std::uint64_t processed, std::uint64_t total)>;

class AstraSha256Context {
public:
    AstraSha256Context();

    void reset();
    void update(const void* data, std::size_t size);
    std::array<std::uint8_t, 32> final();

private:
#ifdef __SWITCH__
    Sha256Context hw_ctx_{};
#else
    void transform(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
#endif
};

bool is_valid_sha256_hex(const std::string& value);
std::string normalize_sha256_hex(std::string value);
std::string sha256_digest_hex(const std::array<std::uint8_t, 32>& digest);
bool sha256_file(const std::string& path, std::string& hex_digest, std::string& error,
                 const HashProgressCallback& progress = {});
std::string sha256_bytes(const void* data, std::size_t size);

// SPDX-License-Identifier: GPL-3.0-or-later
#include "sha256.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace {
#ifndef __SWITCH__
constexpr std::array<std::uint32_t, 64> k = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }
#endif

std::string to_hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = hex[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return out;
}
}

AstraSha256Context::AstraSha256Context() { reset(); }

void AstraSha256Context::reset() {
#ifdef __SWITCH__
    sha256ContextCreate(&hw_ctx_);
#else
    state_ = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    total_bytes_ = 0;
    buffer_size_ = 0;
    buffer_.fill(0);
#endif
}

void AstraSha256Context::update(const void* input, std::size_t len) {
#ifdef __SWITCH__
    if (len != 0) sha256ContextUpdate(&hw_ctx_, input, len);
#else
    if (len == 0) return;
    const auto* data = static_cast<const std::uint8_t*>(input);
    total_bytes_ += static_cast<std::uint64_t>(len);
    if (buffer_size_ != 0) {
        const std::size_t take = std::min<std::size_t>(64 - buffer_size_, len);
        std::memcpy(buffer_.data() + buffer_size_, data, take);
        buffer_size_ += take;
        data += take;
        len -= take;
        if (buffer_size_ == 64) {
            transform(buffer_.data());
            buffer_size_ = 0;
        }
    }
    while (len >= 64) {
        transform(data);
        data += 64;
        len -= 64;
    }
    if (len != 0) {
        std::memcpy(buffer_.data(), data, len);
        buffer_size_ = len;
    }
#endif
}

std::array<std::uint8_t, 32> AstraSha256Context::final() {
#ifdef __SWITCH__
    std::array<std::uint8_t, 32> out{};
    sha256ContextGetHash(&hw_ctx_, out.data());
    return out;
#else
    const std::uint64_t bit_len = total_bytes_ * 8u;
    buffer_[buffer_size_++] = 0x80;
    if (buffer_size_ > 56) {
        while (buffer_size_ < 64) buffer_[buffer_size_++] = 0;
        transform(buffer_.data());
        buffer_size_ = 0;
    }
    while (buffer_size_ < 56) buffer_[buffer_size_++] = 0;
    for (int i = 7; i >= 0; --i)
        buffer_[buffer_size_++] = static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xffu);
    transform(buffer_.data());

    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffu);
        out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffu);
        out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffu);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xffu);
    }
    return out;
#endif
}

#ifndef __SWITCH__
void AstraSha256Context::transform(const std::uint8_t* block) {
        std::uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a=state_[0], b=state_[1], c=state_[2], d=state_[3], e=state_[4], f=state_[5], g=state_[6], hh=state_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = hh + s1 + ch + k[static_cast<std::size_t>(i)] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            hh=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=hh;
}
#endif

bool is_valid_sha256_hex(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string normalize_sha256_hex(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string sha256_digest_hex(const std::array<std::uint8_t, 32>& digest) {
    return to_hex(digest);
}

std::string sha256_bytes(const void* data, std::size_t size) {
    AstraSha256Context sha;
    sha.update(static_cast<const std::uint8_t*>(data), size);
    return to_hex(sha.final());
}

bool sha256_file(const std::string& path, std::string& hex_digest, std::string& error,
                 const HashProgressCallback& progress) {
    hex_digest.clear();
    struct stat st{};
    std::uint64_t total = 0;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) total = static_cast<std::uint64_t>(st.st_size);
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { error = "cannot open file for SHA-256"; return false; }
    AstraSha256Context sha;
    std::array<std::uint8_t, 256 * 1024> buffer{};
    std::uint64_t processed = 0;
    for (;;) {
        const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), fp);
        if (got != 0) {
            sha.update(buffer.data(), got);
            processed += static_cast<std::uint64_t>(got);
            if (progress && !progress(processed, total)) {
                std::fclose(fp);
                error = "SHA-256 verification cancelled";
                return false;
            }
        }
        if (got < buffer.size()) {
            if (std::ferror(fp)) {
                std::fclose(fp);
                error = "file read failed during SHA-256";
                return false;
            }
            break;
        }
    }
    std::fclose(fp);
    hex_digest = to_hex(sha.final());
    return true;
}

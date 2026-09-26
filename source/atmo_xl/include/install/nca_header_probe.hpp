// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "install/nca.hpp"
#include "sha256.hpp"
#include "util/crypto.hpp"
#include "util/error.hpp"

namespace tin::install {

enum class NcaHeaderMode {
    Invalid,
    Encrypted,
    Plaintext,
};

struct NcaHeaderDecodeResult {
    NcaHeader header{};
    NcaHeaderMode mode = NcaHeaderMode::Invalid;
    u32 rawMagic = 0;
    u32 decryptedMagic = 0;
};

inline const char* NcaHeaderModeLabel(NcaHeaderMode mode)
{
    switch (mode) {
        case NcaHeaderMode::Encrypted: return "encrypted";
        case NcaHeaderMode::Plaintext: return "plaintext";
        default: return "invalid";
    }
}

inline NcaHeaderDecodeResult DecodeNcaHeaderBytes(const void* bytes, std::size_t size)
{
    NcaHeaderDecodeResult result{};
    if (!bytes || size < sizeof(NcaHeader)) return result;

    NcaHeader raw{};
    std::memcpy(&raw, bytes, sizeof(raw));
    result.rawMagic = raw.magic;

    // Some tooling can emit a plaintext NCA header. Detect it before attempting
    // AES-XTS so a correct raw NCA3 header is not transformed into random bytes.
    if (raw.magic == MAGIC_NCA3 && raw.nca_size >= sizeof(NcaHeader)) {
        result.header = raw;
        result.mode = NcaHeaderMode::Plaintext;
        result.decryptedMagic = raw.magic;
        return result;
    }

    NcaHeader decrypted = raw;
    Crypto::Keys keys;
    Crypto::AesXtr decryptor(keys.headerKey, false);
    decryptor.decrypt(&decrypted, &decrypted, sizeof(decrypted), 0, 0x200);
    result.decryptedMagic = decrypted.magic;
    if (decrypted.magic == MAGIC_NCA3 && decrypted.nca_size >= sizeof(NcaHeader)) {
        result.header = decrypted;
        result.mode = NcaHeaderMode::Encrypted;
    }
    return result;
}

inline std::string NcaHeaderKeySelfTest(NcmStorageId preferredStorage)
{
    Crypto::Keys keys;
    const std::array<NcmStorageId, 3> candidates = {
        preferredStorage, NcmStorageId_BuiltInUser, NcmStorageId_BuiltInSystem
    };
    bool sawReadable = false;

    for (std::size_t storageIndex = 0; storageIndex < candidates.size(); ++storageIndex) {
        const NcmStorageId storageId = candidates[storageIndex];
        bool duplicate = false;
        for (std::size_t i = 0; i < storageIndex; ++i)
            duplicate = duplicate || candidates[i] == storageId;
        if (duplicate) continue;

        NcmContentStorage storage{};
        if (R_FAILED(ncmOpenContentStorage(&storage, storageId))) continue;

        s32 count = 0;
        const Result countRc = ncmContentStorageGetContentCount(&storage, &count);
        if (R_FAILED(countRc) || count <= 0) {
            serviceClose(&storage.s);
            continue;
        }

        std::array<NcmContentId, 8> ids{};
        s32 written = 0;
        const Result listRc = ncmContentStorageListContentId(
            &storage, ids.data(), static_cast<s32>(ids.size()), &written, 0);
        if (R_FAILED(listRc) || written <= 0) {
            serviceClose(&storage.s);
            continue;
        }

        written = std::min<s32>(written, static_cast<s32>(ids.size()));
        for (s32 i = 0; i < written; ++i) {
            s64 contentSize = 0;
            if (R_FAILED(ncmContentStorageGetSizeFromContentId(
                    &storage, &contentSize, &ids[static_cast<std::size_t>(i)])) ||
                contentSize < static_cast<s64>(sizeof(NcaHeader)))
                continue;

            std::array<u8, sizeof(NcaHeader)> raw{};
            if (R_FAILED(ncmContentStorageReadContentIdFile(
                    &storage, raw.data(), raw.size(),
                    &ids[static_cast<std::size_t>(i)], 0)))
                continue;

            sawReadable = true;
            NcaHeader header{};
            std::memcpy(&header, raw.data(), sizeof(header));
            Crypto::AesXtr decryptor(keys.headerKey, false);
            decryptor.decrypt(&header, &header, sizeof(header), 0, 0x200);
            if (header.magic == MAGIC_NCA3 && header.nca_size >= sizeof(NcaHeader)) {
                serviceClose(&storage.s);
                return "pass";
            }
        }
        serviceClose(&storage.s);
    }
    return sawReadable ? "fail" : "unavailable";
}

template <typename Reader>
NcaHeader ReadValidatedNcaHeader(const char* container,
                                 const std::string& fileName,
                                 u64 absoluteOffset,
                                 u64 entrySize,
                                 bool compressed,
                                 NcmStorageId selfTestStorage,
                                 Reader&& readHeader)
{
    const u64 minimumEntrySize = compressed ? NCA_HEADER_SIZE : sizeof(NcaHeader);
    if (entrySize < minimumEntrySize)
        THROW_FORMAT("%s NCA entry is truncated [file=%s offset=0x%lx size=0x%lx need=0x%lx]",
                     container, fileName.c_str(), absoluteOffset, entrySize, minimumEntrySize);

    std::array<u8, sizeof(NcaHeader)> raw{};
    readHeader(raw.data(), raw.size());
    const auto decoded = DecodeNcaHeaderBytes(raw.data(), raw.size());

    const u64 minimumDeclaredSize = compressed ? NCA_HEADER_SIZE : sizeof(NcaHeader);
    if (decoded.mode != NcaHeaderMode::Invalid &&
        decoded.header.nca_size >= minimumDeclaredSize) {
        if (decoded.mode == NcaHeaderMode::Plaintext) {
            const std::string keySelfTest = NcaHeaderKeySelfTest(selfTestStorage);
            if (keySelfTest == "fail")
                THROW_FORMAT("%s plaintext NCA header needs re-encryption but header-key self-test failed [file=%s offset=0x%lx entry=0x%lx raw_magic=0x%08x header_mode=plaintext key_self_test=fail]",
                             container, fileName.c_str(), absoluteOffset, entrySize,
                             decoded.rawMagic);
        }
        return decoded.header;
    }

    // Re-read the exact same range before reporting an invalid header. A mismatch
    // means the random-read source is unstable; a match plus key_self_test=pass
    // means the same non-NCA bytes were stably returned at this package offset.
    std::array<u8, sizeof(NcaHeader)> reread{};
    readHeader(reread.data(), reread.size());
    const bool sameRead = raw == reread;
    const auto secondDecoded = DecodeNcaHeaderBytes(reread.data(), reread.size());
    const std::string firstHash = sha256_bytes(raw.data(), raw.size());
    const std::string secondHash = sha256_bytes(reread.data(), reread.size());
    const std::string keySelfTest = NcaHeaderKeySelfTest(selfTestStorage);

    THROW_FORMAT("%s invalid NCA header [file=%s offset=0x%lx entry=0x%lx raw_magic=0x%08x decrypted_magic=0x%08x header_mode=%s key_self_test=%s reread=%s sha256=%s/%s second_raw=0x%08x second_decrypted=0x%08x]",
                 container, fileName.c_str(), absoluteOffset, entrySize,
                 decoded.rawMagic, decoded.decryptedMagic, NcaHeaderModeLabel(decoded.mode),
                 keySelfTest.c_str(), sameRead ? "same" : "DIFFERENT",
                 firstHash.c_str(), secondHash.c_str(),
                 secondDecoded.rawMagic, secondDecoded.decryptedMagic);
}

} // namespace tin::install

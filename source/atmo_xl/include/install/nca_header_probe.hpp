// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstring>
#include <string>

#include "install/nca.hpp"
#include "sha256.hpp"
#include "util/crypto.hpp"
#include "util/error.hpp"

namespace tin::install {

template <typename Reader>
NcaHeader ReadValidatedNcaHeader(const char* container,
                                 const std::string& fileName,
                                 u64 absoluteOffset,
                                 u64 entrySize,
                                 bool compressed,
                                 Reader&& readHeader)
{
    const u64 minimumEntrySize = compressed ? NCA_HEADER_SIZE : sizeof(NcaHeader);
    if (entrySize < minimumEntrySize)
        THROW_FORMAT("%s NCA entry is truncated [file=%s offset=0x%lx size=0x%lx need=0x%lx]",
                     container, fileName.c_str(), absoluteOffset, entrySize, minimumEntrySize);

    std::array<u8, sizeof(NcaHeader)> raw{};
    readHeader(raw.data(), raw.size());

    NcaHeader header{};
    std::memcpy(&header, raw.data(), sizeof(header));
    Crypto::Keys keys;
    Crypto::AesXtr decryptor(keys.headerKey, false);
    decryptor.decrypt(&header, &header, sizeof(header), 0, 0x200);

    const u64 minimumDeclaredSize = compressed ? NCA_HEADER_SIZE : sizeof(NcaHeader);
    if (header.magic == MAGIC_NCA3 && header.nca_size >= minimumDeclaredSize)
        return header;

    // Re-read the exact same range before reporting an invalid header. This turns
    // the next field report into a useful SMB/WebDAV diagnostic: a mismatch means
    // the random-read source is unstable, while a match means the same bytes were
    // returned twice and the package/offset/key path can be investigated directly.
    std::array<u8, sizeof(NcaHeader)> reread{};
    readHeader(reread.data(), reread.size());
    const bool sameRead = raw == reread;
    const std::string firstHash = sha256_bytes(raw.data(), raw.size());
    const std::string secondHash = sha256_bytes(reread.data(), reread.size());

    THROW_FORMAT("%s invalid NCA header [file=%s offset=0x%lx entry=0x%lx magic=0x%08x declared=0x%lx reread=%s sha256=%s/%s]",
                 container, fileName.c_str(), absoluteOffset, entrySize,
                 header.magic, header.nca_size, sameRead ? "same" : "DIFFERENT",
                 firstHash.c_str(), secondHash.c_str());
}

} // namespace tin::install

/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "nx/nca_writer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <zstd.h>

#include "install/nca.hpp"
#include "install/nca_header_probe.hpp"
#include "install_performance.hpp"
#include "util/config.hpp"
#include "util/crypto.hpp"
#include "util/error.hpp"

namespace {

constexpr u64 kNczSectionMagic = 0x4E544345535A434EULL; // "NCZSECTN"
constexpr u64 kNczBlockMagic = 0x4B434F4C425A434EULL;   // "NCZBLOCK"
// NCZ section ranges may begin at the 0xC00 crypto-header boundary even though
// the stream preserves a 0x4000-byte prefix from the original NCA.
constexpr u64 kNcaCryptoHeaderSize = sizeof(tin::install::NcaHeader);
// BKTR update NCAs can legitimately expand to thousands of NCZ crypto sections.
constexpr u64 kMaxNczSections = 0xFFFFULL;
// Bound the table allocation itself instead of imposing an arbitrary block-count
// ceiling. 64 MiB permits up to 16,777,216 entries (256 GiB at 16 KiB blocks).
constexpr u64 kMaxNczBlockTableBytes = 64ULL * 1024ULL * 1024ULL;

u32 read_u32(const u8* p) {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

u64 read_u64(const u8* p) {
    u64 value = 0;
    for (unsigned int i = 0; i < 8; ++i) value |= static_cast<u64>(p[i]) << (i * 8);
    return value;
}

NcmPlaceHolderId placeholder_id_for(const NcmContentId& contentId) {
    static_assert(sizeof(NcmPlaceHolderId) == sizeof(NcmContentId),
                  "Content and placeholder IDs must have equal size");
    NcmPlaceHolderId placeholder{};
    std::memcpy(&placeholder, &contentId, sizeof(placeholder));
    return placeholder;
}

void append(std::vector<u8>& buffer, const u8* ptr, u64 size) {
    if (size > std::numeric_limits<std::size_t>::max() - buffer.size())
        THROW_FORMAT("NCZ buffer size overflow");
    const std::size_t offset = buffer.size();
    buffer.resize(offset + static_cast<std::size_t>(size));
    std::memcpy(buffer.data() + offset, ptr, static_cast<std::size_t>(size));
}

class NczSection {
public:
    u64 offset;
    u64 size;
    u8 cryptoType;
    u8 padding1[7];
    u64 padding2;
    u8 cryptoKey[0x10];
    u8 cryptoCounter[0x10];
} __attribute__((packed));

static_assert(sizeof(NczSection) == 0x40, "Unexpected NCZ section layout");

class NczSectionContext : public NczSection {
public:
    explicit NczSectionContext(const NczSection& section)
        : NczSection(section),
          crypto(section.cryptoKey,
                 Crypto::AesCtr(Crypto::swapEndian(read_u64(section.cryptoCounter)))) {}

    void encrypt(void* data, u64 chunkSize, u64 absoluteOffset) {
        // Canonical NSZ normally serializes BKTR subsections as CTR (3), while
        // compatible installers also accept explicit BKTR (4) section records.
        if ((cryptoType != 3 && cryptoType != 4) || chunkSize == 0) return;
        crypto.seek(absoluteOffset);
        const auto begin = std::chrono::steady_clock::now();
        crypto.encrypt(data, data, static_cast<std::size_t>(chunkSize));
        const auto end = std::chrono::steady_clock::now();
        astranas::install_performance::add_aes(
            chunkSize,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
    }

    Crypto::Aes128Ctr crypto;
};

class NczBodyWriter final : public NcaBodyWriter {
public:
    NczBodyWriter(const NcmContentId& ncaId, u64 offset, u64 expectedSize,
                  std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
                  AstraSha256Context* hashContext,
                  const NcmPlaceHolderId& placeholderId)
        : NcaBodyWriter(ncaId, offset, expectedSize, contentStorage, hashContext,
                        placeholderId),
          m_dctx(ZSTD_createDCtx()),
          m_zstdOutput(ZSTD_DStreamOutSize()) {
        if (!m_dctx) THROW_FORMAT("failed to create Zstandard decompressor");
        // Do not clamp the frame window below libzstd's own decoder policy.
        // NSZ can be produced with long-distance mode, and upstream installers
        // let libzstd decide whether the encoded window is supportable.
    }

    ~NczBodyWriter() override {
        if (m_dctx) ZSTD_freeDCtx(m_dctx);
    }

    u64 write(const u8* ptr, u64 size) override {
        if (m_closed) THROW_FORMAT("attempted to write a closed NCZ stream");
        const u64 originalSize = size;
        append(m_pending, ptr, size);
        process();
        return originalSize;
    }

    bool close() override {
        if (m_closed) return true;
        process();

        if (!m_sectionsReady)
            THROW_FORMAT("truncated NCZ section header");
        if (m_mode == Mode::Detect)
            THROW_FORMAT("truncated NCZ compression header");
        if (m_mode == Mode::BlockHeader)
            THROW_FORMAT("truncated NCZ block header");

        if (m_mode == Mode::Solid) {
            if (m_lastZstdResult != 0) flush_solid_output();
            if (m_lastZstdResult != 0)
                THROW_FORMAT("truncated NCZ Zstandard stream");
        } else if (m_mode == Mode::Blocks) {
            if (m_currentBlock != m_blockSizes.size() || m_blockInputRemaining != 0)
                THROW_FORMAT("truncated NCZ block data");
            if (m_blockOutputWritten != m_blockDecompressedSize)
                THROW_FORMAT("NCZ block output size mismatch");
        }

        if (!m_pending.empty())
            THROW_FORMAT("unexpected trailing NCZ data");
        if (m_offset != m_expectedSize)
            THROW_FORMAT("NCZ output size mismatch: expected 0x%lx, wrote 0x%lx",
                         m_expectedSize, m_offset);

        m_closed = true;
        return true;
    }

private:
    enum class Mode { Sections, Detect, Solid, BlockHeader, Blocks };

    void process() {
        bool advanced = true;
        while (advanced) {
            advanced = false;
            switch (m_mode) {
                case Mode::Sections: advanced = parse_sections(); break;
                case Mode::Detect: advanced = detect_compression(); break;
                case Mode::Solid: advanced = process_solid(); break;
                case Mode::BlockHeader: advanced = parse_block_header(); break;
                case Mode::Blocks: advanced = process_blocks(); break;
            }
        }
    }

    bool parse_sections() {
        if (m_pending.size() < 16) return false;
        const u64 magic = read_u64(m_pending.data());
        if (magic != kNczSectionMagic)
            THROW_FORMAT("invalid NCZ section magic: got=0x%lx nca_offset=0x%lx expected_size=0x%lx",
                         magic, m_offset, m_expectedSize);

        const u64 count = read_u64(m_pending.data() + 8);
        if (count == 0 || count > kMaxNczSections) {
            const u8* r = m_pending.data();
            THROW_FORMAT("invalid NCZ section count: count=%lu nca_offset=0x%lx expected_size=0x%lx raw16=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                         count, m_offset, m_expectedSize,
                         r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                         r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
        }
        if (count > (std::numeric_limits<std::size_t>::max() - 16) / sizeof(NczSection))
            THROW_FORMAT("NCZ section header size overflow");

        const std::size_t headerSize = 16 + static_cast<std::size_t>(count) * sizeof(NczSection);
        if (m_pending.size() < headerSize) return false;

        m_sections.reserve(static_cast<std::size_t>(count));
        u64 previousEnd = kNcaCryptoHeaderSize;
        for (u64 i = 0; i < count; ++i) {
            NczSection section{};
            std::memcpy(&section, m_pending.data() + 16 + i * sizeof(NczSection), sizeof(section));
            if (section.offset < kNcaCryptoHeaderSize || section.size == 0 ||
                section.offset > m_expectedSize || section.size > m_expectedSize - section.offset ||
                section.offset < previousEnd)
                THROW_FORMAT("invalid or overlapping NCZ section");
            previousEnd = section.offset + section.size;
            m_sections.emplace_back(std::make_unique<NczSectionContext>(section));
        }

        consume(headerSize);
        m_sectionsReady = true;
        m_mode = Mode::Detect;
        return true;
    }

    bool detect_compression() {
        if (m_pending.size() < sizeof(u64)) return false;
        if (read_u64(m_pending.data()) == kNczBlockMagic) {
            m_mode = Mode::BlockHeader;
        } else {
            m_mode = Mode::Solid;
        }
        return true;
    }

    bool parse_block_header() {
        constexpr std::size_t fixedSize = 24;
        if (m_pending.size() < fixedSize) return false;
        if (read_u64(m_pending.data()) != kNczBlockMagic)
            THROW_FORMAT("invalid NCZ block magic");

        const u8 version = m_pending[8];
        const u8 type = m_pending[9];
        const u8 unused = m_pending[10];
        const u8 exponent = m_pending[11];
        const u32 blockCount = read_u32(m_pending.data() + 12);
        const u64 decompressedSize = read_u64(m_pending.data() + 16);
        if (version != 2 || type != 1 || unused != 0)
            THROW_FORMAT("unsupported NCZ block header version or type");
        if (exponent < 14 || exponent > 32)
            THROW_FORMAT("invalid NCZ block size exponent");
        if (blockCount == 0)
            THROW_FORMAT("invalid NCZ block count");

        const u64 blockSize = 1ULL << exponent;
        if (decompressedSize != m_expectedSize - NCA_HEADER_SIZE)
            THROW_FORMAT("NCZ block decompressed size does not match NCA size");
        const u64 expectedBlocks = decompressedSize / blockSize +
                                   (decompressedSize % blockSize != 0 ? 1 : 0);
        if (expectedBlocks != blockCount)
            THROW_FORMAT("NCZ block count does not match decompressed size");

        const u64 tableBytes = static_cast<u64>(blockCount) * sizeof(u32);
        if (tableBytes > kMaxNczBlockTableBytes)
            THROW_FORMAT("NCZ block table is unreasonably large");
        if (blockCount > (std::numeric_limits<std::size_t>::max() - fixedSize) / sizeof(u32))
            THROW_FORMAT("NCZ block header size overflow");
        const std::size_t headerSize = fixedSize + static_cast<std::size_t>(tableBytes);
        if (m_pending.size() < headerSize) return false;

        m_blockSize = blockSize;
        m_blockDecompressedSize = decompressedSize;
        m_blockSizes.reserve(blockCount);
        for (u32 i = 0; i < blockCount; ++i) {
            const u32 compressedSize = read_u32(m_pending.data() + fixedSize + i * sizeof(u32));
            const u64 producedBefore = static_cast<u64>(i) * m_blockSize;
            const u64 expectedOutput = std::min(m_blockSize, m_blockDecompressedSize - producedBefore);
            if (compressedSize == 0 || compressedSize > expectedOutput)
                THROW_FORMAT("invalid NCZ compressed block size");
            m_blockSizes.push_back(compressedSize);
        }

        consume(headerSize);
        m_mode = Mode::Blocks;
        return true;
    }

    bool process_solid() {
        if (m_pending.empty()) return false;
        ZSTD_inBuffer input{m_pending.data(), m_pending.size(), 0};
        bool outputWasFull = false;
        do {
            ZSTD_outBuffer output{m_zstdOutput.data(), m_zstdOutput.size(), 0};
            const auto zstdBegin = std::chrono::steady_clock::now();
            m_lastZstdResult = ZSTD_decompressStream(m_dctx, &output, &input);
            const auto zstdEnd = std::chrono::steady_clock::now();
            astranas::install_performance::add_zstd(
                output.pos,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(zstdEnd - zstdBegin).count()));
            if (ZSTD_isError(m_lastZstdResult))
                THROW_FORMAT("NCZ decompression failed: %s", ZSTD_getErrorName(m_lastZstdResult));
            if (output.pos) write_output(m_zstdOutput.data(), output.pos);
            outputWasFull = output.pos == output.size && m_lastZstdResult != 0;
        } while (input.pos < input.size || outputWasFull);
        consume(input.pos);
        return input.pos != 0;
    }

    void flush_solid_output() {
        for (;;) {
            ZSTD_inBuffer input{nullptr, 0, 0};
            ZSTD_outBuffer output{m_zstdOutput.data(), m_zstdOutput.size(), 0};
            const auto zstdBegin = std::chrono::steady_clock::now();
            m_lastZstdResult = ZSTD_decompressStream(m_dctx, &output, &input);
            const auto zstdEnd = std::chrono::steady_clock::now();
            astranas::install_performance::add_zstd(
                output.pos,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(zstdEnd - zstdBegin).count()));
            if (ZSTD_isError(m_lastZstdResult))
                THROW_FORMAT("NCZ decompression failed: %s", ZSTD_getErrorName(m_lastZstdResult));
            if (output.pos) write_output(m_zstdOutput.data(), output.pos);
            if (output.pos == 0 || m_lastZstdResult == 0) break;
        }
    }

    bool process_blocks() {
        bool advanced = false;
        while (m_currentBlock < m_blockSizes.size()) {
            if (m_blockInputRemaining == 0) begin_block();
            if (m_pending.empty()) break;

            const std::size_t available = static_cast<std::size_t>(
                std::min<u64>(m_pending.size(), m_blockInputRemaining));
            if (m_currentBlockCompressed) {
                ZSTD_inBuffer input{m_pending.data(), available, 0};
                bool outputWasFull = false;
                do {
                    ZSTD_outBuffer output{m_zstdOutput.data(), m_zstdOutput.size(), 0};
                    const auto zstdBegin = std::chrono::steady_clock::now();
                    m_lastZstdResult = ZSTD_decompressStream(m_dctx, &output, &input);
                    const auto zstdEnd = std::chrono::steady_clock::now();
                    astranas::install_performance::add_zstd(
                        output.pos,
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(zstdEnd - zstdBegin).count()));
                    if (ZSTD_isError(m_lastZstdResult))
                        THROW_FORMAT("NCZ block decompression failed: %s", ZSTD_getErrorName(m_lastZstdResult));
                    if (output.pos) {
                        write_output(m_zstdOutput.data(), output.pos);
                        m_currentBlockOutput += output.pos;
                    }
                    outputWasFull = output.pos == output.size && m_lastZstdResult != 0;
                } while (input.pos < input.size || outputWasFull);
                consume(input.pos);
                m_blockInputRemaining -= input.pos;
                advanced = advanced || input.pos != 0;
            } else {
                write_output(m_pending.data(), available);
                consume(available);
                m_blockInputRemaining -= available;
                m_currentBlockOutput += available;
                advanced = true;
            }

            if (m_currentBlockOutput > m_currentBlockExpected)
                THROW_FORMAT("NCZ block produced too much output");
            if (m_blockInputRemaining == 0) finish_block();
        }
        return advanced;
    }

    void begin_block() {
        const u64 producedBefore = static_cast<u64>(m_currentBlock) * m_blockSize;
        m_currentBlockExpected = std::min(m_blockSize, m_blockDecompressedSize - producedBefore);
        const u64 compressedSize = m_blockSizes[m_currentBlock];
        if (compressedSize == 0 || compressedSize > m_currentBlockExpected)
            THROW_FORMAT("invalid NCZ compressed block size");

        m_blockInputRemaining = compressedSize;
        m_currentBlockOutput = 0;
        m_currentBlockCompressed = compressedSize < m_currentBlockExpected;
        m_lastZstdResult = 1;
        if (m_currentBlockCompressed) {
            const size_t resetResult = ZSTD_DCtx_reset(m_dctx, ZSTD_reset_session_only);
            if (ZSTD_isError(resetResult))
                THROW_FORMAT("failed to reset NCZ block decompressor: %s", ZSTD_getErrorName(resetResult));
        }
    }

    void finish_block() {
        if (m_currentBlockOutput != m_currentBlockExpected)
            THROW_FORMAT("NCZ block output size mismatch");
        if (m_currentBlockCompressed && m_lastZstdResult != 0)
            THROW_FORMAT("truncated NCZ compressed block");
        m_blockOutputWritten += m_currentBlockOutput;
        ++m_currentBlock;
        m_currentBlockOutput = 0;
        m_currentBlockExpected = 0;
    }

    void write_output(const u8* data, std::size_t size) {
        if (size == 0) return;
        if (m_offset > m_expectedSize || size > m_expectedSize - m_offset)
            THROW_FORMAT("NCZ output exceeds declared NCA size");

        m_cryptoBuffer.assign(data, data + size);
        std::size_t position = 0;
        while (position < m_cryptoBuffer.size()) {
            const u64 absoluteOffset = m_offset + position;
            NczSectionContext* active = nullptr;
            u64 nextSectionOffset = m_expectedSize;
            for (const auto& section : m_sections) {
                const u64 sectionEnd = section->offset + section->size;
                if (absoluteOffset >= section->offset && absoluteOffset < sectionEnd) {
                    active = section.get();
                    nextSectionOffset = sectionEnd;
                    break;
                }
                if (section->offset > absoluteOffset)
                    nextSectionOffset = std::min(nextSectionOffset, section->offset);
            }

            const u64 remaining = m_cryptoBuffer.size() - position;
            const u64 chunk = std::min(remaining, nextSectionOffset - absoluteOffset);
            if (chunk == 0) THROW_FORMAT("invalid NCZ section boundary");
            if (active) active->encrypt(m_cryptoBuffer.data() + position, chunk, absoluteOffset);
            position += static_cast<std::size_t>(chunk);
        }

        NcaBodyWriter::write(m_cryptoBuffer.data(), m_cryptoBuffer.size());
    }

    void consume(std::size_t size) {
        if (size == 0) return;
        if (size > m_pending.size()) THROW_FORMAT("internal NCZ buffer underflow");
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(size));
    }

    Mode m_mode = Mode::Sections;
    bool m_sectionsReady = false;
    std::vector<u8> m_pending;
    std::vector<std::unique_ptr<NczSectionContext>> m_sections;
    ZSTD_DCtx* m_dctx = nullptr;
    std::vector<u8> m_zstdOutput;
    std::vector<u8> m_cryptoBuffer;
    size_t m_lastZstdResult = 1;

    u64 m_blockSize = 0;
    u64 m_blockDecompressedSize = 0;
    u64 m_blockOutputWritten = 0;
    std::vector<u32> m_blockSizes;
    std::size_t m_currentBlock = 0;
    u64 m_blockInputRemaining = 0;
    u64 m_currentBlockExpected = 0;
    u64 m_currentBlockOutput = 0;
    bool m_currentBlockCompressed = false;
};

} // namespace

NcaBodyWriter::NcaBodyWriter(const NcmContentId& ncaId, u64 offset, u64 expectedSize,
                             std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
                             AstraSha256Context* hashContext,
                             const NcmPlaceHolderId& placeholderId)
    : m_contentStorage(contentStorage), m_ncaId(ncaId), m_placeholderId(placeholderId), m_offset(offset),
      m_expectedSize(expectedSize), m_hashContext(hashContext) {}

NcaBodyWriter::~NcaBodyWriter() = default;

u64 NcaBodyWriter::write(const u8* ptr, u64 size) {
    if (m_closed) THROW_FORMAT("attempted to write a closed NCA stream");
    if (!isOpen()) THROW_FORMAT("NCA placeholder is not open");
    if (m_offset > m_expectedSize || size > m_expectedSize - m_offset)
        THROW_FORMAT("NCA content exceeds declared size");
    if (size) {
        if (inst::config::verifyNcaContentHashes) {
            if (!m_hashContext) THROW_FORMAT("NCA 哈希上下文不可用");
            m_hashContext->update(ptr, static_cast<std::size_t>(size));
        }
        const auto begin = std::chrono::steady_clock::now();
        m_contentStorage->WritePlaceholder(m_placeholderId, m_offset,
                                           const_cast<u8*>(ptr), static_cast<std::size_t>(size));
        const auto end = std::chrono::steady_clock::now();
        astranas::install_performance::add_ncm_write(
            size,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
        m_offset += size;
    }
    return size;
}

bool NcaBodyWriter::close() {
    if (m_closed) return true;
    if (m_offset != m_expectedSize)
        THROW_FORMAT("truncated NCA content: expected 0x%lx, wrote 0x%lx", m_expectedSize, m_offset);
    m_closed = true;
    return true;
}

bool NcaBodyWriter::isOpen() const {
    return m_contentStorage != nullptr;
}

NcaWriter::NcaWriter(const NcmContentId& ncaId,
                     std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
                     const u8* expectedHash)
    : NcaWriter(ncaId, placeholder_id_for(ncaId), contentStorage, expectedHash) {}

NcaWriter::NcaWriter(const NcmContentId& ncaId, const NcmPlaceHolderId& placeholderId,
                     std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
                     const u8* expectedHash)
    : m_ncaId(ncaId), m_placeholderId(placeholderId), m_contentStorage(contentStorage),
      m_writer(nullptr) {
    if (expectedHash) {
        std::memcpy(m_expectedHash.data(), expectedHash, m_expectedHash.size());
        m_hasExpectedHash = true;
    }
}

NcaWriter::~NcaWriter() {
    try { close(); } catch (...) {}
}

bool NcaWriter::close() {
    if (m_closed) return true;

    if (!m_headerFlushed) {
        if (!m_headerParsed) {
            if (m_buffer.size() < sizeof(tin::install::NcaHeader))
                THROW_FORMAT("truncated NCA crypto header: need 0x%lx, got 0x%lx",
                             static_cast<u64>(sizeof(tin::install::NcaHeader)),
                             static_cast<u64>(m_buffer.size()));
            parseHeader();
        }
        if (m_buffer.size() != m_prefixSize)
            THROW_FORMAT("truncated NCA prefix: expected 0x%lx, got 0x%lx",
                         m_prefixSize, static_cast<u64>(m_buffer.size()));
        flushHeader();
    }

    if (!m_writer && !m_bodyProbe.empty()) {
        m_writer = std::make_shared<NcaBodyWriter>(m_ncaId, m_prefixSize, m_ncaSize,
                                                  m_contentStorage, &m_hashContext,
                                                  m_placeholderId);
        m_writer->write(m_bodyProbe.data(), m_bodyProbe.size());
        m_bodyProbe.clear();
    }

    if (m_writer) m_writer->close();
    else if (m_ncaSize != m_prefixSize)
        THROW_FORMAT("NCA body is missing: prefix=0x%lx declared=0x%lx",
                     m_prefixSize, m_ncaSize);

    if (inst::config::verifyNcaContentHashes) {
        if (!m_hashFinalized) {
            m_actualHash = m_hashContext.final();
            m_hashFinalized = true;
        }
        const char* headerMode = m_headerPlaintext ? "plaintext" : "encrypted";
        if (std::memcmp(m_actualHash.data(), &m_ncaId, sizeof(m_ncaId)) != 0)
            THROW_FORMAT("NCA SHA-256 与内容 ID 不匹配 [header_mode=%s prefix=0x%lx declared=0x%lx]",
                         headerMode, m_prefixSize, m_ncaSize);
        if (m_hasExpectedHash && m_actualHash != m_expectedHash)
            THROW_FORMAT("NCA SHA-256 与 CNMT 内容哈希不匹配 [header_mode=%s prefix=0x%lx declared=0x%lx]",
                         headerMode, m_prefixSize, m_ncaSize);
    }

    m_writer.reset();
    m_contentStorage.reset();
    m_closed = true;
    return true;
}

bool NcaWriter::isOpen() const {
    return static_cast<bool>(m_contentStorage);
}

u64 NcaWriter::write(const u8* ptr, u64 size) {
    if (m_closed) THROW_FORMAT("attempted to write a closed NCA");
    const u64 originalSize = size;

    // All NCA variants have a 0xC00 crypto header, but NCZ keeps a 0x4000
    // uncompressed prefix. Parse at 0xC00 first, then choose the real prefix
    // target dynamically so legal small NCAs (for example 0xE00 CNMTs) do not
    // get rejected while NCZ retains its canonical 0x4000 boundary.
    while (!m_headerFlushed && size) {
        const u64 target = m_headerParsed ? m_prefixSize : sizeof(tin::install::NcaHeader);
        if (m_buffer.size() > target)
            THROW_FORMAT("internal NCA prefix overflow");
        const u64 needed = target - m_buffer.size();
        const u64 chunk = std::min(needed, size);
        append(m_buffer, ptr, chunk);
        ptr += chunk;
        size -= chunk;

        if (!m_headerParsed && m_buffer.size() == sizeof(tin::install::NcaHeader))
            parseHeader();
        if (m_headerParsed && m_buffer.size() == m_prefixSize)
            flushHeader();
    }

    if (size && !m_writer) {
        const u64 needed = sizeof(u64) - m_bodyProbe.size();
        const u64 chunk = std::min(needed, size);
        append(m_bodyProbe, ptr, chunk);
        ptr += chunk;
        size -= chunk;

        if (m_bodyProbe.size() == sizeof(u64)) {
            if (read_u64(m_bodyProbe.data()) == kNczSectionMagic)
                m_writer = std::make_shared<NczBodyWriter>(m_ncaId, m_prefixSize, m_ncaSize,
                                                           m_contentStorage, &m_hashContext,
                                                           m_placeholderId);
            else
                m_writer = std::make_shared<NcaBodyWriter>(m_ncaId, m_prefixSize, m_ncaSize,
                                                           m_contentStorage, &m_hashContext,
                                                           m_placeholderId);
            m_writer->write(m_bodyProbe.data(), m_bodyProbe.size());
            m_bodyProbe.clear();
        }
    }

    if (size) {
        if (!m_writer) THROW_FORMAT("failed to select NCA body writer");
        m_writer->write(ptr, size);
    }
    return originalSize;
}

void NcaWriter::parseHeader() {
    if (m_headerParsed) return;
    if (m_buffer.size() < sizeof(tin::install::NcaHeader))
        THROW_FORMAT("cannot parse an incomplete NCA crypto header");

    const auto decoded = tin::install::DecodeNcaHeaderBytes(m_buffer.data(), m_buffer.size());
    if (decoded.mode == tin::install::NcaHeaderMode::Invalid)
        THROW_FORMAT("invalid NCA header during stream [raw_magic=0x%08x decrypted_magic=0x%08x]",
                     decoded.rawMagic, decoded.decryptedMagic);

    m_ncaSize = decoded.header.nca_size;
    if (m_ncaSize < sizeof(tin::install::NcaHeader))
        THROW_FORMAT("NCA declared size is smaller than its crypto header");
    m_prefixSize = std::min<u64>(NCA_HEADER_SIZE, m_ncaSize);
    m_headerPlaintext = decoded.mode == tin::install::NcaHeaderMode::Plaintext;
    m_headerParsed = true;
}

void NcaWriter::flushHeader() {
    if (m_headerFlushed) return;
    if (!m_headerParsed) parseHeader();
    if (m_buffer.size() != m_prefixSize)
        THROW_FORMAT("cannot flush an incomplete NCA prefix");

    const auto decoded = tin::install::DecodeNcaHeaderBytes(m_buffer.data(), m_buffer.size());
    if (decoded.mode == tin::install::NcaHeaderMode::Invalid)
        THROW_FORMAT("invalid NCA header during prefix flush [raw_magic=0x%08x decrypted_magic=0x%08x]",
                     decoded.rawMagic, decoded.decryptedMagic);
    if (decoded.header.nca_size != m_ncaSize)
        THROW_FORMAT("NCA declared size changed while streaming");

    // For a normal encrypted source, validate the source bytes exactly as they
    // arrived (preserving the historical gamecard->download conversion behavior).
    if (inst::config::verifyNcaContentHashes && !m_headerPlaintext)
        m_hashContext.update(m_buffer.data(), m_buffer.size());

    if (!isOpen()) THROW_FORMAT("NCA placeholder storage is closed");
    m_contentStorage->CreatePlaceholder(m_ncaId, m_placeholderId,
                                        static_cast<std::size_t>(m_ncaSize));

    tin::install::NcaHeader header = decoded.header;
    const bool needsHeaderRewrite = m_headerPlaintext || header.distribution == 1;
    if (header.distribution == 1) header.distribution = 0;

    // Keep a valid encrypted NSP header byte-for-byte. A decrypt/re-encrypt
    // round trip is unnecessary unless converting a gamecard NCA or encrypting
    // a plaintext header, and can disturb the filesystem-header hash material.
    // Gamecard-to-NCM conversion retains AtmoXL's distribution-flag rewrite.
    if (needsHeaderRewrite) {
        Crypto::Keys keys;
        Crypto::AesXtr encryptor(keys.headerKey, true);
        encryptor.encrypt(m_buffer.data(), &header, sizeof(header), 0, 0x200);
    }

    // A plaintext package header cannot be validated against its content ID until
    // it has been normalized back to the encrypted on-storage representation.
    if (inst::config::verifyNcaContentHashes && m_headerPlaintext)
        m_hashContext.update(m_buffer.data(), m_buffer.size());

    const auto begin = std::chrono::steady_clock::now();
    m_contentStorage->WritePlaceholder(m_placeholderId, 0,
                                       m_buffer.data(), m_buffer.size());
    const auto end = std::chrono::steady_clock::now();
    astranas::install_performance::add_ncm_write(
        m_buffer.size(),
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
    m_headerFlushed = true;
}

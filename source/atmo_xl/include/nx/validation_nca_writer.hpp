#pragma once

#include <cstring>

#include "nx/nca_writer.h"
#include "util/config.hpp"
#include "util/crypto.hpp"
#include "util/error.hpp"

// AstraNAS compatibility wrapper around AtmoXL's streaming NCA writer.
// Standard NCAs only require the 0xC00 encrypted NCA header; NCZ keeps its
// 0x4000 uncompressed prefix. The base writer waits for 0x4000 bytes before
// flushing, so a valid small uncompressed NCA needs a safe close-time path.
// 始终执行结构与声明大小检查。全量 Content-ID/CNMT SHA-256 是独立的强校验模式；
// v1.1.0 默认关闭，以对齐 AtmoXL 的轻量 NCA body 写入热路径。
class ValidationAwareNcaWriter final : public NcaWriter {
public:
    using NcaWriter::NcaWriter;

    ~ValidationAwareNcaWriter() override {
        try { close(); } catch (...) {}
    }

    bool close() {
        if (m_closed) return true;

        if (!m_headerFlushed) {
            flush_short_header();
        }

        if (!m_writer && !m_bodyProbe.empty()) {
            m_writer = std::make_shared<NcaBodyWriter>(m_ncaId, NCA_HEADER_SIZE, m_ncaSize,
                                                      m_contentStorage, &m_hashContext,
                                                      m_placeholderId);
            m_writer->write(m_bodyProbe.data(), m_bodyProbe.size());
            m_bodyProbe.clear();
        }

        if (m_writer) m_writer->close();
        else if (m_ncaSize != m_buffer.size())
            THROW_FORMAT("NCA body is missing");

        if (inst::config::verifyNcaContentHashes && !m_hashFinalized) {
            m_actualHash = m_hashContext.final();
            m_hashFinalized = true;
        }
        if (inst::config::verifyNcaContentHashes) {
            if (std::memcmp(m_actualHash.data(), &m_ncaId, sizeof(m_ncaId)) != 0)
                THROW_FORMAT("NCA SHA-256 does not match its content ID");
            if (m_hasExpectedHash && m_actualHash != m_expectedHash)
                THROW_FORMAT("NCA SHA-256 does not match the CNMT content hash");
        }

        m_writer.reset();
        m_contentStorage.reset();
        m_closed = true;
        return true;
    }

private:
    void flush_short_header() {
        if (m_buffer.size() < sizeof(tin::install::NcaHeader))
            THROW_FORMAT("truncated NCA header");
        if (m_buffer.size() >= NCA_HEADER_SIZE) {
            // A complete normal/NCZ prefix should already have been flushed by
            // NcaWriter::write(). Reaching this path means the stream state is inconsistent.
            THROW_FORMAT("NCA header flush state is inconsistent");
        }

        tin::install::NcaHeader header{};
        std::memcpy(&header, m_buffer.data(), sizeof(header));
        Crypto::Keys keys;
        Crypto::AesXtr decryptor(keys.headerKey, false);
        Crypto::AesXtr encryptor(keys.headerKey, true);
        decryptor.decrypt(&header, &header, sizeof(header), 0, 0x200);
        if (header.magic != MAGIC_NCA3 || header.nca_size < sizeof(tin::install::NcaHeader))
            THROW_FORMAT("invalid NCA header or declared size");
        if (header.nca_size != m_buffer.size())
            THROW_FORMAT("short NCA size does not match its declared size");

        m_ncaSize = header.nca_size;
        if (inst::config::verifyNcaContentHashes)
            m_hashContext.update(m_buffer.data(), m_buffer.size());
        if (!isOpen()) THROW_FORMAT("NCA placeholder storage is closed");
        m_contentStorage->CreatePlaceholder(m_ncaId, m_placeholderId,
                                            static_cast<std::size_t>(m_ncaSize));

        if (header.distribution == 1) header.distribution = 0;
        encryptor.encrypt(m_buffer.data(), &header, sizeof(header), 0, 0x200);
        m_contentStorage->WritePlaceholder(m_placeholderId, 0,
                                           m_buffer.data(), m_buffer.size());
        m_headerFlushed = true;
    }
};

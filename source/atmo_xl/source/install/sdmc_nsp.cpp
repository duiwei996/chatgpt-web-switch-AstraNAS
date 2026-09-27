#include "install/sdmc_nsp.hpp"
#include "error.hpp"
#include "debug.h"
#include "nx/nca_writer.h"
#include "ui/instPage.hpp"
#include "util/lang.hpp"
#include "bridge/install_hooks.hpp"
#include "title_backend/buffered_stream.hpp"
#include "install_performance.hpp"
#include <algorithm>
#include <chrono>
#include <exception>

namespace tin::install::nsp
{
    SDMCNSP::SDMCNSP(std::shared_ptr<astranas::title_backend::PackageSource> source)
        : m_source(std::move(source))
    {
        if (!m_source) THROW_FORMAT("NSP package source is unavailable");
    }

    void SDMCNSP::StreamToPlaceholder(std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
                                      NcmContentId ncaId, const u8* expectedHash)
    {
        StreamToPlaceholderImpl(contentStorage, ncaId, nullptr, expectedHash);
    }

    void SDMCNSP::StreamToReplacementPlaceholder(
        std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
        NcmContentId ncaId, NcmPlaceHolderId placeholderId, const u8* expectedHash)
    {
        StreamToPlaceholderImpl(contentStorage, ncaId, &placeholderId, expectedHash);
    }

    void SDMCNSP::StreamToPlaceholderImpl(
        std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
        NcmContentId ncaId, const NcmPlaceHolderId* placeholderId, const u8* expectedHash)
    {
        const PFS0FileEntry* fileEntry = this->GetFileEntryByNcaId(ncaId);
        std::string ncaFileName = this->GetFileEntryName(fileEntry);

        LOG_DEBUG("Retrieving %s\n", ncaFileName.c_str());
        size_t ncaSize = fileEntry->fileSize;

        // Keep one authoritative NCA writer state machine. The base writer now
        // handles short NCAs, NCZ, replacement placeholders and strong hashes,
        // so a second close/header state machine only risks diverging from AtmoXL.
        std::unique_ptr<NcaWriter> writer = placeholderId
            ? std::make_unique<NcaWriter>(ncaId, *placeholderId, contentStorage, expectedHash)
            : std::make_unique<NcaWriter>(ncaId, contentStorage, expectedHash);

        u64 fileStart = GetDataOffset() + fileEntry->dataOffset;

        inst::ui::instPage::setInstInfoText("inst.info_page.top_info0"_lang + ncaFileName + "...");
        inst::ui::instPage::setInstBarPerc(0);
        if (!astranas::atmo_xl_bridge::report_install_progress(ncaFileName.c_str(), 0, ncaSize))
            THROW_FORMAT("安装已取消");

        std::string streamError;
        bool streamed = false;
        try {
            streamed = astranas::title_backend::stream_buffered_read_ahead(
                *m_source, fileStart, ncaSize,
            [&](const unsigned char* data, std::size_t size, std::uint64_t logicalOffset) {
                std::size_t position = 0;
                while (position < size) {
                    const u64 before = logicalOffset + position;
                    if (!astranas::atmo_xl_bridge::report_install_progress(ncaFileName.c_str(), before, ncaSize))
                        THROW_FORMAT("安装已取消");
                    const std::size_t slice = std::min(
                        astranas::title_backend::kInstallWriteSliceSize, size - position);
                    writer->write(data + position, slice);
                    position += slice;
                }
                const u64 completed = logicalOffset + position;
                const float progress = static_cast<float>(completed) / static_cast<float>(ncaSize);
                if (completed == ncaSize || completed % (astranas::title_backend::kInstallStreamChunkSize * 2) == 0) {
                    LOG_DEBUG("> Progress: %lu/%lu MB (%d%s)\r", (completed / 1000000), (ncaSize / 1000000), (int)(progress * 100.0), "%");
                    inst::ui::instPage::setInstBarPerc(static_cast<double>(progress * 100.0));
                }
                if (!astranas::atmo_xl_bridge::report_install_progress(ncaFileName.c_str(), completed, ncaSize))
                    THROW_FORMAT("安装已取消");
            }, streamError,
                [&](std::uint64_t consumed, std::uint64_t total) {
                    return astranas::atmo_xl_bridge::report_install_progress(
                        ncaFileName.c_str(), consumed, total);
                });
        } catch (const std::exception& ex) {
            THROW_FORMAT("NSP NCA 流处理失败 [%s @0x%lx size=0x%lx]: %s",
                         ncaFileName.c_str(), fileStart, static_cast<u64>(ncaSize), ex.what());
        }
        if (!streamed)
            THROW_FORMAT("NSP 安装源读取失败 [%s @0x%lx size=0x%lx]: %s",
                         ncaFileName.c_str(), fileStart, static_cast<u64>(ncaSize), streamError.c_str());
        inst::ui::instPage::setInstBarPerc(100);
        writer->close();
    }

    u64 SDMCNSP::GetSourceSize() const
    {
        return m_source ? m_source->size() : 0;
    }

    void SDMCNSP::BufferData(void* buf, off_t offset, size_t size)
    {
        if (offset < 0) THROW_FORMAT("invalid negative NSP source offset");
        std::size_t actual = 0;
        std::string error;
        const auto begin = std::chrono::steady_clock::now();
        const bool ok = m_source->read_at(static_cast<std::uint64_t>(offset), buf, size, actual, error);
        const auto end = std::chrono::steady_clock::now();
        astranas::install_performance::add_source_read(
            actual,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
        if (!ok || actual != size)
            THROW_FORMAT("NSP 安装源读取失败 [offset=0x%lx requested=0x%lx actual=0x%lx]: %s",
                         static_cast<u64>(offset), static_cast<u64>(size), static_cast<u64>(actual),
                         error.empty() ? "输入数据被截断" : error.c_str());
    }
}

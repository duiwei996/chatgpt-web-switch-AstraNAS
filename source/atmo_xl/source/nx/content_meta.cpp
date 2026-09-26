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

#include "nx/content_meta.hpp"

#include <string.h>
#include <limits>
#include "util/title_util.hpp"
#include "util/debug.h"
#include "util/error.hpp"

namespace nx::ncm
{
    namespace
    {
        bool checked_add_mul(size_t base, size_t count, size_t elementSize, size_t& result)
        {
            if (count != 0 && elementSize > (std::numeric_limits<size_t>::max() - base) / count)
                return false;
            result = base + count * elementSize;
            return true;
        }
    }

    ContentMeta::ContentMeta()
    {
        m_bytes.Resize(sizeof(PackagedContentMetaHeader));
    }

    ContentMeta::ContentMeta(u8* data, size_t size) :
        m_bytes(size)
    {
        if (!data || size < sizeof(PackagedContentMetaHeader))
            THROW_FORMAT("Content meta data size is too small!");

        m_bytes.Resize(size);
        memcpy(m_bytes.GetData(), data, size);
        this->ValidateLayout();
    }

    size_t ContentMeta::GetPackagedContentOffset()
    {
        const auto header = this->GetPackagedContentMetaHeader();
        size_t offset = 0;
        if (!checked_add_mul(sizeof(PackagedContentMetaHeader), 1,
                             header.extended_header_size, offset))
            THROW_FORMAT("Content meta extended header size overflow");
        return offset;
    }

    size_t ContentMeta::GetContentMetaInfoOffset()
    {
        const auto header = this->GetPackagedContentMetaHeader();
        size_t offset = 0;
        if (!checked_add_mul(this->GetPackagedContentOffset(), header.content_count,
                             sizeof(PackagedContentInfo), offset))
            THROW_FORMAT("Content meta content table size overflow");
        return offset;
    }

    size_t ContentMeta::GetExtendedDataOffset()
    {
        const auto header = this->GetPackagedContentMetaHeader();
        size_t offset = 0;
        if (!checked_add_mul(this->GetContentMetaInfoOffset(), header.content_meta_count,
                             sizeof(NcmContentMetaInfo), offset))
            THROW_FORMAT("Content meta info table size overflow");
        return offset;
    }

    size_t ContentMeta::GetExtendedDataSize()
    {
        const auto header = this->GetPackagedContentMetaHeader();
        const u8* extended = m_bytes.GetData() + sizeof(PackagedContentMetaHeader);
        switch (static_cast<NcmContentMetaType>(header.type))
        {
            case NcmContentMetaType_Patch:
                if (header.extended_header_size < sizeof(NcmPatchMetaExtendedHeader))
                    THROW_FORMAT("Patch content meta extended header is truncated");
                {
                    NcmPatchMetaExtendedHeader value{};
                    memcpy(&value, extended, sizeof(value));
                    return value.extended_data_size;
                }
            case NcmContentMetaType_DataPatch:
                if (header.extended_header_size < sizeof(NcmDataPatchMetaExtendedHeader))
                    THROW_FORMAT("Data patch content meta extended header is truncated");
                {
                    NcmDataPatchMetaExtendedHeader value{};
                    memcpy(&value, extended, sizeof(value));
                    return value.extended_data_size;
                }
            case NcmContentMetaType_SystemUpdate:
                if (header.extended_header_size < sizeof(NcmSystemUpdateMetaExtendedHeader))
                    THROW_FORMAT("System update content meta extended header is truncated");
                {
                    NcmSystemUpdateMetaExtendedHeader value{};
                    memcpy(&value, extended, sizeof(value));
                    return value.extended_data_size;
                }
            default:
                return 0;
        }
    }

    void ContentMeta::ValidateLayout()
    {
        constexpr size_t kMaxContentMetaSize = 16 * 1024 * 1024;
        if (m_bytes.GetSize() < sizeof(PackagedContentMetaHeader))
            THROW_FORMAT("Content meta data size is too small");
        if (m_bytes.GetSize() > kMaxContentMetaSize)
            THROW_FORMAT("Content meta data is too large");

        const size_t extendedDataOffset = this->GetExtendedDataOffset();
        if (extendedDataOffset > m_bytes.GetSize())
            THROW_FORMAT("Content meta tables exceed the CNMT file");
        const auto header = this->GetPackagedContentMetaHeader();
        switch (static_cast<NcmContentMetaType>(header.type))
        {
            case NcmContentMetaType_Application:
                if (header.extended_header_size < sizeof(NcmApplicationMetaExtendedHeader))
                    THROW_FORMAT("Application content meta extended header is truncated");
                break;
            case NcmContentMetaType_AddOnContent:
                if (header.extended_header_size < sizeof(NcmLegacyAddOnContentMetaExtendedHeader))
                    THROW_FORMAT("Add-on content meta extended header is truncated");
                break;
            default:
                break;
        }
        const size_t extendedDataSize = this->GetExtendedDataSize();
        if (extendedDataSize > m_bytes.GetSize() - extendedDataOffset)
            THROW_FORMAT("Content meta extended data exceeds the CNMT file");
    }

    PackagedContentMetaHeader ContentMeta::GetPackagedContentMetaHeader()
    {
        return m_bytes.Read<PackagedContentMetaHeader>(0);
    }

    NcmContentMetaKey ContentMeta::GetContentMetaKey()
    {
        NcmContentMetaKey metaRecord;
        PackagedContentMetaHeader contentMetaHeader = this->GetPackagedContentMetaHeader();

        memset(&metaRecord, 0, sizeof(NcmContentMetaKey));
        metaRecord.id = contentMetaHeader.title_id;
        metaRecord.version = contentMetaHeader.version;
        metaRecord.type = static_cast<NcmContentMetaType>(contentMetaHeader.type);
        metaRecord.install_type = contentMetaHeader.install_type;

        return metaRecord;
    }

    u64 ContentMeta::GetApplicationId()
    {
        const PackagedContentMetaHeader header = this->GetPackagedContentMetaHeader();
        const u8* extended = m_bytes.GetData() + sizeof(PackagedContentMetaHeader);
        switch (static_cast<NcmContentMetaType>(header.type))
        {
            case NcmContentMetaType_Application:
                return header.title_id;
            case NcmContentMetaType_Patch:
            {
                if (header.extended_header_size < sizeof(NcmPatchMetaExtendedHeader))
                    THROW_FORMAT("Patch content meta extended header is truncated");
                NcmPatchMetaExtendedHeader value{};
                memcpy(&value, extended, sizeof(value));
                return value.application_id;
            }
            case NcmContentMetaType_AddOnContent:
            {
                if (header.extended_header_size < sizeof(NcmLegacyAddOnContentMetaExtendedHeader))
                    THROW_FORMAT("Add-on content meta extended header is truncated");
                NcmLegacyAddOnContentMetaExtendedHeader value{};
                memcpy(&value, extended, sizeof(value));
                return value.application_id;
            }
            case NcmContentMetaType_DataPatch:
            {
                if (header.extended_header_size < sizeof(NcmDataPatchMetaExtendedHeader))
                    THROW_FORMAT("Data patch content meta extended header is truncated");
                NcmDataPatchMetaExtendedHeader value{};
                memcpy(&value, extended, sizeof(value));
                return value.application_id;
            }
            default:
                THROW_FORMAT("Content metadata does not belong to an installable application");
        }
    }

    std::vector<PackagedContentInfo> ContentMeta::GetPackagedContentInfos()
    {
        PackagedContentMetaHeader contentMetaHeader = this->GetPackagedContentMetaHeader();

        std::vector<PackagedContentInfo> contentInfos;
        contentInfos.reserve(contentMetaHeader.content_count);
        const PackagedContentInfo* packagedContentInfos = reinterpret_cast<const PackagedContentInfo*>(
            m_bytes.GetData() + this->GetPackagedContentOffset());

        for (unsigned int i = 0; i < contentMetaHeader.content_count; i++)
        {
            PackagedContentInfo packagedContentInfo = packagedContentInfos[i];

            // Don't install delta fragments. Even patches don't seem to install them.
            if (static_cast<u8>(packagedContentInfo.content_info.content_type) <= 5)
            {
                contentInfos.push_back(packagedContentInfo);
            }
        }

        return contentInfos;
    }

    std::vector<NcmContentInfo> ContentMeta::GetContentInfos()
    {
        const auto packagedInfos = this->GetPackagedContentInfos();
        std::vector<NcmContentInfo> contentInfos;
        contentInfos.reserve(packagedInfos.size());
        for (const auto& packaged : packagedInfos)
            contentInfos.push_back(packaged.content_info);
        return contentInfos;
    }

    void ContentMeta::GetInstallContentMeta(tin::data::ByteBuffer& installContentMetaBuffer, const NcmContentInfo& cnmtNcmContentInfo, bool ignoreReqFirmVersion)
    {
        PackagedContentMetaHeader packagedContentMetaHeader = this->GetPackagedContentMetaHeader();
        std::vector<NcmContentInfo> contentInfos = this->GetContentInfos();
        if (contentInfos.size() >= std::numeric_limits<u16>::max())
            THROW_FORMAT("Content meta has too many installable content records");

        // Setup the content meta header
        NcmContentMetaHeader contentMetaHeader{};
        contentMetaHeader.extended_header_size = packagedContentMetaHeader.extended_header_size;
        contentMetaHeader.content_count = contentInfos.size() + 1; // Add one for the cnmt content record
        contentMetaHeader.content_meta_count = packagedContentMetaHeader.content_meta_count;
        contentMetaHeader.attributes = packagedContentMetaHeader.attributes; // Sparse Titles use 0x04 not 0x0
        contentMetaHeader.storage_id = 0;

        installContentMetaBuffer.Append<NcmContentMetaHeader>(contentMetaHeader);

        // Setup the meta extended header
        LOG_DEBUG("Install content meta pre size: 0x%lx\n", installContentMetaBuffer.GetSize());
        installContentMetaBuffer.Resize(installContentMetaBuffer.GetSize() + contentMetaHeader.extended_header_size);
        LOG_DEBUG("Install content meta post size: 0x%lx\n", installContentMetaBuffer.GetSize());
        auto* extendedHeaderSourceBytes = m_bytes.GetData() + sizeof(PackagedContentMetaHeader);
        u8* installExtendedHeaderStart = installContentMetaBuffer.GetData() + sizeof(NcmContentMetaHeader);
        memcpy(installExtendedHeaderStart, extendedHeaderSourceBytes, contentMetaHeader.extended_header_size);

        // Optionally disable the required system version field
        if (ignoreReqFirmVersion && (packagedContentMetaHeader.type == NcmContentMetaType_Application || packagedContentMetaHeader.type == NcmContentMetaType_Patch))
        {
            installContentMetaBuffer.Write<u32>(0, sizeof(NcmContentMetaHeader) + 8);
        }

        // Setup cnmt content record
        installContentMetaBuffer.Append<NcmContentInfo>(cnmtNcmContentInfo);

        // Setup the content records
        for (auto& contentInfo : contentInfos)
        {
            installContentMetaBuffer.Append<NcmContentInfo>(contentInfo);
        }

        // Preserve content-meta references and type-specific extended data.
        // Their source offsets are based on the original packaged content
        // count, even when delta fragments are omitted from installation.
        const size_t metaInfoBytes = static_cast<size_t>(packagedContentMetaHeader.content_meta_count) *
                                     sizeof(NcmContentMetaInfo);
        const size_t outputMetaOffset = installContentMetaBuffer.GetSize();
        installContentMetaBuffer.Resize(outputMetaOffset + metaInfoBytes);
        if (metaInfoBytes != 0)
        {
            memcpy(installContentMetaBuffer.GetData() + outputMetaOffset,
                   m_bytes.GetData() + this->GetContentMetaInfoOffset(), metaInfoBytes);
        }

        const size_t extendedDataSize = this->GetExtendedDataSize();
        const size_t outputExtendedOffset = installContentMetaBuffer.GetSize();
        installContentMetaBuffer.Resize(outputExtendedOffset + extendedDataSize);
        if (extendedDataSize != 0)
        {
            memcpy(installContentMetaBuffer.GetData() + outputExtendedOffset,
                   m_bytes.GetData() + this->GetExtendedDataOffset(), extendedDataSize);
        }
    }
}


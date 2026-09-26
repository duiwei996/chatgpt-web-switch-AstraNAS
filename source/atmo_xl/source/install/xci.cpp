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

#include "install/xci.hpp"
#include "util/title_util.hpp"
#include "error.hpp"
#include "debug.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <array>
#include <sstream>
#include <utility>
#include "sha256.hpp"

namespace tin::install::xci
{
    namespace
    {
        std::string ascii_lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        constexpr u32 kMaxContainerFiles = 16384;
        constexpr u32 kMaxStringTableSize = 4U * 1024U * 1024U;

        size_t checked_remaining_header_size(const HFS0BaseHeader& header)
        {
            if (header.magic != MAGIC_HFS0 || header.numFiles == 0 ||
                header.numFiles > kMaxContainerFiles || header.stringTableSize == 0 ||
                header.stringTableSize > kMaxStringTableSize)
                THROW_FORMAT("Invalid or unreasonable HFS0 header");
            if (header.numFiles > (std::numeric_limits<size_t>::max() - header.stringTableSize) /
                                  sizeof(HFS0FileEntry))
                THROW_FORMAT("HFS0 header size overflow");
            return static_cast<size_t>(header.numFiles) * sizeof(HFS0FileEntry) +
                   header.stringTableSize;
        }

        const char* checked_file_name(const HFS0BaseHeader* header,
                                      const HFS0FileEntry* entry)
        {
            if (!header || !entry || entry->stringTableOffset >= header->stringTableSize)
                THROW_FORMAT("HFS0 filename offset is out of bounds");
            const char* name = hfs0GetStringTable(header) + entry->stringTableOffset;
            const size_t remaining = header->stringTableSize - entry->stringTableOffset;
            const void* terminator = std::memchr(name, '\0', remaining);
            if (!terminator)
                THROW_FORMAT("HFS0 filename is not null terminated");
            if (static_cast<const char*>(terminator) - name > 255)
                THROW_FORMAT("HFS0 filename is too long");
            return name;
        }
    }

    XCI::XCI()
    {
    }

    void XCI::RetrieveHeader()
    {
        LOG_DEBUG("Retrieving HFS0 header...\n");

        // Retrieve hfs0 offset
        u64 hfs0Offset = 0xf000;

        // Retrieve main hfs0 header
        std::vector<u8> m_headerBytes;
        m_headerBytes.resize(sizeof(HFS0BaseHeader), 0);
        this->BufferData(m_headerBytes.data(), hfs0Offset, sizeof(HFS0BaseHeader));

        LOG_DEBUG("Base header: \n");
        printBytes(m_headerBytes.data(), sizeof(HFS0BaseHeader), true);

        // Retrieve full header
        HFS0BaseHeader *header = reinterpret_cast<HFS0BaseHeader*>(m_headerBytes.data());
        if (header->magic != MAGIC_HFS0)
            THROW_FORMAT("hfs0 magic doesn't match at 0x%lx\n", hfs0Offset);

        size_t remainingHeaderSize = checked_remaining_header_size(*header);
        m_headerBytes.resize(sizeof(HFS0BaseHeader) + remainingHeaderSize, 0);
        this->BufferData(m_headerBytes.data() + sizeof(HFS0BaseHeader), hfs0Offset + sizeof(HFS0BaseHeader), remainingHeaderSize);

        LOG_DEBUG("Base header: \n");
        printBytes(m_headerBytes.data(), sizeof(HFS0BaseHeader) + remainingHeaderSize, true);

        // Find Secure partition
        header = reinterpret_cast<HFS0BaseHeader*>(m_headerBytes.data());
        for (unsigned int i = 0; i < header->numFiles; i++)
        {
            const HFS0FileEntry *entry = hfs0GetFileEntry(header, i);
            std::string entryName(checked_file_name(header, entry));

            if (entryName != "secure")
                continue;

            const u64 dataBase = hfs0Offset + sizeof(HFS0BaseHeader) + remainingHeaderSize;
            if (entry->dataOffset > std::numeric_limits<u64>::max() - dataBase)
                THROW_FORMAT("Secure HFS0 offset overflow");
            m_secureHeaderOffset = dataBase + entry->dataOffset;
            m_secureHeaderBytes.resize(sizeof(HFS0BaseHeader), 0);
            this->BufferData(m_secureHeaderBytes.data(), m_secureHeaderOffset, sizeof(HFS0BaseHeader));

            LOG_DEBUG("Secure header: \n");
            printBytes(m_secureHeaderBytes.data(), sizeof(HFS0BaseHeader), true);

            if (this->GetSecureHeader()->magic != MAGIC_HFS0)
                THROW_FORMAT("hfs0 magic doesn't match at 0x%lx\n", m_secureHeaderOffset);

            // Retrieve full header
            remainingHeaderSize = checked_remaining_header_size(*this->GetSecureHeader());
            m_secureHeaderBytes.resize(sizeof(HFS0BaseHeader) + remainingHeaderSize, 0);
            this->BufferData(m_secureHeaderBytes.data() + sizeof(HFS0BaseHeader), m_secureHeaderOffset + sizeof(HFS0BaseHeader), remainingHeaderSize);

            LOG_DEBUG("Base header: \n");
            printBytes(m_secureHeaderBytes.data(), sizeof(HFS0BaseHeader) + remainingHeaderSize, true);
            return;
        }
        THROW_FORMAT("couldn't optain secure hfs0 header\n");
    }

    const HFS0BaseHeader* XCI::GetSecureHeader()
    {
        if (m_secureHeaderBytes.empty())
            THROW_FORMAT("Cannot retrieve header as header bytes are empty. Have you retrieved it yet?\n");

        return reinterpret_cast<HFS0BaseHeader*>(m_secureHeaderBytes.data());
    }

    u64 XCI::GetDataOffset()
    {
        if (m_secureHeaderBytes.empty())
            THROW_FORMAT("Cannot get data offset as header is empty. Have you retrieved it yet?\n");

        return m_secureHeaderOffset + m_secureHeaderBytes.size();
    }

    const HFS0FileEntry* XCI::GetFileEntry(unsigned int index)
    {
        if (index >= this->GetSecureHeader()->numFiles)
            THROW_FORMAT("File entry index is out of bounds\n");

        return hfs0GetFileEntry(this->GetSecureHeader(), index);
    }

    const HFS0FileEntry* XCI::GetFileEntryByName(std::string name)
    {
        for (unsigned int i = 0; i < this->GetSecureHeader()->numFiles; i++)
        {
            const HFS0FileEntry* fileEntry = this->GetFileEntry(i);
            std::string foundName(this->GetFileEntryName(fileEntry));

            if (ascii_lower(foundName) == ascii_lower(name))
                return fileEntry;
        }

        return nullptr;
    }

    const HFS0FileEntry* XCI::GetFileEntryByNcaId(const NcmContentId& ncaId)
    {
        const HFS0FileEntry* fileEntry = nullptr;
        std::string ncaIdStr = tin::util::GetNcaIdString(ncaId);

        if ((fileEntry = this->GetFileEntryByName(ncaIdStr + ".nca")) == nullptr)
        {
            if ((fileEntry = this->GetFileEntryByName(ncaIdStr + ".cnmt.nca")) == nullptr)
            {
                    if ((fileEntry = this->GetFileEntryByName(ncaIdStr + ".ncz")) == nullptr)
                    {
                         if ((fileEntry = this->GetFileEntryByName(ncaIdStr + ".cnmt.ncz")) == nullptr)
                         {
                              return nullptr;
                         }
                    }
            }
        }

        return fileEntry;
    }

    std::vector<const HFS0FileEntry*> XCI::GetFileEntriesByExtension(std::string extension)
    {
        std::vector<const HFS0FileEntry*> entryList;

        for (unsigned int i = 0; i < this->GetSecureHeader()->numFiles; i++)
        {
            const HFS0FileEntry* fileEntry = this->GetFileEntry(i);
            std::string name(this->GetFileEntryName(fileEntry));
            const auto dot = name.find('.');
            if (dot == std::string::npos) continue;
            auto foundExtension = name.substr(dot + 1);

            if (ascii_lower(foundExtension) == ascii_lower(extension))
                entryList.push_back(fileEntry);
        }

        return entryList;
    }

    const char* XCI::GetFileEntryName(const HFS0FileEntry* fileEntry)
    {
        if (!fileEntry) THROW_FORMAT("Cannot get the name of a null HFS0 entry");
        const auto* header = this->GetSecureHeader();
        return checked_file_name(header, fileEntry);
    }

    tin::install::SourceEntryAudit XCI::AuditFileEntry(
        const HFS0FileEntry* fileEntry,
        const NcmContentId& expectedContentId,
        bool compareContentId)
    {
        if (!fileEntry) THROW_FORMAT("Cannot audit a null HFS0 entry");
        const u64 packageSize = GetSourceSize();
        const u64 dataBase = GetDataOffset();
        bool tableBoundsOk = dataBase <= packageSize;
        bool tableOverlap = false;
        std::vector<std::pair<u64, u64>> ranges;
        ranges.reserve(GetSecureHeader()->numFiles);

        for (unsigned int i = 0; i < GetSecureHeader()->numFiles; ++i) {
            const auto* entry = GetFileEntry(i);
            if (entry->dataOffset > std::numeric_limits<u64>::max() - dataBase) {
                tableBoundsOk = false;
                continue;
            }
            const u64 begin = dataBase + entry->dataOffset;
            if (begin > packageSize || entry->fileSize > packageSize - begin) {
                tableBoundsOk = false;
                continue;
            }
            ranges.emplace_back(begin, begin + entry->fileSize);
        }
        std::sort(ranges.begin(), ranges.end());
        u64 previousEnd = 0;
        bool havePrevious = false;
        for (const auto& range : ranges) {
            if (havePrevious && range.first < previousEnd) tableOverlap = true;
            previousEnd = std::max(previousEnd, range.second);
            havePrevious = true;
        }

        bool targetBoundsOk = false;
        u64 absoluteOffset = 0;
        u64 entryEnd = 0;
        if (fileEntry->dataOffset <= std::numeric_limits<u64>::max() - dataBase) {
            absoluteOffset = dataBase + fileEntry->dataOffset;
            if (absoluteOffset <= packageSize && fileEntry->fileSize <= packageSize - absoluteOffset) {
                entryEnd = absoluteOffset + fileEntry->fileSize;
                targetBoundsOk = true;
            }
        }

        std::string entryHash = "skipped_out_of_bounds";
        tin::install::SourceContentIdStatus contentIdStatus =
            compareContentId
                ? tin::install::SourceContentIdStatus::Unavailable
                : tin::install::SourceContentIdStatus::NotApplicable;
        if (targetBoundsOk) {
            AstraSha256Context sha;
            std::array<u8, 64 * 1024> buffer{};
            u64 position = 0;
            while (position < fileEntry->fileSize) {
                const size_t chunk = static_cast<size_t>(
                    std::min<u64>(buffer.size(), fileEntry->fileSize - position));
                BufferData(buffer.data(), static_cast<off_t>(absoluteOffset + position), chunk);
                sha.update(buffer.data(), chunk);
                position += chunk;
            }
            entryHash = sha256_digest_hex(sha.final());
            if (compareContentId) {
                const std::string expected = tin::util::GetNcaIdString(expectedContentId);
                contentIdStatus =
                    entryHash.size() >= expected.size() &&
                    entryHash.compare(0, expected.size(), expected) == 0
                        ? tin::install::SourceContentIdStatus::Match
                        : tin::install::SourceContentIdStatus::Mismatch;
            }
        }

        const std::string expectedId = tin::util::GetNcaIdString(expectedContentId);
        std::ostringstream out;
        // Put the verdict first. Even if a future UI or transport imposes a
        // display limit, the integrity/bounds result must survive ahead of the
        // lower-priority offset details.
        out << "content_id_match=" << tin::install::SourceContentIdStatusName(contentIdStatus)
            << " target_bounds=" << (targetBoundsOk ? "pass" : "fail")
            << " table_bounds=" << (tableBoundsOk ? "pass" : "fail")
            << " table_overlap=" << (tableOverlap ? "yes" : "no")
            << " entry_sha256=" << entryHash
            << " expected_content_id=" << expectedId
            << " container=HFS0"
            << " files=" << GetSecureHeader()->numFiles
            << " data_base=0x" << std::hex << dataBase
            << " relative_offset=0x" << fileEntry->dataOffset
            << " absolute_offset=0x" << absoluteOffset
            << " entry_size=0x" << fileEntry->fileSize
            << " entry_end=0x" << entryEnd
            << " package_size=0x" << packageSize;

        tin::install::SourceEntryAudit result{};
        result.contentIdStatus = contentIdStatus;
        result.targetBoundsOk = targetBoundsOk;
        result.tableBoundsOk = tableBoundsOk;
        result.tableOverlap = tableOverlap;
        result.entrySha256 = entryHash;
        result.expectedContentId = expectedId;
        result.details = out.str();
        return result;
    }
}

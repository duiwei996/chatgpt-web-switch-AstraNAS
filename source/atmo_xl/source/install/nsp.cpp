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

#include "install/nsp.hpp"

#include <threads.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include "data/buffered_placeholder_writer.hpp"
#include "util/title_util.hpp"
#include "util/error.hpp"
#include "util/debug.h"

namespace tin::install::nsp
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

        constexpr u32 kPfs0Magic = 0x30534650;
        constexpr u32 kMaxContainerFiles = 16384;
        constexpr u32 kMaxStringTableSize = 4U * 1024U * 1024U;
    }

    NSP::NSP() {}

    void NSP::RetrieveHeader()
    {
        LOG_DEBUG("Retrieving remote NSP header...\n");

        // Retrieve the base header
        m_headerBytes.resize(sizeof(PFS0BaseHeader), 0);
        this->BufferData(m_headerBytes.data(), 0x0, sizeof(PFS0BaseHeader));

        const PFS0BaseHeader baseHeader = *reinterpret_cast<const PFS0BaseHeader*>(m_headerBytes.data());
        if (baseHeader.magic != kPfs0Magic || baseHeader.numFiles == 0 ||
            baseHeader.numFiles > kMaxContainerFiles ||
            baseHeader.stringTableSize == 0 || baseHeader.stringTableSize > kMaxStringTableSize)
            THROW_FORMAT("Invalid or unreasonable PFS0 header");
        if (baseHeader.numFiles > (std::numeric_limits<size_t>::max() - baseHeader.stringTableSize) /
                                  sizeof(PFS0FileEntry))
            THROW_FORMAT("PFS0 header size overflow");

        LOG_DEBUG("Base header: \n");
        printBytes(m_headerBytes.data(), sizeof(PFS0BaseHeader), true);

        // Retrieve the full header
        size_t remainingHeaderSize = static_cast<size_t>(baseHeader.numFiles) * sizeof(PFS0FileEntry) +
                                     baseHeader.stringTableSize;
        m_headerBytes.resize(sizeof(PFS0BaseHeader) + remainingHeaderSize, 0);
        this->BufferData(m_headerBytes.data() + sizeof(PFS0BaseHeader), sizeof(PFS0BaseHeader), remainingHeaderSize);

        LOG_DEBUG("Full header: \n");
        printBytes(m_headerBytes.data(), m_headerBytes.size(), true);
    }

    const PFS0FileEntry* NSP::GetFileEntry(unsigned int index)
    {
        if (index >= this->GetBaseHeader()->numFiles)
            THROW_FORMAT("File entry index is out of bounds\n");

        size_t fileEntryOffset = sizeof(PFS0BaseHeader) + index * sizeof(PFS0FileEntry);

        if (m_headerBytes.size() < fileEntryOffset + sizeof(PFS0FileEntry))
            THROW_FORMAT("Header bytes is too small to get file entry!");

        return reinterpret_cast<PFS0FileEntry*>(m_headerBytes.data() + fileEntryOffset);
    }

    std::vector<const PFS0FileEntry*> NSP::GetFileEntriesByExtension(std::string extension)
    {
        std::vector<const PFS0FileEntry*> entryList;

        for (unsigned int i = 0; i < this->GetBaseHeader()->numFiles; i++)
        {
            const PFS0FileEntry* fileEntry = this->GetFileEntry(i);
            std::string name(this->GetFileEntryName(fileEntry));
            const auto dot = name.find('.');
            if (dot == std::string::npos) continue;
            auto foundExtension = name.substr(dot + 1);

            if (ascii_lower(foundExtension) == ascii_lower(extension))
                entryList.push_back(fileEntry);
        }

        return entryList;
    }

    const PFS0FileEntry* NSP::GetFileEntryByName(std::string name)
    {
        for (unsigned int i = 0; i < this->GetBaseHeader()->numFiles; i++)
        {
            const PFS0FileEntry* fileEntry = this->GetFileEntry(i);
            std::string foundName(this->GetFileEntryName(fileEntry));

            if (ascii_lower(foundName) == ascii_lower(name))
                return fileEntry;
        }

        return nullptr;
    }

    const PFS0FileEntry* NSP::GetFileEntryByNcaId(const NcmContentId& ncaId)
    {
        const PFS0FileEntry* fileEntry = nullptr;
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

    const char* NSP::GetFileEntryName(const PFS0FileEntry* fileEntry)
    {
        if (!fileEntry) THROW_FORMAT("Cannot get the name of a null PFS0 entry");
        const auto* header = this->GetBaseHeader();
        if (fileEntry->stringTableOffset >= header->stringTableSize)
            THROW_FORMAT("PFS0 filename offset is out of bounds");
        u64 stringTableStart = sizeof(PFS0BaseHeader) + this->GetBaseHeader()->numFiles * sizeof(PFS0FileEntry);
        const char* name = reinterpret_cast<const char*>(m_headerBytes.data() + stringTableStart + fileEntry->stringTableOffset);
        const size_t remaining = header->stringTableSize - fileEntry->stringTableOffset;
        const void* terminator = std::memchr(name, '\0', remaining);
        if (!terminator)
            THROW_FORMAT("PFS0 filename is not null terminated");
        if (static_cast<const char*>(terminator) - name > 255)
            THROW_FORMAT("PFS0 filename is too long");
        return name;
    }

    const PFS0BaseHeader* NSP::GetBaseHeader()
    {
        if (m_headerBytes.empty())
            THROW_FORMAT("Cannot retrieve header as header bytes are empty. Have you retrieved it yet?\n");

        return reinterpret_cast<PFS0BaseHeader*>(m_headerBytes.data());
    }

    u64 NSP::GetDataOffset()
    {
        if (m_headerBytes.empty())
            THROW_FORMAT("Cannot get data offset as header is empty. Have you retrieved it yet?\n");

        return m_headerBytes.size();
    }
}

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

#include "install/install_xci.hpp"
#include <algorithm>
#include <cctype>
#include "util/file_util.hpp"
#include "util/title_util.hpp"
#include "util/debug.h"
#include "util/error.hpp"
#include "util/config.hpp"
#include "util/crypto.hpp"
#include "util/util.hpp"
#include "bridge/install_hooks.hpp"
#include "install/nca.hpp"
#include "install/nca_header_probe.hpp"
#include <limits>

namespace {
    bool has_ncz_suffix(const std::string& name)
    {
        if (name.size() < 4) return false;
        const std::size_t offset = name.size() - 4;
        return name[offset] == '.' &&
               std::tolower(static_cast<unsigned char>(name[offset + 1])) == 'n' &&
               std::tolower(static_cast<unsigned char>(name[offset + 2])) == 'c' &&
               std::tolower(static_cast<unsigned char>(name[offset + 3])) == 'z';
    }
}

namespace tin::install::xci
{
    XCIInstallTask::XCIInstallTask(NcmStorageId destStorageId, bool ignoreReqFirmVersion, const std::shared_ptr<XCI>& xci) :
        Install(destStorageId, ignoreReqFirmVersion), m_xci(xci)
    {
        m_xci->RetrieveHeader();
    }

    std::vector<std::tuple<nx::ncm::ContentMeta, NcmContentInfo>> XCIInstallTask::ReadCNMT()
    {
        std::vector<std::tuple<nx::ncm::ContentMeta, NcmContentInfo>> CNMTList;

        auto cnmtEntries = m_xci->GetFileEntriesByExtension("cnmt.nca");
        auto compressedCnmtEntries = m_xci->GetFileEntriesByExtension("cnmt.ncz");
        cnmtEntries.insert(cnmtEntries.end(), compressedCnmtEntries.begin(), compressedCnmtEntries.end());
        for (const HFS0FileEntry* fileEntry : cnmtEntries) {
            std::string cnmtNcaName(m_xci->GetFileEntryName(fileEntry));
            NcmContentId cnmtContentId = tin::util::GetNcaIdFromString(cnmtNcaName);
            // Defer compressed CNMT header reads until repair/install is actually
            // needed; the registered size is filled after a successful mount.
            const bool compressedCnmt = has_ncz_suffix(cnmtNcaName);
            u64 cnmtNcaSize = compressedCnmt ? 0 : fileEntry->fileSize;

            LOG_DEBUG("CNMT Name: %s\n", cnmtNcaName.c_str());

            NcmContentInfo cnmtContentInfo{};
            cnmtContentInfo.content_id = cnmtContentId;
            if (cnmtNcaSize != 0)
                ncmU64ToContentInfoSize(cnmtNcaSize, &cnmtContentInfo);
            cnmtContentInfo.content_type = NcmContentType_Meta;

            // Keep XCI/XCZ on the same verified CNMT recovery path as NSP/NSZ.
            CNMTList.push_back( { this->InstallAndReadCnmtWithRepair(cnmtContentInfo), cnmtContentInfo } );
        }
        
        return CNMTList;
    }

    void XCIInstallTask::InstallNCA(const NcmContentInfo& contentInfo)
    {
        const NcmContentId& ncaId = contentInfo.content_id;
        const HFS0FileEntry* fileEntry = m_xci->GetFileEntryByNcaId(ncaId);
        if (!fileEntry) THROW_FORMAT("package is missing a referenced NCA");
        std::string ncaFileName = m_xci->GetFileEntryName(fileEntry);
        const bool compressed = has_ncz_suffix(ncaFileName);
        const u64 headerOffset = m_xci->GetDataOffset() + fileEntry->dataOffset;
        const auto header = tin::install::ReadValidatedNcaHeader(
            "XCI", ncaFileName, headerOffset, fileEntry->fileSize, compressed,
            [&](void* out, std::size_t size) {
                m_xci->BufferData(out, static_cast<off_t>(headerOffset), size);
            });
        u64 expectedSize = 0;
        ncmContentInfoSizeToU64(&contentInfo, &expectedSize);
        if (expectedSize != 0 && header.nca_size != expectedSize)
            THROW_FORMAT("NCA size does not match CNMT content record");
        if (!compressed && fileEntry->fileSize != header.nca_size)
            THROW_FORMAT("Uncompressed NCA size does not match its header");
        #ifdef NXLINK_DEBUG
        LOG_DEBUG("Installing %s to storage Id %u\n", ncaFileName.c_str(), m_destStorageId);
        #endif

        if (inst::config::validateNCAs && !m_declinedValidation)
        {
            if (!Crypto::rsa2048PssVerify(&header.magic, 0x200, header.fixed_key_sig, Crypto::NCAHeaderSignature))
            {
                const std::string contentId = tin::util::GetNcaIdString(ncaId);
                if (!astranas::atmo_xl_bridge::request_disable_nca_validation(contentId.c_str()))
                    THROW_FORMAT("NCA header signature verification failed for %s", contentId.c_str());
                m_declinedValidation = true;
                inst::config::validateNCAs = false;
            }
        }

        std::shared_ptr<nx::ncm::ContentStorage> contentStorage(new nx::ncm::ContentStorage(m_destStorageId));
        this->RegisterRequiredRightsId(reinterpret_cast<const u8*>(header.m_rightsId));
        const bool replacing = contentStorage->Has(ncaId);
        if (replacing && !this->IsForceReinstall()) return;
        const NcmPlaceHolderId placeholderId = replacing
            ? this->ReplacementPlaceholderIdForContent(ncaId)
            : this->PlaceholderIdForContent(ncaId);

        try { contentStorage->DeletePlaceholder(placeholderId); }
        catch (...) {}

        LOG_DEBUG("Size: 0x%lx\n", fileEntry->fileSize);
        bool existingDeleted = false;
        try
        {
            if (replacing)
                m_xci->StreamToReplacementPlaceholder(contentStorage, ncaId, placeholderId,
                                                       this->FindExpectedContentHash(ncaId));
            else
                m_xci->StreamToPlaceholder(contentStorage, ncaId,
                                           this->FindExpectedContentHash(ncaId));
            LOG_DEBUG("Registering placeholder...\n");
            if (replacing) {
                contentStorage->Delete(ncaId);
                existingDeleted = true;
            }
            contentStorage->Register(placeholderId, ncaId);
        }
        catch (...)
        {
            if (existingDeleted) {
                bool recovered = false;
                try { contentStorage->Register(placeholderId, ncaId); recovered = true; }
                catch (...) {}
                if (!recovered)
                    THROW_FORMAT("替换内容无法注册，已保留恢复 placeholder");
            } else {
                try { contentStorage->DeletePlaceholder(placeholderId); }
                catch (...) {}
            }
            throw;
        }

        // Clean up the line for whatever comes next
        LOG_DEBUG("                                                           \r");
        try { contentStorage->DeletePlaceholder(placeholderId); }
        catch (...) {}
    }

    void XCIInstallTask::InstallTicketCert()
    {
        constexpr u64 kMaxTicketSize = 64ULL * 1024ULL;
        constexpr u64 kMaxCertificateSize = 128ULL * 1024ULL;
        struct TicketCandidate {
            const HFS0FileEntry* ticket;
            const HFS0FileEntry* certificate;
            std::array<u8, 16> rightsId;
        };
        std::vector<const HFS0FileEntry*> tikFileEntries = m_xci->GetFileEntriesByExtension("tik");
        std::vector<const HFS0FileEntry*> certFileEntries = m_xci->GetFileEntriesByExtension("cert");
        std::vector<TicketCandidate> candidates;
        std::vector<std::array<u8, 16>> matchedRightsIds;
        for (size_t i = 0; i < tikFileEntries.size(); i++)
        {
            if (tikFileEntries[i] == nullptr)
            {
                LOG_DEBUG("Remote tik file is missing.\n");
                THROW_FORMAT("Remote tik file is not present!");
            }

            u64 tikSize = tikFileEntries[i]->fileSize;
            if (tikSize < 4 || tikSize > kMaxTicketSize || tikSize > std::numeric_limits<size_t>::max())
                THROW_FORMAT("Ticket has an invalid size");
            auto tikBuf = std::make_unique<u8[]>(static_cast<size_t>(tikSize));
            LOG_DEBUG("> Reading tik\n");
            m_xci->BufferData(tikBuf.get(), m_xci->GetDataOffset() + tikFileEntries[i]->dataOffset,
                              static_cast<size_t>(tikSize));

            const std::string ticketName = m_xci->GetFileEntryName(tikFileEntries[i]);
            const auto rightsId = this->ResolveTicketRightsId(ticketName, tikBuf.get(), static_cast<size_t>(tikSize));
            if (!this->IsRequiredRightsId(rightsId.data()))
            {
                LOG_DEBUG("Skipping ticket not referenced by installed NCAs.\n");
                continue;
            }
            if (std::find(matchedRightsIds.begin(), matchedRightsIds.end(), rightsId) != matchedRightsIds.end())
                continue;

            matchedRightsIds.push_back(rightsId);
            if (this->TicketExists(rightsId.data()))
                continue;

            const std::string certName = ticketName.substr(0, ticketName.size() - 4) + ".cert";
            const HFS0FileEntry* certEntry = m_xci->GetFileEntryByName(certName);
            if (certEntry == nullptr && certFileEntries.size() == tikFileEntries.size() && i < certFileEntries.size())
                certEntry = certFileEntries[i];
            if (certEntry == nullptr)
            {
                LOG_DEBUG("Remote cert file is missing.\n");
                THROW_FORMAT("Remote cert file is not present!");
            }

            u64 certSize = certEntry->fileSize;
            if (certSize == 0 || certSize > kMaxCertificateSize || certSize > std::numeric_limits<size_t>::max())
                THROW_FORMAT("Ticket certificate has an invalid size");

            candidates.push_back({tikFileEntries[i], certEntry, rightsId});
        }
        for (const auto& required : m_requiredRightsIds)
        {
            const bool covered = std::find(matchedRightsIds.begin(), matchedRightsIds.end(), required) !=
                                 matchedRightsIds.end();
            if (!covered && !this->TicketExists(required.data()))
                THROW_FORMAT("A required ticket is neither installed nor present in the package");
        }

        for (const auto& candidate : candidates)
        {
            const u64 tikSize = candidate.ticket->fileSize;
            const u64 certSize = candidate.certificate->fileSize;
            auto tikBuf = std::make_unique<u8[]>(static_cast<size_t>(tikSize));
            auto certBuf = std::make_unique<u8[]>(static_cast<size_t>(certSize));
            LOG_DEBUG("> Reading tik\n");
            m_xci->BufferData(tikBuf.get(), m_xci->GetDataOffset() + candidate.ticket->dataOffset,
                              static_cast<size_t>(tikSize));
            LOG_DEBUG("> Reading cert\n");
            m_xci->BufferData(certBuf.get(), m_xci->GetDataOffset() + candidate.certificate->dataOffset,
                              static_cast<size_t>(certSize));

            this->RecordImportedTicket(candidate.rightsId.data());
            ASSERT_OK(esImportTicket(tikBuf.get(), tikSize, certBuf.get(), certSize), "Failed to import ticket");
        }
    }
}

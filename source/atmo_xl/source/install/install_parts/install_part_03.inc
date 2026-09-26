
    void Install::InstallNcaTracked(const NcmContentInfo& contentInfo)
    {
        nx::ncm::ContentStorage storage(m_destStorageId);
        const bool existed = storage.Has(contentInfo.content_id);
        if (existed)
        {
            u64 expectedSize = 0;
            ncmContentInfoSizeToU64(&contentInfo, &expectedSize);
            if (expectedSize != 0 && storage.GetSize(contentInfo.content_id) != expectedSize)
                THROW_FORMAT("Existing NCA size does not match the package metadata");
        }

        if (!existed &&
            std::none_of(m_newContentIds.begin(), m_newContentIds.end(),
                         [&](const NcmContentId& id) {
                             return same_content_id(id, contentInfo.content_id);
                         }))
            m_newContentIds.push_back(contentInfo.content_id);
        this->InstallNCA(contentInfo);
    }

    const u8* Install::FindExpectedContentHash(const NcmContentId& contentId) const
    {
        const auto value = std::find_if(m_expectedContentHashes.begin(),
                                        m_expectedContentHashes.end(),
                                        [&](const ExpectedContentHash& expected) {
                                            return same_content_id(expected.contentId, contentId);
                                        });
        return value == m_expectedContentHashes.end() ? nullptr : value->hash.data();
    }

    void Install::RegisterRequiredRightsId(const u8 rightsId[16])
    {
        const bool allZero = std::all_of(rightsId, rightsId + 16,
                                         [](u8 value) { return value == 0; });
        if (allZero) return;
        std::array<u8, 16> value{};
        std::memcpy(value.data(), rightsId, value.size());
        if (std::find(m_requiredRightsIds.begin(), m_requiredRightsIds.end(), value) ==
            m_requiredRightsIds.end())
            m_requiredRightsIds.push_back(value);
    }

    bool Install::IsRequiredRightsId(const u8 rightsId[16]) const
    {
        return std::any_of(m_requiredRightsIds.begin(), m_requiredRightsIds.end(),
                           [&](const auto& value) {
                               return std::memcmp(value.data(), rightsId, value.size()) == 0;
                           });
    }

    bool Install::TicketExists(const u8 rightsId[16]) const
    {
        auto listContains = [&](bool personalized) {
            u32 count = 0;
            const Result countRc = personalized
                ? esCountPersonalizedTicket(&count)
                : esCountCommonTicket(&count);
            ASSERT_OK(countRc, "Failed to count installed tickets");
            if (count > kMaxTicketCount)
                THROW_FORMAT("System ticket list is unreasonably large");
            if (count == 0) return false;
            std::vector<EsRightsId> ids(count);
            u32 written = 0;
            const Result rc = personalized
                ? esListPersonalizedTicket(&written, ids.data(), ids.size() * sizeof(ids[0]))
                : esListCommonTicket(&written, ids.data(), ids.size() * sizeof(ids[0]));
            ASSERT_OK(rc, "Failed to list installed tickets");
            if (written > count) THROW_FORMAT("System returned an invalid ticket count");
            return std::any_of(ids.begin(), ids.begin() + written,
                               [&](const EsRightsId& id) {
                                   return std::memcmp(id.bytes, rightsId, 16) == 0;
                               });
        };
        return listContains(false) || listContains(true);
    }

    void Install::RecordImportedTicket(const u8 rightsId[16])
    {
        std::array<u8, 16> value{};
        std::memcpy(value.data(), rightsId, value.size());
        if (std::find(m_importedRightsIds.begin(), m_importedRightsIds.end(), value) ==
            m_importedRightsIds.end())
            m_importedRightsIds.push_back(value);
    }

    std::array<u8, 16> Install::ExtractTicketRightsId(const u8* ticket, size_t size)
    {
        if (!ticket || size < 4) THROW_FORMAT("Ticket is truncated");
        size_t signatureBlockSize = 0;
        switch (read_be32(ticket))
        {
            case 0x00010000:
            case 0x00010003: signatureBlockSize = 0x240; break;
            case 0x00010001:
            case 0x00010004: signatureBlockSize = 0x140; break;
            case 0x00010002:
            case 0x00010005: signatureBlockSize = 0x80; break;
            case 0x00010006: signatureBlockSize = 0x40; break;
            default: THROW_FORMAT("Ticket has an unsupported signature type");
        }
        constexpr size_t kTicketDataSize = 0x180;
        constexpr size_t kTicketVersionOffset = 0x140;
        constexpr size_t kRightsIdOffset = 0x160;
        if (signatureBlockSize > size || size - signatureBlockSize < kTicketDataSize)
            THROW_FORMAT("Ticket data is truncated");
        const u8* data = ticket + signatureBlockSize;
        if (data[kTicketVersionOffset] != 2)
            THROW_FORMAT("Unsupported ticket version");
        std::array<u8, 16> rightsId{};
        std::memcpy(rightsId.data(), data + kRightsIdOffset, rightsId.size());
        return rightsId;
    }

    NcmPlaceHolderId Install::PlaceholderIdForContent(const NcmContentId& contentId)
    {
        static_assert(sizeof(NcmPlaceHolderId) == sizeof(NcmContentId),
                      "Content and placeholder IDs must have equal size");
        NcmPlaceHolderId placeholder{};
        std::memcpy(&placeholder, &contentId, sizeof(placeholder));
        return placeholder;
    }

    NcmPlaceHolderId Install::ReplacementPlaceholderIdForContent(const NcmContentId& contentId)
    {
        NcmPlaceHolderId placeholder = PlaceholderIdForContent(contentId);
        auto* bytes = reinterpret_cast<u8*>(&placeholder);
        bytes[0] ^= 0xA5;
        bytes[sizeof(placeholder) - 1] ^= 0x5A;
        return placeholder;
    }

    bool Install::Rollback(std::string* error) noexcept
    {
        std::string errors;
        bool applicationRecordsRestored = true;
        for (auto it = m_applicationRecords.rbegin(); it != m_applicationRecords.rend(); ++it)
        {
            if (!it->touched) continue;
            try
            {
                if (it->existed)
                {
                    const s32 count = count_application_records(it->applicationId);
                    if (count > 0)
                        ASSERT_OK(nsDeleteApplicationRecord(it->applicationId),
                                  "Failed to clear application record before restoring it");
                    ASSERT_OK(nsPushApplicationRecord(it->applicationId,
                              NsApplicationRecordType_Installed, it->previous.data(),
                              static_cast<u32>(it->previous.size())),
                              "Failed to restore application record");
                }
                else
                {
                    const s32 count = count_application_records(it->applicationId);
                    if (count > 0)
                        ASSERT_OK(nsDeleteApplicationRecord(it->applicationId),
                                  "Failed to remove new application record");
                }
                it->touched = false;
            }
            catch (...)
            {
                applicationRecordsRestored = false;
                append_error(errors, "application record rollback failed: " +
                                      current_exception_message());
            }
        }

        bool metadataRestored = true;
        if (m_metadataTouched)
        {
            try
            {
                NcmContentMetaDatabase database{};
                ASSERT_OK(ncmOpenContentMetaDatabase(&database, m_destStorageId),
                          "Failed to open content meta database for rollback");
                try
                {
                    for (auto it = m_preparedMetaRecords.rbegin();
                         it != m_preparedMetaRecords.rend(); ++it)
                    {
                        if (it->existed)
                        {
                            ASSERT_OK(ncmContentMetaDatabaseSet(&database, &it->key,
                                      it->previous.GetData(), it->previous.GetSize()),
                                      "Failed to restore content meta record");
                        }
                        else
                        {
                            bool present = false;
                            ASSERT_OK(ncmContentMetaDatabaseHas(&database, &present, &it->key),
                                      "Failed to inspect new content meta record");
                            if (present)
                                ASSERT_OK(ncmContentMetaDatabaseRemove(&database, &it->key),
                                          "Failed to remove new content meta record");
                        }
                    }
                    for (const auto& old : m_supersededMetaRecords)
                        ASSERT_OK(ncmContentMetaDatabaseSet(&database, &old.key,
                                  old.previous.GetData(), old.previous.GetSize()),
                                  "Failed to restore superseded content meta record");
                    ASSERT_OK(ncmContentMetaDatabaseCommit(&database),
                              "Failed to commit content meta rollback");
                }
                catch (...)
                {
                    ncmContentMetaDatabaseClose(&database);
                    throw;
                }
                ncmContentMetaDatabaseClose(&database);
                m_metadataTouched = false;
            }
            catch (...)
            {
                metadataRestored = false;
                append_error(errors, "content metadata rollback failed: " +
                                      current_exception_message());
            }
        }

        if (applicationRecordsRestored && metadataRestored)
        {
            std::vector<NcmContentId> retainedContent;
            try
            {
                nx::ncm::ContentStorage storage(m_destStorageId);
                for (auto it = m_newContentIds.rbegin(); it != m_newContentIds.rend(); ++it)
                {
                    bool removed = true;
                    try
                    {
                        if (storage.Has(*it)) storage.Delete(*it);
                    }
                    catch (...)
                    {
                        removed = false;
                        append_error(errors, "installed content rollback failed: " +
                                              current_exception_message());
                    }
                    try
                    {
                        storage.DeletePlaceholder(PlaceholderIdForContent(*it));
                    }
                    catch (...)
                    {
                        // A registered content normally has no remaining placeholder.
                    }
                    if (!removed) retainedContent.push_back(*it);
                }
            }
            catch (...)
            {
                retainedContent = m_newContentIds;
                append_error(errors, "content storage rollback failed: " +
                                      current_exception_message());
            }
            std::reverse(retainedContent.begin(), retainedContent.end());
            m_newContentIds = std::move(retainedContent);

            std::vector<std::array<u8, 16>> retainedTickets;
            for (auto it = m_importedRightsIds.rbegin(); it != m_importedRightsIds.rend(); ++it)
            {
                try
                {
                    if (TicketExists(it->data()))
                    {
                        EsRightsId id{};
                        std::memcpy(id.bytes, it->data(), it->size());
                        ASSERT_OK(esDeleteTicket(&id), "Failed to remove imported ticket");
                    }
                }
                catch (...)
                {
                    retainedTickets.push_back(*it);
                    append_error(errors, "ticket rollback failed: " +
                                          current_exception_message());
                }
            }
            std::reverse(retainedTickets.begin(), retainedTickets.end());
            m_importedRightsIds = std::move(retainedTickets);
        }
        else if (!m_newContentIds.empty() || !m_importedRightsIds.empty())
        {
            append_error(errors,
                         "new content was retained because database rollback was incomplete");
        }

        m_beginCompleted = false;
        if (error) *error = errors;
        return errors.empty();
    }

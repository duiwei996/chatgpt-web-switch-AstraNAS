
    [[noreturn]] void Install::RethrowAfterRollback(std::exception_ptr original)
    {
        std::string originalMessage = "installation failed";
        try
        {
            std::rethrow_exception(original);
        }
        catch (const std::exception& failure)
        {
            originalMessage = failure.what();
        }
        catch (...)
        {
            originalMessage = "installation failed with an unknown error";
        }

        std::string rollbackError;
        if (!this->Rollback(&rollbackError))
            throw std::runtime_error(originalMessage +
                                     " | rollback incomplete: " + rollbackError);
        std::rethrow_exception(original);
    }

    void Install::Verify()
    {
        if (!m_beginCompleted || m_committed)
            THROW_FORMAT("Installation transaction is not ready for verification");

        try
        {
            nx::ncm::ContentStorage storage(m_destStorageId);
            auto verifyContent = [&](const NcmContentInfo& content, const char* description) {
                if (!storage.Has(content.content_id))
                    THROW_FORMAT("Installed %s content is missing", description);
                u64 expectedSize = 0;
                ncmContentInfoSizeToU64(&content, &expectedSize);
                if (expectedSize != 0 && storage.GetSize(content.content_id) != expectedSize)
                    THROW_FORMAT("Installed %s content has the wrong size", description);
            };
            for (const auto& cnmt : m_cnmtContentRecords)
                verifyContent(cnmt, "CNMT");
            for (auto& meta : m_contentMeta)
                for (const auto& content : meta.GetContentInfos())
                    verifyContent(content, "NCA");

            NcmContentMetaDatabase database{};
            ASSERT_OK(ncmOpenContentMetaDatabase(&database, m_destStorageId),
                      "Failed to open content meta database for verification");
            try
            {
                for (const auto& record : m_preparedMetaRecords)
                {
                    u64 size = 0;
                    ASSERT_OK(ncmContentMetaDatabaseGetSize(&database, &size, &record.key),
                              "Failed to verify content meta record size");
                    if (size != record.install.GetSize())
                        THROW_FORMAT("Installed content meta record has the wrong size");
                    tin::data::ByteBuffer actual(static_cast<size_t>(size));
                    u64 written = 0;
                    ASSERT_OK(ncmContentMetaDatabaseGet(&database, &record.key, &written,
                              actual.GetData(), actual.GetSize()),
                              "Failed to read installed content meta record");
                    if (written != size ||
                        std::memcmp(actual.GetData(), record.install.GetData(),
                                    actual.GetSize()) != 0)
                        THROW_FORMAT("Installed content meta record does not match the package");
                }
                for (const auto& old : m_supersededMetaRecords)
                {
                    bool present = false;
                    ASSERT_OK(ncmContentMetaDatabaseHas(&database, &present, &old.key),
                              "Failed to inspect superseded content meta record");
                    if (present) THROW_FORMAT("Superseded content meta version is still active");
                }
            }
            catch (...)
            {
                ncmContentMetaDatabaseClose(&database);
                throw;
            }
            ncmContentMetaDatabaseClose(&database);

            for (const auto& snapshot : m_applicationRecords)
            {
                const s32 count = count_application_records(snapshot.applicationId);
                if (count < 0 || count > kMaxApplicationMetaRecords)
                    THROW_FORMAT("Installed application record has an invalid size");
                std::vector<ContentStorageRecord> records(static_cast<size_t>(count));
                u32 written = 0;
                if (count > 0)
                    ASSERT_OK(nsListApplicationRecordContentMeta(
                              0, snapshot.applicationId, records.data(),
                              static_cast<u32>(records.size()), &written),
                              "Failed to verify application record");
                if (written != static_cast<u32>(count))
                    THROW_FORMAT("Installed application record changed during verification");
                if (records.size() != snapshot.installed.size())
                    THROW_FORMAT("Installed application record has unexpected entries");
                for (const auto& expected : snapshot.installed)
                {
                    if (std::none_of(records.begin(), records.end(), [&](const auto& actual) {
                            return same_storage_record(actual, expected);
                        }))
                        THROW_FORMAT("Installed application record is incomplete");
                }
            }

            for (const auto& rightsId : m_requiredRightsIds)
                if (!TicketExists(rightsId.data()))
                    THROW_FORMAT("Required ticket is not installed");

            m_committed = true;
            this->CleanupSupersededContent();
        }
        catch (...)
        {
            this->RethrowAfterRollback(std::current_exception());
        }
    }

    void Install::CleanupSupersededContent() noexcept
    {
        if (m_supersededMetaRecords.empty()) return;
        try
        {
            NcmContentMetaDatabase database{};
            ASSERT_OK(ncmOpenContentMetaDatabase(&database, m_destStorageId),
                      "Failed to open content meta database for old-content cleanup");
            try
            {
                nx::ncm::ContentStorage storage(m_destStorageId);
                for (const auto& old : m_supersededMetaRecords)
                    for (const auto& contentId : old.contentIds)
                    {
                        bool orphaned = false;
                        ASSERT_OK(ncmContentMetaDatabaseLookupOrphanContent(
                                      &database, &orphaned, &contentId, 1),
                                  "Failed to determine whether old content is orphaned");
                        if (orphaned && storage.Has(contentId)) storage.Delete(contentId);
                    }
            }
            catch (...)
            {
                ncmContentMetaDatabaseClose(&database);
                throw;
            }
            ncmContentMetaDatabaseClose(&database);
        }
        catch (...)
        {
            m_warning = "installed successfully, but superseded content cleanup failed: " +
                        current_exception_message();
        }
    }

    u64 Install::GetTitleId(int i)
    {
        return m_contentMeta.at(static_cast<size_t>(i)).GetContentMetaKey().id;
    }

    NcmContentMetaType Install::GetContentMetaType(int i)
    {
        return static_cast<NcmContentMetaType>(
            m_contentMeta.at(static_cast<size_t>(i)).GetContentMetaKey().type);
    }

    std::vector<PreparedContentInfo> Install::GetPreparedContents()
    {
        std::vector<PreparedContentInfo> result;
        result.reserve(m_contentMeta.size());
        for (size_t i = 0; i < m_contentMeta.size(); ++i)
        {
            auto& meta = m_contentMeta[i];
            const auto header = meta.GetPackagedContentMetaHeader();
            PreparedContentInfo info{};
            info.titleId = header.title_id;
            info.applicationId = meta.GetApplicationId();
            info.version = header.version;
            info.requiredSystemVersion = header.required_system_version;
            info.type = static_cast<NcmContentMetaType>(header.type);

            auto appendSize = [&](const NcmContentInfo& content) {
                u64 size = 0;
                ncmContentInfoSizeToU64(&content, &size);
                if (size > std::numeric_limits<u64>::max() - info.installSize)
                    THROW_FORMAT("Prepared content size overflow");
                info.installSize += size;
            };
            if (i < m_cnmtContentRecords.size()) appendSize(m_cnmtContentRecords[i]);
            for (const auto& content : meta.GetContentInfos()) appendSize(content);
            result.push_back(info);
        }
        return result;
    }

    PreparedStorageInfo Install::GetPreparedStorageInfo()
    {
        if (m_contentMeta.empty() || m_preparedMetaRecords.empty())
            THROW_FORMAT("Install task has not been prepared");

        std::vector<NcmContentInfo> records;
        for (const auto& cnmt : m_cnmtContentRecords)
            append_unique_content_info(records, cnmt);
        for (auto& meta : m_contentMeta)
            for (const auto& content : meta.GetContentInfos())
                append_unique_content_info(records, content);

        nx::ncm::ContentStorage storage(m_destStorageId);
        PreparedStorageInfo info{};
        u64 stagedByPrepare = 0;
        u64 forceReserve = 0;

        for (const auto& record : records)
        {
            const u64 size = content_info_size(record);
            const bool staged = std::any_of(
                m_newContentIds.begin(), m_newContentIds.end(),
                [&](const NcmContentId& id) {
                    return same_content_id(id, record.content_id);
                });

            if (staged)
            {
                if (size > std::numeric_limits<u64>::max() - info.requiredSpace ||
                    size > std::numeric_limits<u64>::max() - stagedByPrepare)
                    THROW_FORMAT("Prepared storage size overflow");
                info.requiredSpace += size;
                stagedByPrepare += size;
                continue;
            }

            if (storage.Has(record.content_id))
            {
                const u64 installedSize = storage.GetSize(record.content_id);
                if (installedSize != size)
                    THROW_FORMAT("Existing NCA size does not match the package metadata");
                forceReserve = std::max(forceReserve, size);
                continue;
            }

            if (size > std::numeric_limits<u64>::max() - info.requiredSpace)
                THROW_FORMAT("Prepared storage size overflow");
            info.requiredSpace += size;
        }

        const u64 currentFree = storage.GetFreeSpace();
        if (stagedByPrepare > std::numeric_limits<u64>::max() - currentFree)
            THROW_FORMAT("Destination free-space accounting overflow");
        info.freeSpace = currentFree + stagedByPrepare;
        if (forceReserve > std::numeric_limits<u64>::max() - info.requiredSpace)
            THROW_FORMAT("Force-install storage size overflow");
        info.forceRequiredSpace = info.requiredSpace + forceReserve;
        if (info.requiredSpace > info.freeSpace)
            THROW_FORMAT("not enough destination space: need %lu bytes, free %lu",
                         info.requiredSpace, info.freeSpace);
        return info;
    }

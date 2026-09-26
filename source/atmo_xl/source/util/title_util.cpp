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

#include "util/title_util.hpp"

#include <machine/endian.h>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include "util/error.hpp"

namespace tin::util
{
    namespace
    {
        u64 read_be_u64(const u8* bytes)
        {
            u64 value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return __bswap64(value);
        }
    }

    u64 GetRightsIdTid(FsRightsId rightsId)
    {
        return read_be_u64(rightsId.c);
    }

    u64 GetRightsIdKeyGen(FsRightsId rightsId)
    {
        return read_be_u64(rightsId.c + 8);
    }

    std::string GetNcaIdString(const NcmContentId& ncaId)
    {
        char ncaIdStr[FS_MAX_PATH] = {0};
        u64 ncaIdLower = read_be_u64(ncaId.c);
        u64 ncaIdUpper = read_be_u64(ncaId.c + 0x8);
        snprintf(ncaIdStr, FS_MAX_PATH, "%016lx%016lx", ncaIdLower, ncaIdUpper);
        return std::string(ncaIdStr);
    }

    NcmContentId GetNcaIdFromString(std::string ncaIdStr)
    {
        if (ncaIdStr.size() < 32)
            THROW_FORMAT("NCA content ID is truncated");
        for (size_t i = 0; i < 32; ++i)
            if (!std::isxdigit(static_cast<unsigned char>(ncaIdStr[i])))
                THROW_FORMAT("NCA content ID is not hexadecimal");
        NcmContentId ncaId = {0};
        char lowerU64[17] = {0};
        char upperU64[17] = {0};
        memcpy(lowerU64, ncaIdStr.c_str(), 16);
        memcpy(upperU64, ncaIdStr.c_str() + 16, 16);

        const u64 lower = __bswap64(strtoull(lowerU64, NULL, 16));
        const u64 upper = __bswap64(strtoull(upperU64, NULL, 16));
        std::memcpy(ncaId.c, &lower, sizeof(lower));
        std::memcpy(ncaId.c + sizeof(lower), &upper, sizeof(upper));

        return ncaId;
    }

    u64 GetBaseTitleId(u64 titleId, NcmContentMetaType contentMetaType)
    {
        switch (contentMetaType)
        {
            case NcmContentMetaType_Patch:
                return titleId ^ 0x800;

            case NcmContentMetaType_AddOnContent:
                return (titleId ^ 0x1000) & ~0xFFF;

            default:
                return titleId;
        }
    }

    std::string GetBaseTitleName(u64 baseTitleId)
    {
        Result rc = 0;
        NsApplicationControlData appControlData;
        size_t sizeRead;

        if (R_FAILED(rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, baseTitleId, &appControlData, sizeof(NsApplicationControlData), &sizeRead)))
        {
            LOG_DEBUG("Failed to get application control data. Error code: 0x%08x\n", rc);
            return "Unknown";
        }

        if (sizeRead < sizeof(appControlData.nacp))
        {
            LOG_DEBUG("Incorrect size for nacp\n");
            return "Unknown";
        }

        NacpLanguageEntry *languageEntry;

        if (R_FAILED(rc = nacpGetLanguageEntry(&appControlData.nacp, &languageEntry)))
        {
            LOG_DEBUG("Failed to get language entry. Error code: 0x%08x\n", rc);
            return "Unknown";
        }

        if (languageEntry == NULL)
        {
            LOG_DEBUG("Language entry is null! Error code: 0x%08x\n", rc);
            return "Unknown";
        }

        return languageEntry->name;
    }

    std::string GetTitleName(u64 titleId, NcmContentMetaType contentMetaType)
    {
        u64 baseTitleId = GetBaseTitleId(titleId, contentMetaType);
        std::string titleName = GetBaseTitleName(baseTitleId);

        switch (contentMetaType)
        {
            case NcmContentMetaType_Patch:
                titleName += " (Update)";
                break;

            case NcmContentMetaType_AddOnContent:
                titleName += " (DLC)";
                break;

            default:
                break;
        }

        return titleName;
    }
}

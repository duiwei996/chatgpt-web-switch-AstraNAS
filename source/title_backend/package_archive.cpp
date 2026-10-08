// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_archive.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace astranas::title_backend {
namespace {

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t le64(const unsigned char* p) {
    std::uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

bool PackageArchive::parse_table(PackageSource& source, std::uint64_t base,
                                 const char* magic, std::uint64_t entrySize,
                                 std::string& error, bool allow_xcz_root_tail_padding) {
    if (base > source.size() || source.size() - base < 0x10) {
        error = "package table header is outside the file";
        return false;
    }

    std::array<unsigned char, 0x10> header{};
    std::size_t actual = 0;
    if (!source.read_at(base, header.data(), header.size(), actual, error) ||
        actual != header.size() || std::memcmp(header.data(), magic, 4) != 0) {
        if (error.empty()) error = "invalid package table";
        return false;
    }

    const std::uint32_t count = le32(header.data() + 4);
    const std::uint32_t stringSize = le32(header.data() + 8);
    if (count == 0 || count > 16384 || stringSize == 0 || stringSize > 4u * 1024u * 1024u) {
        error = "package table is unreasonable";
        return false;
    }
    if (entrySize > std::numeric_limits<std::size_t>::max() / count) {
        error = "package entry table size overflow";
        return false;
    }

    const std::size_t entriesSize = static_cast<std::size_t>(count * entrySize);
    const std::uint64_t tableSize = 0x10ull + entriesSize + stringSize;
    if (tableSize > source.size() - base) {
        error = "package table is truncated";
        return false;
    }

    std::vector<unsigned char> raw(entriesSize);
    if (!source.read_at(base + 0x10, raw.data(), raw.size(), actual, error) || actual != raw.size()) {
        if (error.empty()) error = "package entries are truncated";
        return false;
    }
    std::vector<char> strings(static_cast<std::size_t>(stringSize) + 1, '\0');
    if (!source.read_at(base + 0x10 + raw.size(), strings.data(), stringSize, actual, error) ||
        actual != stringSize) {
        if (error.empty()) error = "package string table is truncated";
        return false;
    }

    const std::uint64_t dataBase = base + tableSize;
    std::vector<PackageEntry> parsed;
    parsed.reserve(count);
    std::unordered_set<std::string> names;
    // Some XCZ writers declare an aligned HFS0 root size without writing the
    // final padding. This exception is only for the outermost, last partition.
    std::int32_t padded_root_entry = -1;
    const auto out_of_bounds = [&](std::uint32_t index, const char* reason,
                                   std::uint64_t offset, std::uint64_t length,
                                   std::uint32_t nameOffset) {
        std::ostringstream details;
        details << "package entry is outside the file (" << reason << ")"
                << ": index=" << index << " table_base=" << base
                << " data_base=" << dataBase << " source_size=" << source.size()
                << " offset=" << offset << " size=" << length
                << " name_offset=" << nameOffset;
        error = details.str();
        return false;
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* entry = raw.data() + static_cast<std::size_t>(i) * entrySize;
        const std::uint64_t offset = le64(entry);
        const std::uint64_t size = le64(entry + 8);
        const std::uint32_t nameOffset = le32(entry + 16);
        if (nameOffset >= stringSize)
            return out_of_bounds(i, "invalid name offset", offset, size, nameOffset);
        if (offset > source.size() - dataBase)
            return out_of_bounds(i, "start past EOF", offset, size, nameOffset);
        const std::uint64_t available = source.size() - dataBase - offset;
        if (size > available) {
            const std::uint64_t overhang = size - available;
            if (!allow_xcz_root_tail_padding || padded_root_entry >= 0 ||
                (size % 0x200u) != 0 || overhang > 0x200u)
                return out_of_bounds(i, "end past EOF", offset, size, nameOffset);
            padded_root_entry = static_cast<std::int32_t>(i);
        }

        const char* begin = strings.data() + nameOffset;
        const char* end = begin;
        const char* limit = strings.data() + stringSize;
        while (end < limit && *end != '\0') ++end;
        if (end == limit || end == begin || end - begin > 255) {
            error = "package entry has an invalid name";
            return false;
        }
        std::string name(begin, end);
        if (std::any_of(name.begin(), name.end(), [](unsigned char c) {
                return c < 0x20 || c == 0x7f || c == '/' || c == '\\';
            })) {
            error = "package entry has an unsafe name";
            return false;
        }
        if (!names.insert(lower(name)).second) {
            error = "package contains duplicate entry names";
            return false;
        }
        parsed.push_back({std::move(name), dataBase + offset, size});
    }

    std::vector<const PackageEntry*> ordered;
    ordered.reserve(parsed.size());
    for (const auto& entry : parsed) ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->offset < rhs->offset;
    });
    if (padded_root_entry >= 0 &&
        ordered.back() != &parsed[static_cast<std::size_t>(padded_root_entry)]) {
        error = "XCZ root padding is not in the final partition";
        return false;
    }
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        const auto* previous = ordered[i - 1];
        if (previous->size != 0 &&
            previous->offset + previous->size > ordered[i]->offset) {
            error = "package contains overlapping entries";
            return false;
        }
    }

    entries_.insert(entries_.end(), parsed.begin(), parsed.end());
    return true;
}

bool PackageArchive::open(PackageSource& source, PackageContainerKind kind, std::string& error) {
    entries_.clear();
    if (kind == PackageContainerKind::Nsp || kind == PackageContainerKind::Nsz)
        return parse_table(source, 0, "PFS0", 0x18, error);
    if (kind != PackageContainerKind::Xci && kind != PackageContainerKind::Xcz) {
        error = "unsupported package container";
        return false;
    }

    constexpr std::uint64_t kXciRootHfs0Offset = 0xF000;
    if (!parse_table(source, kXciRootHfs0Offset, "HFS0", 0x40, error,
                     kind == PackageContainerKind::Xcz)) return false;

    const auto secure = std::find_if(entries_.begin(), entries_.end(), [](const PackageEntry& entry) {
        return lower(entry.name) == "secure";
    });
    if (secure == entries_.end()) {
        entries_.clear();
        error = "XCI secure partition is missing";
        return false;
    }

    const std::uint64_t secureOffset = secure->offset;
    entries_.clear();
    if (parse_table(source, secureOffset, "HFS0", 0x40, error)) return true;
    entries_.clear();
    return false;
}

std::vector<const PackageEntry*> PackageArchive::suffix(const std::string& extension) const {
    std::vector<const PackageEntry*> result;
    const std::string expected = lower(extension);
    for (const auto& entry : entries_) {
        const std::string name = lower(entry.name);
        if (name.size() >= expected.size() &&
            name.compare(name.size() - expected.size(), expected.size(), expected) == 0)
            result.push_back(&entry);
    }
    return result;
}

} // namespace astranas::title_backend

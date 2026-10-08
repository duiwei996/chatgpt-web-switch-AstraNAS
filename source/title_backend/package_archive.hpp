// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "package_source.hpp"
#include "package_inspect.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace astranas::title_backend {
struct PackageEntry { std::string name; std::uint64_t offset = 0; std::uint64_t size = 0; };
class PackageArchive {
public:
    bool open(PackageSource&, PackageContainerKind, std::string&);
    const std::vector<PackageEntry>& entries() const { return entries_; }
    std::vector<const PackageEntry*> suffix(const std::string&) const;
private:
    bool parse_table(PackageSource&, std::uint64_t, const char*, std::uint64_t, std::string&, bool allow_xcz_root_tail_padding = false);
    std::vector<PackageEntry> entries_;
};
} // namespace astranas::title_backend

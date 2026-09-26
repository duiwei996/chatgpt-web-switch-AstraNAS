// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_inspect.hpp"
#include "title_backend/package_archive.hpp"
#include "title_backend/package_source.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace {
std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string extension_of(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower_copy(path.substr(dot));
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

PackageContainerKind split_part_kind(const std::string& path) {
    if (!astranas::title_backend::is_split_package_first_part(path))
        return PackageContainerKind::None;
    const std::string lower = lower_copy(path);
    const auto slash = lower.find_last_of("/\\");
    const std::string name = lower.substr(slash == std::string::npos ? 0 : slash + 1);
    if (name.find(".nsp.") != std::string::npos || name.find(".nsp.part") != std::string::npos ||
        (extension_of(name).size() >= 4 && extension_of(name).substr(0, 3) == ".ns"))
        return PackageContainerKind::Nsp;
    if (name.find(".nsz.") != std::string::npos || name.find(".nsz.part") != std::string::npos)
        return PackageContainerKind::Nsz;
    if (name.find(".xci.") != std::string::npos || name.find(".xci.part") != std::string::npos ||
        (extension_of(name).size() >= 4 && extension_of(name).substr(0, 3) == ".xc"))
        return PackageContainerKind::Xci;
    if (name.find(".xcz.") != std::string::npos || name.find(".xcz.part") != std::string::npos)
        return PackageContainerKind::Xcz;
    return PackageContainerKind::None;
}

void parse_filename_hints(const std::string& path, PackageInspection& out) {
    const auto slash = path.find_last_of("/\\");
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    for (std::size_t i = 0; i + 18 <= name.size(); ++i) {
        if (name[i] != '[' || name[i + 17] != ']') continue;
        bool hex = true;
        for (std::size_t j = i + 1; j < i + 17; ++j) {
            if (!std::isxdigit(static_cast<unsigned char>(name[j]))) { hex = false; break; }
        }
        if (hex) {
            out.title_id_hint = name.substr(i + 1, 16);
            std::transform(out.title_id_hint.begin(), out.title_id_hint.end(),
                           out.title_id_hint.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            break;
        }
    }
    for (std::size_t i = 0; i + 4 <= name.size(); ++i) {
        if (name[i] != '[' || (name[i + 1] != 'v' && name[i + 1] != 'V')) continue;
        std::uint64_t value = 0;
        bool any = false;
        std::size_t j = i + 2;
        for (; j < name.size() && std::isdigit(static_cast<unsigned char>(name[j])); ++j) {
            any = true;
            value = value * 10 + static_cast<unsigned>(name[j] - '0');
            if (value > 0xFFFFFFFFull) value = 0xFFFFFFFFull;
        }
        if (any && j < name.size() && name[j] == ']') {
            out.version_hint = static_cast<std::uint32_t>(value);
            break;
        }
    }
}

void classify_member_name(const std::string& raw, PackageInspection& out) {
    const std::string name = lower_copy(raw);
    if (ends_with(name, ".cnmt.nca") || ends_with(name, ".cnmt.ncz")) out.has_cnmt_nca = true;
    if (ends_with(name, ".tik")) { out.has_ticket = true; ++out.ticket_count; }
    if (ends_with(name, ".cert")) { out.has_cert = true; ++out.certificate_count; }
}

std::string build_summary(const PackageInspection& out, bool split) {
    std::string summary = package_container_label(out.kind);
    if (split) summary += " split";
    if (out.deep_inspected) summary += " files=" + std::to_string(out.file_count);
    if (out.has_cnmt_nca) summary += " CNMT";
    if (out.has_ticket) summary += " TIK=" + std::to_string(out.ticket_count);
    if (out.has_cert) summary += " CERT=" + std::to_string(out.certificate_count);
    if (!out.title_id_hint.empty()) summary += " id~" + out.title_id_hint;
    if (out.version_hint) summary += " v~" + std::to_string(out.version_hint);
    if (out.kind == PackageContainerKind::Nsz || out.kind == PackageContainerKind::Xcz)
        summary += " compressed";
    return summary;
}
} // namespace

PackageContainerKind detect_package_container(const std::string& path) {
    if (astranas::title_backend::is_split_package_later_part(path)) return PackageContainerKind::None;
    const auto split_kind = split_part_kind(path);
    if (split_kind != PackageContainerKind::None) return split_kind;
    const std::string ext = extension_of(path);
    if (ext == ".nro") return PackageContainerKind::HomebrewNro;
    if (ext == ".nsp") return PackageContainerKind::Nsp;
    if (ext == ".nsz") return PackageContainerKind::Nsz;
    if (ext == ".xci") return PackageContainerKind::Xci;
    if (ext == ".xcz") return PackageContainerKind::Xcz;
    return PackageContainerKind::None;
}

const char* package_container_label(PackageContainerKind kind) {
    switch (kind) {
        case PackageContainerKind::HomebrewNro: return "NRO";
        case PackageContainerKind::Nsp: return "NSP";
        case PackageContainerKind::Nsz: return "NSZ";
        case PackageContainerKind::Xci: return "XCI";
        case PackageContainerKind::Xcz: return "XCZ";
        default: return "FILE";
    }
}

bool inspect_package_file(const std::string& path, PackageInspection& out, std::string& error) {
    out = PackageInspection{};
    error.clear();
    out.kind = detect_package_container(path);
    out.recognized = out.kind != PackageContainerKind::None;
    parse_filename_hints(path, out);
    if (!out.recognized) { out.summary = "FILE"; return true; }
    if (out.kind == PackageContainerKind::HomebrewNro) {
        out.summary = build_summary(out, false);
        return true;
    }

    astranas::title_backend::FilePackageSource source;
    if (!source.open(path, error)) {
        out.summary = build_summary(out, false);
        return false;
    }
    const bool ok = inspect_package_source(path, out.kind, source, out, error);
    out.summary = build_summary(out, source.split());
    return ok;
}

bool inspect_package_source(const std::string& display_path, PackageContainerKind kind,
                            astranas::title_backend::PackageSource& source,
                            PackageInspection& out, std::string& error) {
    out = PackageInspection{};
    error.clear();
    out.kind = kind;
    out.recognized = kind != PackageContainerKind::None;
    parse_filename_hints(display_path, out);
    if (!out.recognized) { out.summary = "FILE"; return true; }
    if (kind == PackageContainerKind::HomebrewNro) {
        out.summary = build_summary(out, false);
        return true;
    }

    astranas::title_backend::PackageArchive archive;
    if (!archive.open(source, kind, error)) {
        out.summary = build_summary(out, false);
        return false;
    }
    out.file_count = static_cast<std::uint32_t>(archive.entries().size());
    out.deep_inspected = true;
    for (const auto& entry : archive.entries()) classify_member_name(entry.name, out);
    out.summary = build_summary(out, false);
    return true;
}

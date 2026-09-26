// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace astranas::title_backend { class PackageSource; }

// Read-only package/container inspection. This deliberately does not install,
// decrypt, validate tickets, or bypass platform authorization.
enum class PackageContainerKind {
    None,
    HomebrewNro,
    Nsp,
    Nsz,
    Xci,
    Xcz,
};

struct PackageInspection {
    PackageContainerKind kind = PackageContainerKind::None;
    bool recognized = false;
    bool deep_inspected = false;
    std::uint32_t file_count = 0;
    std::uint32_t ticket_count = 0;
    std::uint32_t certificate_count = 0;
    bool has_cnmt_nca = false;
    bool has_ticket = false;
    bool has_cert = false;
    std::string title_id_hint;
    std::uint32_t version_hint = 0;
    std::string summary;
};

PackageContainerKind detect_package_container(const std::string& path);
const char* package_container_label(PackageContainerKind kind);
bool inspect_package_file(const std::string& path, PackageInspection& out, std::string& error);
bool inspect_package_source(const std::string& display_path, PackageContainerKind kind,
                            astranas::title_backend::PackageSource& source,
                            PackageInspection& out, std::string& error);

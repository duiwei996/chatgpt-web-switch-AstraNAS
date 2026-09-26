// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch_install_order.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
astranas::user_backend::PackageInstallInfo package(
        std::uint64_t title_id, std::uint64_t application_id,
        astranas::user_backend::ContentKind kind) {
    astranas::user_backend::PackageInstallInfo info;
    astranas::user_backend::PackageContentInfo content;
    content.title_id = title_id;
    content.application_id = application_id;
    content.kind = kind;
    info.contents.push_back(content);
    return info;
}
} // namespace

int main() {
    using astranas::user_backend::ContentKind;
    using astranas::user_backend::PackageInstallInfo;
    constexpr std::uint64_t game = 0x0100000000001000ull;

    PackageInstallInfo data_patch = package(game + 0x1100, game, ContentKind::DataPatch);
    PackageInstallInfo dlc = package(game + 0x1000, game, ContentKind::AddOnContent);
    PackageInstallInfo same_id_patch = package(game, game, ContentKind::Patch);
    PackageInstallInfo patch = package(game + 0x800, game, ContentKind::Patch);
    PackageInstallInfo application = package(game, game, ContentKind::Application);
    PackageInstallInfo bundle = package(game + 0x1000, game, ContentKind::AddOnContent);
    bundle.contents.push_back(package(game, game, ContentKind::Application).contents.front());
    PackageInstallInfo another_application = package(game + 0x10000, game + 0x10000,
                                                      ContentKind::Application);
    PackageInstallInfo no_metadata;
    PackageInstallInfo second_no_metadata;

    const std::vector<PackageInstallInfo> infos{
        data_patch, dlc, same_id_patch, patch, application, bundle,
        another_application, no_metadata, second_no_metadata};
    const auto order = astranas::app::order_batch_install_infos(infos);
    const std::vector<std::size_t> expected{4, 5, 2, 3, 1, 0, 6, 7, 8};
    assert(order == expected);
}

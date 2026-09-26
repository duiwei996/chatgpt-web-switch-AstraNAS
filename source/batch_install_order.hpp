// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "user_title_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <tuple>
#include <vector>

namespace astranas::app {

struct BatchInstallOrderKey {
    std::uint64_t title_id = std::numeric_limits<std::uint64_t>::max();
    unsigned int content_kind_order = 4;
};

inline unsigned int batch_content_kind_order(user_backend::ContentKind kind) {
    switch (kind) {
        case user_backend::ContentKind::Application: return 0;
        case user_backend::ContentKind::Patch: return 1;
        case user_backend::ContentKind::AddOnContent: return 2;
        case user_backend::ContentKind::DataPatch: return 3;
        default: return 4;
    }
}

inline BatchInstallOrderKey batch_install_order_key(
        const user_backend::PackageInstallInfo& info) {
    BatchInstallOrderKey key;
    for (const auto& content : info.contents) {
        const std::uint64_t title_id = content.title_id != 0
            ? content.title_id : content.application_id;
        if (title_id == 0) continue;
        const BatchInstallOrderKey candidate{title_id, batch_content_kind_order(content.kind)};
        if (std::tie(candidate.title_id, candidate.content_kind_order) <
            std::tie(key.title_id, key.content_kind_order))
            key = candidate;
    }
    return key;
}

inline bool batch_install_order_less(const user_backend::PackageInstallInfo& lhs,
                                    const user_backend::PackageInstallInfo& rhs) {
    const auto lhs_key = batch_install_order_key(lhs);
    const auto rhs_key = batch_install_order_key(rhs);
    return std::tie(lhs_key.title_id, lhs_key.content_kind_order) <
           std::tie(rhs_key.title_id, rhs_key.content_kind_order);
}

// Returns a stable order by Title ID and then Application, Patch, AddOnContent,
// and DataPatch. Packages without prepared content metadata stay at the end.
inline std::vector<std::size_t> order_batch_install_infos(
        const std::vector<user_backend::PackageInstallInfo>& infos) {
    std::vector<std::size_t> order(infos.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return batch_install_order_less(infos[lhs], infos[rhs]);
    });
    return order;
}

} // namespace astranas::app

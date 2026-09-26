// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model.hpp"
#include <cstdint>
#include <string>
#include <vector>

class TitleCatalog {
public:
    bool refresh(std::string& error);
    const std::vector<InstalledTitle>& titles() const { return titles_; }
    bool uninstall(std::uint64_t application_id, std::string& error);
private:
    std::vector<InstalledTitle> titles_;
};

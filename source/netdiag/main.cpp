// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"
#include "netdiag/ui.hpp"
#include "network_runtime.hpp"
#include "ui/gui.hpp"
#include "ui/input.hpp"
#include <switch.h>
#include <string>
#include <utility>

namespace {
constexpr const char* kConfigPath = "sdmc:/switch/AstraNAS/config.ini";
}

int main(int, char**) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad{};
    padInitializeDefault(&pad);

    astranas::ui::InputRouter input;
    input.initialize_touch();

    const bool nifm_ready = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));

    astranas::ui::Gui gui;
    std::string gui_error;
    if (!gui.initialize(gui_error)) {
        if (nifm_ready) nifmExit();
        return 2;
    }

    AppConfig config;
    std::string config_error;
    const bool config_loaded = load_config(kConfigPath, config, config_error);

    astranas::netdiag::NetDiagUi app(
        gui, input, pad, std::move(config), config_loaded, std::move(config_error));
    app.run();
    gui.shutdown();
    if (nifm_ready) nifmExit();
    return 0;
}

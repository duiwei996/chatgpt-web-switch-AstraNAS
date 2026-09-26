// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include "config.hpp"
#include "network_diagnostics.hpp"
#include "network_runtime.hpp"
#include "ui/gui.hpp"
#include "ui/input.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace astranas::netdiag {

class NetDiagUi {
public:
    NetDiagUi(astranas::ui::Gui& gui,
              astranas::ui::InputRouter& input,
              PadState& pad,
              AppConfig config,
              bool config_loaded,
              std::string config_error);

    void run();

private:
    enum class State { Ready, Running, Success, Failed, Skipped };

    struct TestItem {
        const char* protocol = "smb";
        astranas::network::SocketProfile profile = astranas::network::SocketProfile::LibnxDefault;
        std::string title;
        State state = State::Ready;
        astranas::network::DiagnosticResult result;
        std::string message;
        std::string tuning_error;
        bool cpu_boost_enabled = false;
        bool wlan_priority_optimized = false;
    };

    struct BufferScanItem {
        std::string protocol;
        std::uint32_t requested = 0;
        State state = State::Ready;
        astranas::network::DiagnosticResult result;
        std::string message;
    };

    astranas::ui::Gui& gui_;
    astranas::ui::InputRouter& input_;
    PadState& pad_;
    AppConfig config_;
    bool config_loaded_ = false;
    std::string config_error_;
    std::string log_status_;
    astranas::network::NetworkLinkInfo link_info_{};
    std::array<TestItem, 4> tests_{};
    std::vector<BufferScanItem> buffer_scan_;
    std::string diagnostic_conclusion_;
    int selected_ = 0;
    int action_selected_ = 0;
    bool running_all_ = false;
    bool exit_requested_ = false;

    bool protocol_configured(const char* protocol) const;
    void reset_results();
    void draw();
    void draw_header();
    void draw_test_row(int index, int y);
    void draw_footer();
    std::string detail_text(const TestItem& item) const;
    static std::string localized_error(const std::string& error);
    void run_one(int index);
    void run_all();
    void run_buffer_scan();
    std::string build_diagnostic_conclusion() const;
    void save_results_log();
    void manual_upload_results_log();
    void activate_action();
    void open_config();
    void configure_log_directory();
    void handle_touch(const astranas::ui::InputFrame& frame);
    static std::string state_label(State state);
    static astranas::ui::Color state_color(State state);
};

} // namespace astranas::netdiag
#endif

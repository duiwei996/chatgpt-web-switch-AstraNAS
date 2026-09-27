// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include "runtime.hpp"
#include "../config.hpp"
#include "../local_fs.hpp"
#include "../model.hpp"
#include "remote_install_queue.hpp"
#include "../installed/catalog.hpp"
#include "../remote/remote_client.hpp"
#include "../ui/gui.hpp"
#include "../ui/input.hpp"
#include "../ui/pages.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <switch.h>
namespace astranas::app {
using astranas::ui::InputFrame;
using astranas::ui::Tab;
struct ActionContext;
class Runtime {
public:
    int execute();
private:
    PadState pad_{};
    astranas::ui::InputRouter input_{};
    astranas::ui::Gui gui_{};
    AppConfig config_{};
    std::unique_ptr<RemoteClient> remote_;
    bool exit_requested_=false;
    bool socket_ready_=false,curl_ready_=false,nifm_ready_=false,nifm_attempted_=false;
    bool ns_ready_=false,ns_attempted_=false,usb_ready_=false,usb_attempted_=false;
    bool first_run_=false,remote_attempted_=false,local_loaded_=false,cache_loaded_=false;
    int installed_stage_=0;
    std::string status_="就绪",remote_dir_="/",remote_state_endpoint_,remote_restore_selection_path_,local_root_="sdmc:/",local_dir_="sdmc:/";
    std::vector<RemoteDirEntry> remote_entries_;
    std::vector<RemoteInstallQueueEntry> install_queue_;
    std::vector<LocalEntry> local_entries_,cache_entries_;
    LocalEntry move_source_{};
    std::string move_source_root_;
    bool move_pending_=false;
    std::vector<std::string> local_selection_;
    std::vector<astranas::installed::TitleEntry> installed_entries_;
    std::map<std::uint64_t,astranas::installed::IconBitmap> installed_icons_;
    Tab tab_=Tab::Local;
    std::size_t remote_sel_=0,local_sel_=0,cache_sel_=0,installed_sel_=0,settings_sel_=0;
    double last_benchmark_=0.0;
    std::uint64_t log_scan_offset_=0;
    bool dirty_=true;
    void initialize_input();
    bool initialize_gui();
    void load_initial_config();
    bool ensure_nifm();
    bool ensure_network();
    bool ensure_ns();
    bool ensure_usb();
    void validate_local_state();
    void persist_local_state();
    void persist_remote_state();
    bool validate_remote_state();
    void restore_remote_selection();
    bool refresh_local();
    void refresh_cache();
    void load_installed_cache();
    void refresh_installed_live();
    void load_visible_icons();
    bool connect_remote(bool test_only = false);
    void refresh_remote();
    void disconnect_for_setting_change(const std::string& label);
    bool save_config_now(const std::string& label,bool network_changed);
    std::string remote_start_dir() const;
    void draw_current();
    bool run_deferred_page_load();
    void set_tab(Tab next);
    bool handle_tab_touch(const InputFrame& frame);
    void loop();
    void handle_remote(const InputFrame& frame,ActionContext& ctx);
    void handle_local(const InputFrame& frame,ActionContext& ctx);
    void handle_cache(const InputFrame& frame,ActionContext& ctx);
    void handle_installed(const InputFrame& frame);
    void handle_settings(const InputFrame& frame);
    void run_network_diagnostic();
    void maybe_save_install_log();
    void shutdown();
};
}
#endif

            out << "  |  RCVBUF " << (item.result.stats.socket_recv_buffer / 1024) << " KiB";
        if (item.result.stats.receive_callback_count)
            out << "  |  HTTP回调均值 " << (item.result.stats.average_callback_bytes / 1024) << " KiB";
        if (!item.tuning_error.empty()) out << "  |  " << item.tuning_error;
        return out.str();
    }
    if (!item.message.empty()) return item.message;
    return "上下选择测速项；左右选底部按钮，A 确认；测试数据只进入 RAM";
}

void NetDiagUi::draw_header() {
    gui_.fill_rect(0, 0, Gui::kWidth, 84, palette::navy);
    gui_.text(48, 18, 30, palette::white, kNetDiagTitle);
    gui_.text(48, 52, 15, Color{215, 224, 237, 255},
              std::string("v") + astranas::app::kVersion + "  |  " + applet_mode_text() +
              "  |  " + astranas::network::network_link_summary(link_info_) + "  |  纯 RAM 分层诊断");

    const std::string config_status = config_loaded_ ? "网络配置已读取" : "尚未保存网络配置";
    const Color badge = config_loaded_ ? palette::success_soft : palette::danger_soft;
    const Color badge_text = config_loaded_ ? palette::success : palette::danger;
    gui_.round_rect(986, 20, 240, 38, 10, badge);
    gui_.text(1006, 29, 16, badge_text, config_status, 205);
}

void NetDiagUi::draw_test_row(int index, int y) {
    const auto& item = tests_[static_cast<std::size_t>(index)];
    const bool selected = index == selected_;
    gui_.round_rect(kRowX, y, kRowW, kRowH, 12, selected ? palette::selected : palette::surface);
    gui_.stroke_rect(kRowX, y, kRowW, kRowH, 1, selected ? palette::accent : palette::border);

    gui_.round_rect(kRowX + 18, y + 18, 82, 28, 14,
                    std::string(item.protocol) == "smb" ? palette::accent_soft : palette::success_soft);
    gui_.text(kRowX + 34, y + 23, 15,
              std::string(item.protocol) == "smb" ? palette::accent : palette::success,
              std::string(item.protocol) == "smb" ? "SMB" : "WebDAV", 62);

    gui_.text(kRowX + 116, y + 14, 21, palette::text, item.title, 520);
    gui_.text(kRowX + 116, y + 48, 15, palette::muted, detail_text(item), 710);

    const Color status_color = state_color(item.state);
    gui_.text(948, y + 16, 16, status_color, state_label(item.state), 96);
    if (item.state == State::Success) {
        gui_.text(1028, y + 38, 25, palette::text, speed_text(item.result), 178);
    } else if (item.state == State::Running) {
        gui_.text(1028, y + 38, 18, palette::accent, "请保持网络连接", 178);
    }
}

void NetDiagUi::draw_footer() {
    draw_action_button(gui_, kStartButtonX, kStartButtonY, kStartButtonW, "开始选中测试", action_selected_ == 0);
    draw_action_button(gui_, kAllButtonX, kStartButtonY, kAllButtonW, "一键基础诊断", action_selected_ == 1);
    draw_action_button(gui_, kResetButtonX, kStartButtonY, kResetButtonW, "清空结果", action_selected_ == 2);
    draw_action_button(gui_, kConfigButtonX, kStartButtonY, kConfigButtonW, "网络配置", action_selected_ == 3);

    gui_.text(54, 646, 14, palette::muted, "配置文件", 74);
    gui_.text(132, 646, 14, palette::text, kConfigPath, 430);
    const auto& selected_test = tests_[static_cast<std::size_t>(selected_)];
    std::string hint = running_all_ ? (log_status_.empty() ? "正在执行基础诊断" : log_status_) :
        (selected_test.state == State::Success
            ? astranas::network::network_link_summary(link_info_) + " · " +
              profile_runtime_text(selected_test.profile, selected_test.wlan_priority_optimized,
                                   selected_test.cpu_boost_enabled)
            : "上下选择测速项 · 左右选择底部按钮 · 日志仅保存本地，可在网络配置手动上传");
    if (!log_status_.empty() && !running_all_) hint = log_status_ + " · " + hint;
    gui_.text(620, 646, 14, palette::muted, hint, 606);

    gui_.divider(54, 668, 1172);
    gui_.button_hint(54, 682, "←/→", "选择按钮");
    gui_.button_hint(278, 682, "A", "确认");
    gui_.button_hint(432, 682, "+", "退出", palette::danger);
}

void NetDiagUi::draw() {
    gui_.begin();
    gui_.clear(palette::background);
    draw_header();
    for (int i = 0; i < static_cast<int>(tests_.size()); ++i)
        draw_test_row(i, kRowsTop + i * (kRowH + kRowGap));
    draw_footer();
    gui_.end();
}

void NetDiagUi::run_one(int index) {
    auto& item = tests_[static_cast<std::size_t>(index)];
    log_status_.clear();
    if (!protocol_configured(item.protocol)) {
        item.state = State::Skipped;
        item.message = std::string(std::string(item.protocol) == "smb" ? "SMB" : "WebDAV") + " 尚未配置服务器";
        draw();
        if (!running_all_) save_results_log();
        return;
    }

    item.state = State::Running;
    item.message = "正在连接服务器、选择测速文件并读取最多 64 MiB 数据";
    item.result = {};
    item.tuning_error.clear();
    item.cpu_boost_enabled = false;
    item.wlan_priority_optimized = false;
    draw();

    std::string error;
    const bool optimized = item.profile == astranas::network::SocketProfile::HighThroughput;
    std::string tuning_error;
    if (!astranas::network::set_wireless_priority(optimized, tuning_error)) item.tuning_error = tuning_error;
    item.wlan_priority_optimized = astranas::network::wireless_priority_optimized();
    if (!astranas::network::initialize_socket(item.profile, error)) {
        item.state = State::Failed;
        item.message = localized_error(error);
        draw();
        if (!running_all_) save_results_log();
        return;
    }

    const CURLcode curl_rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_rc != CURLE_OK) {
        item.state = State::Failed;
        item.message = "curl 初始化失败";
        socketExit();
        draw();
        if (!running_all_) save_results_log();
        return;
    }

    bool success = false;
    {
        astranas::network::ScopedCpuBoost cpu_boost(optimized);
        item.cpu_boost_enabled = cpu_boost.enabled();
        AppConfig active = config_;
        set_active_protocol(active, item.protocol);
        auto remote = make_remote_client(active, error);
        if (!remote) item.message = "无法创建远程客户端：" + localized_error(error);
        else if (!remote->connect(active, error)) item.message = "连接失败：" + localized_error(error);
        else {
            const std::string start_dir = active.remote_dir.empty()
                ? (active.root.empty() ? "/" : active.root) : active.remote_dir;
            success = astranas::network::run_remote_diagnostic(
                *remote, start_dir, kBenchmarkBytes, item.result, error);
            if (!success) item.message = localized_error(error);
        }
    }

    curl_global_cleanup();
    socketExit();
    link_info_ = astranas::network::query_network_link();
    item.state = success ? State::Success : State::Failed;
    if (success) item.message.clear();
    draw();
    if (!running_all_) save_results_log();
}

void NetDiagUi::run_buffer_scan() {
    buffer_scan_.clear();
    buffer_scan_.reserve(kReceiveBufferSteps.size() * 2);
    for (const char* protocol : {"smb", "webdav"}) {
        for (const auto requested : kReceiveBufferSteps) {
            BufferScanItem scan;
            scan.protocol = protocol;
            scan.requested = requested;
            if (!protocol_configured(protocol)) {
                scan.state = State::Skipped;
                scan.message = "协议未配置";
                buffer_scan_.push_back(std::move(scan));
                continue;
            }
            scan.state = State::Running;
            log_status_ = "高级缓冲扫描 " + std::string(protocol == std::string("smb") ? "SMB · " : "WebDAV · ") + buffer_label(requested);
            draw();

            std::string error;
            std::string tuning_error;
            astranas::network::set_wireless_priority(true, tuning_error);
            if (!astranas::network::initialize_socket(astranas::network::SocketProfile::HighThroughput, error)) {
                scan.state = State::Failed;
                scan.message = localized_error(error);
                buffer_scan_.push_back(std::move(scan));
                continue;
            }
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                socketExit();
                scan.state = State::Failed;
                scan.message = "curl 初始化失败";
                buffer_scan_.push_back(std::move(scan));
                continue;
            }

            bool success = false;
            {
                astranas::network::ScopedCpuBoost cpu_boost;
                AppConfig active = config_;
                set_active_protocol(active, protocol);
                auto remote = make_remote_client(active, error);
                if (!remote) scan.message = "无法创建远程客户端：" + localized_error(error);
                else if (!remote->connect(active, error)) scan.message = "连接失败：" + localized_error(error);
                else {
                    remote->set_benchmark_socket_recv_buffer(requested);
                    const std::string start_dir = active.remote_dir.empty()
                        ? (active.root.empty() ? "/" : active.root) : active.remote_dir;

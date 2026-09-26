        return;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        socketExit();
        log_status_ = "测速日志手动上传失败，已保留本地";
        draw();
        return;
    }

    bool uploaded = false;
    std::string uploaded_path;
    {
        astranas::network::ScopedCpuBoost cpu_boost;
        AppConfig active = config_;
        activate_remote_profile(active);
        normalize_config_endpoint(active);
        auto remote = make_remote_client(active, error);
        if (remote && remote->connect(active, error)) {
            uploaded = astranas::log_upload::upload_log(
                *remote, active, gui_, input_, pad_, exit_requested_,
                kNetDiagLogPath, "AstraNAS-NetDiag", "选择测速日志保存目录",
                uploaded_path, error);
        } else if (remote && error.empty()) {
            error = "连接日志上传服务器失败";
        }
    }

    curl_global_cleanup();
    socketExit();
    if (uploaded) log_status_ = "测速日志已手动上传：" + uploaded_path;
    else if (error == "cancelled") log_status_ = "测速日志上传已取消；本地日志已保留";
    else log_status_ = "测速日志手动上传失败；本地日志已保留：" + localized_error(error);
    draw();
}

void NetDiagUi::configure_log_directory() {
    AppConfig active = config_;
    activate_remote_profile(active);
    normalize_config_endpoint(active);
    const std::string current = astranas::log_upload::configured_directory(
        active, astranas::log_upload::LogDestinationKind::NetDiag);
    const std::vector<std::string> actions = {"选择 / 重新选择目录", "清空目录", "返回"};
    const int choice = astranas::ui::choose_action(
        gui_, pad_, input_, exit_requested_, "测速日志上传目录",
        current.empty() ? "当前：未设置。只有手动选择“上传最近测速日志”时才会按需弹出目录选择器。"
                        : "当前：" + current,
        actions, kNetDiagTitle);
    if (exit_requested_ || choice < 0 || choice == 2) return;
    if (choice == 1) {
        astranas::log_upload::clear_configured_directory(astranas::log_upload::LogDestinationKind::NetDiag);
        log_status_ = "测速日志上传目录已清空";
        return;
    }

    std::string error, tuning_error, selected;
    astranas::network::set_wireless_priority(true, tuning_error);
    if (!astranas::network::initialize_socket(astranas::network::SocketProfile::HighThroughput, error)) {
        log_status_ = "无法打开目录：" + localized_error(error);
        return;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        socketExit();
        log_status_ = "curl 初始化失败";
        return;
    }
    bool saved = false;
    {
        astranas::network::ScopedCpuBoost boost;
        auto remote = make_remote_client(active, error);
        if (remote && remote->connect(active, error)) {
            saved = astranas::log_upload::configure_directory(
                *remote, active, astranas::log_upload::LogDestinationKind::NetDiag,
                gui_, input_, pad_, exit_requested_, "选择测速日志上传目录", selected, error);
        }
    }
    curl_global_cleanup();
    socketExit();
    if (saved) log_status_ = "测速日志上传目录已保存";
    else if (error == "cancelled") log_status_ = "未修改测速日志上传目录";
    else log_status_ = "目录设置失败：" + localized_error(error);
}

void NetDiagUi::open_config() {
    const std::vector<std::string> actions = {
        "选择协议", "SMB 配置", "WebDAV 配置", "上传最近测速日志",
        "测速日志上传目录", "高级缓冲扫描", "返回测速"
    };
    while (appletMainLoop() && !exit_requested_) {
        const int action = astranas::ui::choose_action(
            gui_, pad_, input_, exit_requested_, "网络配置",
            "普通一键诊断只跑 4 个基础项目；日志只保存本地，上传和 SO_RCVBUF 高级扫描都必须手动执行。",
            actions, kNetDiagTitle);
        if (exit_requested_ || action < 0 || action == 6) break;
        if (action == 3) {
            manual_upload_results_log();
            continue;
        }
        if (action == 4) {
            configure_log_directory();
            continue;
        }
        if (action == 5) {
            if (!astranas::ui::confirm_action(gui_, pad_, exit_requested_, "高级缓冲扫描",
                    "将依次运行 SMB/WebDAV 的 256 KiB、512 KiB、1 MiB、2 MiB、4 MiB SO_RCVBUF 档位。\n该测试耗时较长，普通诊断无需运行。",
                    "开始扫描", "取消")) {
                if (!exit_requested_) log_status_ = "高级缓冲扫描已取消";
                continue;
            }
            running_all_ = true;
            log_status_ = "准备高级缓冲扫描";
            draw();
            run_buffer_scan();
            diagnostic_conclusion_ = build_diagnostic_conclusion();
            running_all_ = false;
            save_results_log();
            draw();
            continue;
        }

        bool changed = false;
        if (action == 0) {
            std::string selected;
            if (astranas::ui::choose_protocol(gui_, pad_, input_, exit_requested_, config_.protocol, selected)) {
                if (selected != config_.protocol) set_active_protocol(config_, selected);
                changed = true;
            }
        } else if (action == 1) {
            SmbProfileConfig edited;
            if (astranas::ui::edit_smb_profile(gui_, pad_, input_, exit_requested_, config_.smb, edited)) {
                config_.smb = edited;
                if (config_.protocol == "smb") activate_remote_profile(config_);
                changed = true;
            }
        } else if (action == 2) {
            WebDavProfileConfig edited;
            if (astranas::ui::edit_webdav_profile(gui_, pad_, input_, exit_requested_, config_.webdav, edited)) {
                config_.webdav = edited;
                if (config_.protocol == "webdav") activate_remote_profile(config_);
                changed = true;
            }
        }

        if (exit_requested_) break;
        if (!changed) continue;
        normalize_config_endpoint(config_);
        std::string error;
        if (!save_config(kConfigPath, config_, error)) {
            astranas::ui::show_message_wait(
                gui_, pad_, exit_requested_, "保存配置失败", localized_error(error), kNetDiagTitle);
            continue;
        }
        config_loaded_ = true;
        config_error_.clear();
        reset_results();
        astranas::ui::show_message_wait(
            gui_, pad_, exit_requested_, "网络配置",
            "配置已保存，可以开始纯 RAM 网络测速。\n\n普通一键诊断只运行四项基础测试；高级缓冲扫描和日志上传都按需手动启动。\n" +
                std::string(kConfigPath),
            kNetDiagTitle);
    }
}

void NetDiagUi::handle_touch(const astranas::ui::InputFrame& frame) {
    if (!frame.touch_tap || running_all_) return;
    for (int index = 0; index < static_cast<int>(tests_.size()); ++index) {
        const int y = kRowsTop + index * (kRowH + kRowGap);
        if (point_in_rect(frame.touch_x, frame.touch_y, kRowX, y, kRowW, kRowH)) {
            selected_ = index;
            return;
        }
    }
    if (point_in_rect(frame.touch_x, frame.touch_y, kStartButtonX, kStartButtonY, kStartButtonW, kButtonH)) {
        action_selected_ = 0;
        activate_action();
    } else if (point_in_rect(frame.touch_x, frame.touch_y, kAllButtonX, kStartButtonY, kAllButtonW, kButtonH)) {
        action_selected_ = 1;
        activate_action();
    } else if (point_in_rect(frame.touch_x, frame.touch_y, kResetButtonX, kStartButtonY, kResetButtonW, kButtonH)) {
        action_selected_ = 2;
        activate_action();
    } else if (point_in_rect(frame.touch_x, frame.touch_y, kConfigButtonX, kStartButtonY, kConfigButtonW, kButtonH)) {
        action_selected_ = 3;
        activate_action();
    }
}

void NetDiagUi::activate_action() {
    switch (action_selected_) {
        case 0: run_one(selected_); break;
        case 1: run_all(); break;
        case 2: reset_results(); break;
        case 3: open_config(); break;
        default: action_selected_ = 0; break;
    }
}

void NetDiagUi::run() {
    draw();
    if (!config_loaded_) {
        open_config();
        if (exit_requested_) return;
        draw();
    }
    while (appletMainLoop() && !exit_requested_) {
        const auto frame = input_.update(pad_);
        if (frame.down & HidNpadButton_Plus) { exit_requested_ = true; break; }
        if (!running_all_) {
            if (frame.nav & HidNpadButton_Up) selected_ = std::max(0, selected_ - 1);
            if (frame.nav & HidNpadButton_Down) selected_ = std::min(static_cast<int>(tests_.size()) - 1, selected_ + 1);
            if (frame.nav_once & HidNpadButton_Left) action_selected_ = (action_selected_ + 3) % 4;
            if (frame.nav_once & HidNpadButton_Right) action_selected_ = (action_selected_ + 1) % 4;
            if (frame.down & HidNpadButton_A) activate_action();
            handle_touch(frame);
        }
        draw();
        svcSleepThread(10'000'000);
    }
}

} // namespace astranas::netdiag

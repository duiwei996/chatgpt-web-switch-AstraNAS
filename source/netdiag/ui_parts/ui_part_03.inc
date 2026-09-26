                    success = astranas::network::run_remote_diagnostic(
                        *remote, start_dir, kBenchmarkBytes, scan.result, error);
                    if (success) scan.result.stats.requested_socket_recv_buffer = requested;
                    else scan.message = localized_error(error);
                }
            }
            curl_global_cleanup();
            socketExit();
            scan.state = success ? State::Success : State::Failed;
            buffer_scan_.push_back(std::move(scan));
        }
    }
    log_status_ = "高级缓冲扫描完成";
    link_info_ = astranas::network::query_network_link();
}

std::string NetDiagUi::build_diagnostic_conclusion() const {
    bool buffer_sensitive = false;
    std::ostringstream details;
    for (const char* protocol : {"smb", "webdav"}) {
        double min_speed = 0.0, max_speed = 0.0;
        bool have = false;
        for (const auto& scan : buffer_scan_) {
            if (scan.protocol != protocol || scan.state != State::Success) continue;
            const double speed = scan.result.stats.mib_per_sec;
            if (!have) { min_speed = max_speed = speed; have = true; }
            else { min_speed = std::min(min_speed, speed); max_speed = std::max(max_speed, speed); }
        }
        if (have && min_speed > 0.0 && max_speed > min_speed * 1.25) {
            buffer_sensitive = true;
            details << (std::string(protocol) == "smb" ? "SMB" : "WebDAV")
                    << " 对 SO_RCVBUF 档位敏感；";
        }
    }

    std::ostringstream out;
    if (buffer_sensitive) {
        out << "网络层/socket receive buffer 对吞吐有明显影响；" << details.str();
    } else if (tests_[2].state == State::Success && tests_[3].state == State::Success) {
        const double smb = tests_[2].result.stats.mib_per_sec;
        const double dav = tests_[3].result.stats.mib_per_sec;
        const double high = std::max(smb, dav);
        const double low = std::min(smb, dav);
        if (high > 0.0 && low / high >= 0.75 && high < 18.0)
            out << "SMB/WebDAV 同时落在相近低吞吐平台，优先怀疑 Switch WLAN/BSD/网络层共同平台；";
        else if (high > 0.0 && low / high < 0.65)
            out << "SMB/WebDAV 差异明显，优先检查较慢协议的协议层/服务端实现；";
        else
            out << "基础协议吞吐未显示单一明显平台；";
    } else {
        out << "协议层测速证据不足；";
    }

    if (buffer_scan_.empty()) out << "高级 SO_RCVBUF 扫描未运行；";

    const InstallMetrics metrics = read_install_metrics();
    if (!metrics.available || metrics.source_mib <= 0.0 || metrics.ncm_mib <= 0.0) {
        out << "最近安装日志不可用或指标不足，安装层结论为证据不足。";
    } else if (metrics.source_mib < metrics.ncm_mib * 0.70 ||
               (metrics.wait_ms > 2000.0 && metrics.backpressure_ms < metrics.wait_ms * 0.10)) {
        out << "最近安装显示源读取/等待明显慢于 NCM，属于网络/源供给瓶颈，不是 SD/NCM 写入瓶颈。";
    } else if (metrics.ncm_mib < metrics.source_mib * 0.75 || metrics.backpressure_ms > 2000.0) {
        out << "最近安装显示 NCM/SD 写入或回压限制吞吐，优先检查写入侧。";
    } else {
        out << "最近安装的源读取与 NCM 写入接近，暂未形成单一安装层瓶颈。";
    }
    return out.str();
}

void NetDiagUi::run_all() {
    log_status_.clear();
    buffer_scan_.clear();
    running_all_ = true;
    draw();
    for (int index = 0; index < static_cast<int>(tests_.size()); ++index) {
        selected_ = index;
        run_one(index);
    }
    diagnostic_conclusion_ = build_diagnostic_conclusion();
    running_all_ = false;
    log_status_ = "基础分层诊断完成；高级缓冲扫描未运行";
    draw();
    save_results_log();
}

void NetDiagUi::save_results_log() {
    std::ostringstream report;
    report.setf(std::ios::fixed);
    report.precision(2);
    report << "Switch网络测速分层诊断日志\n"
           << "版本: " << astranas::app::kVersion << "\n"
           << "运行模式: " << applet_mode_text() << "\n"
           << "链路: " << astranas::network::network_link_summary(link_info_) << "\n";
    if (link_info_.available && link_info_.wifi)
        report << "Switch 可读 Wi-Fi 强度: " << link_info_.wifi_strength << "/3\n";
    else
        report << "Switch 可读 Wi-Fi 强度: 不适用/不可用\n";
    report << "PHY Rate: 需在 AP 管理页确认\n"
           << "MCS: 需在 AP 管理页确认\n"
           << "信道带宽(20/40/80 MHz): 需在 AP 管理页确认\n"
           << "重传率: 需在 AP 管理页确认\n"
           << "日志上传协议: " << config_.protocol << "\n"
           << "上传策略: 仅手动上传\n\n";

    for (std::size_t i = 0; i < tests_.size(); ++i) {
        const auto& item = tests_[i];
        const auto profile = astranas::network::socket_profile_info(item.profile);
        report << "[基础 " << (i + 1) << "] " << item.title << "\n"
               << "状态: " << state_label(item.state) << "\n"
               << "协议: " << item.protocol << "\n"
               << "Socket profile: " << profile.name << "\n"
               << "TCP TX initial/max: " << profile.tcp_tx_initial << "/" << profile.tcp_tx_max << " bytes\n"
               << "TCP RX initial/max: " << profile.tcp_rx_initial << "/" << profile.tcp_rx_max << " bytes\n"
               << "sb_efficiency: " << profile.sb_efficiency << "\n"
               << "BSD sessions: " << profile.bsd_sessions << "\n"
               << "WLAN Priority: " << (item.wlan_priority_optimized ? "OptimizedForWlan" : "Default") << "\n"
               << "CPU Boost: " << (item.cpu_boost_enabled ? "FastLoad" : "Normal/未启用") << "\n";
        if (!item.tuning_error.empty()) report << "调优错误: " << item.tuning_error << "\n";
        if (item.state == State::Success) {
            const auto& stats = item.result.stats;
            report << "测速文件: " << item.result.candidate.path << "\n"
                   << "测速字节: " << stats.bytes << "\n"
                   << "耗时秒: " << stats.seconds << "\n"
                   << "吞吐 MiB/s: " << stats.mib_per_sec << "\n"
                   << "请求次数: " << stats.request_count << "\n"
                   << "平均请求字节: " << stats.average_request_bytes << "\n"
                   << "SMB 协商读大小: " << stats.negotiated_read_size << "\n"
                   << "流水线请求大小: " << stats.pipeline_request_size << "\n"
                   << "流水线窗口: " << stats.pipeline_window << "\n"
                   << "实际 SO_RCVBUF: " << stats.socket_recv_buffer << "\n"
                   << "HTTP 接收回调次数: " << stats.receive_callback_count << "\n"
                   << "HTTP 平均回调字节: " << stats.average_callback_bytes << "\n"
                   << "诊断提示: " << astranas::network::diagnostic_hint(item.protocol, item.result) << "\n";
        } else if (!item.message.empty()) report << "错误/说明: " << item.message << "\n";
        report << "\n";
    }

    report << "[Socket receive buffer 高级档位扫描]\n";
    if (buffer_scan_.empty()) report << "未执行；默认一键基础诊断不会运行，请在“网络配置 -> 高级缓冲扫描”中手动启动。\n";
    for (const auto& scan : buffer_scan_) {
        report << "协议=" << scan.protocol
               << " 申请SO_RCVBUF=" << scan.requested
               << " 状态=" << state_label(scan.state);
        if (scan.state == State::Success) {
            const auto& stats = scan.result.stats;
            report << " 实际SO_RCVBUF=" << stats.socket_recv_buffer
                   << " 吞吐MiB/s=" << stats.mib_per_sec
                   << " 请求数=" << stats.request_count
                   << " 平均请求字节=" << stats.average_request_bytes
                   << " 回调数=" << stats.receive_callback_count
                   << " 平均回调字节=" << stats.average_callback_bytes;
        } else if (!scan.message.empty()) report << " 说明=" << scan.message;
        report << "\n";
    }

    const InstallMetrics metrics = read_install_metrics();
    report << "\n[最近安装日志]\n";
    if (metrics.available) {
        report << "源读取MiB/s: " << metrics.source_mib << "\n"
               << "NCM写入MiB/s: " << metrics.ncm_mib << "\n"
               << "等待源数据_ms: " << metrics.wait_ms << "\n"
               << "源回压_ms: " << metrics.backpressure_ms << "\n";
    } else {
        report << "未找到可解析的 install-latest.log；安装层标记为证据不足。\n";
    }
    report << "\n[自动结论]\n"
           << (diagnostic_conclusion_.empty() ? build_diagnostic_conclusion() : diagnostic_conclusion_)
           << "\n";

    std::string error;
    if (!astranas::log_upload::write_text_file(kNetDiagLogPath, report.str(), error)) {
        log_status_ = "测速日志生成失败";
        draw();
        return;
    }
    log_status_ = "测速日志已保存本地；需在网络配置中手动上传";
    draw();
}

void NetDiagUi::manual_upload_results_log() {
    const bool have_result = std::any_of(tests_.begin(), tests_.end(), [](const TestItem& item) {
        return item.state == State::Success || item.state == State::Failed || item.state == State::Skipped;
    }) || !buffer_scan_.empty();
    if (!have_result) {
        log_status_ = "暂无测速结果；请先完成至少一项测试";
        draw();
        return;
    }

    save_results_log();
    if (!config_loaded_ || config_.server.empty() || exit_requested_) {
        log_status_ = "测速日志已保留本地；当前协议未配置，无法手动上传";
        draw();
        return;
    }

    std::string error, tuning_error;
    astranas::network::set_wireless_priority(true, tuning_error);
    if (!astranas::network::initialize_socket(astranas::network::SocketProfile::HighThroughput, error)) {
        log_status_ = "测速日志手动上传失败，已保留本地";
        draw();

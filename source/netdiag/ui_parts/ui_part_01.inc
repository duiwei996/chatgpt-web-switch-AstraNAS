namespace astranas::netdiag {
namespace {
using astranas::ui::Color;
using astranas::ui::Gui;
namespace palette = astranas::ui::palette;

constexpr const char* kConfigPath = "sdmc:/switch/AstraNAS/config.ini";
constexpr const char* kNetDiagLogPath = "sdmc:/switch/AstraNAS/netdiag-latest.log";
constexpr const char* kInstallLogPath = "sdmc:/switch/AstraNAS/install-latest.log";
constexpr const char* kNetDiagTitle = "Switch网络测速";
constexpr std::size_t kBenchmarkBytes = 64u * 1024u * 1024u;
constexpr std::array<std::uint32_t, 5> kReceiveBufferSteps = {
    256u * 1024u, 512u * 1024u, 1024u * 1024u, 2u * 1024u * 1024u, 4u * 1024u * 1024u
};
constexpr int kRowX = 54;
constexpr int kRowW = 1172;
constexpr int kRowH = 92;
constexpr int kRowGap = 10;
constexpr int kRowsTop = 118;
constexpr int kStartButtonX = 54;
constexpr int kStartButtonY = 588;
constexpr int kStartButtonW = 252;
constexpr int kButtonH = 52;
constexpr int kAllButtonX = 324;
constexpr int kAllButtonW = 252;
constexpr int kResetButtonX = 594;
constexpr int kResetButtonW = 220;
constexpr int kConfigButtonX = 832;
constexpr int kConfigButtonW = 250;

struct InstallMetrics {
    bool available = false;
    double source_mib = 0.0;
    double ncm_mib = 0.0;
    double wait_ms = 0.0;
    double backpressure_ms = 0.0;
};

bool point_in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

std::string speed_text(const astranas::network::DiagnosticResult& result) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << result.stats.mib_per_sec << " MiB/s";
    return out.str();
}

std::string applet_mode_text() {
    const auto type = appletGetAppletType();
    return (type == AppletType_Application || type == AppletType_SystemApplication)
        ? "Application" : "Applet";
}

std::string profile_runtime_text(astranas::network::SocketProfile profile,
                                 bool wlan_optimized, bool cpu_boost) {
    const auto info = astranas::network::socket_profile_info(profile);
    std::ostringstream out;
    out << "TCP " << (info.tcp_tx_initial / 1024) << "/" << (info.tcp_rx_initial / 1024)
        << "→" << (info.tcp_tx_max / 1024) << "/" << (info.tcp_rx_max / 1024) << " KiB"
        << " · sb" << info.sb_efficiency
        << (wlan_optimized ? " · WLAN优先" : " · WLAN默认")
        << (cpu_boost ? " · CPU FastLoad" : " · CPU正常");
    return out.str();
}

void draw_action_button(Gui& gui, int x, int y, int w, const std::string& label, bool selected) {
    gui.round_rect(x, y, w, kButtonH, 12, selected ? palette::accent : palette::surface);
    gui.stroke_rect(x, y, w, kButtonH, selected ? 2 : 1, selected ? palette::accent : palette::border);
    const Color text_color = selected ? palette::white : palette::text;
    const int tw = gui.text_width(20, label);
    gui.text(x + std::max(16, (w - tw) / 2), y + 13, 20, text_color, label, w - 24);
}

bool parse_metric(const std::string& text, const char* key, double& value) {
    const auto pos = text.find(key);
    if (pos == std::string::npos) return false;
    const char* begin = text.c_str() + pos + std::char_traits<char>::length(key);
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

InstallMetrics read_install_metrics() {
    InstallMetrics metrics;
    std::ifstream in(kInstallLogPath, std::ios::binary);
    if (!in) return metrics;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    const bool source = parse_metric(text, "源读取=", metrics.source_mib);
    const bool ncm = parse_metric(text, "NCM写入=", metrics.ncm_mib);
    parse_metric(text, "等待源数据_ms=", metrics.wait_ms);
    parse_metric(text, "源回压_ms=", metrics.backpressure_ms);
    metrics.available = source || ncm;
    return metrics;
}

std::string buffer_label(std::uint32_t bytes) {
    if (bytes >= 1024u * 1024u && bytes % (1024u * 1024u) == 0)
        return std::to_string(bytes / (1024u * 1024u)) + " MiB";
    return std::to_string(bytes / 1024u) + " KiB";
}
}

NetDiagUi::NetDiagUi(Gui& gui,
                     astranas::ui::InputRouter& input,
                     PadState& pad,
                     AppConfig config,
                     bool config_loaded,
                     std::string config_error)
    : gui_(gui), input_(input), pad_(pad), config_(std::move(config)),
      config_loaded_(config_loaded), config_error_(std::move(config_error)) {
    link_info_ = astranas::network::query_network_link();
    tests_[0] = {"smb", astranas::network::SocketProfile::LibnxDefault, "SMB  /  libnx 默认", State::Ready, {}, {}};
    tests_[1] = {"webdav", astranas::network::SocketProfile::LibnxDefault, "WebDAV · HTTP 连续 Range  /  libnx 默认", State::Ready, {}, {}};
    tests_[2] = {"smb", astranas::network::SocketProfile::HighThroughput, "SMB  /  AstraNAS 高吞吐", State::Ready, {}, {}};
    tests_[3] = {"webdav", astranas::network::SocketProfile::HighThroughput, "WebDAV · HTTP 连续 Range  /  AstraNAS 高吞吐", State::Ready, {}, {}};
    reset_results();
}

bool NetDiagUi::protocol_configured(const char* protocol) const {
    if (!config_loaded_) return false;
    if (std::string(protocol) == "smb") return !config_.smb.server.empty();
    return !config_.webdav.server.empty();
}

std::string NetDiagUi::state_label(State state) {
    switch (state) {
        case State::Running: return "测速中";
        case State::Success: return "完成";
        case State::Failed: return "失败";
        case State::Skipped: return "未配置";
        case State::Ready: default: return "待测试";
    }
}

Color NetDiagUi::state_color(State state) {
    switch (state) {
        case State::Running: return palette::accent;
        case State::Success: return palette::success;
        case State::Failed: return palette::danger;
        case State::Skipped: return palette::warning;
        case State::Ready: default: return palette::muted;
    }
}

void NetDiagUi::reset_results() {
    log_status_.clear();
    buffer_scan_.clear();
    diagnostic_conclusion_.clear();
    for (auto& item : tests_) {
        item.result = {};
        item.message.clear();
        item.tuning_error.clear();
        item.cpu_boost_enabled = false;
        item.wlan_priority_optimized = false;
        if (!config_loaded_) {
            item.state = State::Failed;
            item.message = "无法读取主程序配置：" + config_error_;
        } else if (!protocol_configured(item.protocol)) {
            item.state = State::Skipped;
            item.message = std::string(std::string(item.protocol) == "smb" ? "SMB" : "WebDAV") + " 尚未配置服务器";
        } else {
            item.state = State::Ready;
        }
    }
}

std::string NetDiagUi::localized_error(const std::string& error) {
    if (error.empty()) return "未知错误";
    if (error == "SMB server address is empty") return "SMB 服务器地址为空";
    if (error == "smb2_init_context failed") return "SMB 客户端初始化失败";
    if (error == "SMB not connected") return "SMB 尚未连接";
    if (error == "invalid SMB share name") return "SMB 共享名无效";
    if (error == "remote directory contains too many entries") return "远程目录项目过多";
    if (error == "remote file not found while determining size") return "无法确定远程文件大小";
    if (error == "empty file") return "测速文件为空";
    if (error == "benchmark timer/read error") return "SMB 测速读取或计时失败";
    if (error == "empty URL") return "WebDAV 地址为空";
    if (error == "curl_easy_init failed") return "WebDAV 客户端初始化失败";
    if (error == "plain HTTP directory listing is not supported; provide library.json")
        return "普通 HTTP 不支持目录浏览，请使用 WebDAV 或提供 library.json";
    if (error == "remote file changed since it was listed") return "远程文件在测速前已变化";
    if (error == "benchmark size is zero") return "测速数据大小为 0";
    if (error == "benchmark read failed") return "网络测速读取失败";
    if (error == "benchmark timer error") return "网络测速计时失败";
    if (error == "cancelled") return "已取消";
    if (error == "unsupported protocol; choose SMB or WebDAV") return "协议配置无效，请选择 SMB 或 WebDAV";
    return error;
}

std::string NetDiagUi::detail_text(const TestItem& item) const {
    if (item.state == State::Success) {
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(1);
        out << item.result.candidate.name
            << "  |  " << (item.result.stats.bytes / 1024.0 / 1024.0) << " MiB"
            << "  |  " << item.result.stats.request_count << " 次请求";
        if (item.result.stats.pipeline_request_size && item.result.stats.pipeline_window > 1) {
            out << "  |  " << (item.result.stats.pipeline_request_size / 1024.0)
                << " KiB × " << item.result.stats.pipeline_window;
        } else if (std::string(item.protocol) == "webdav") {
            out << "  |  HTTP 单 Range 连续流";
        }
        if (item.result.stats.socket_recv_buffer)

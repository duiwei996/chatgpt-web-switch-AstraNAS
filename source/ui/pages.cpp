// SPDX-License-Identifier: GPL-3.0-or-later
#include "pages.hpp"
#ifdef __SWITCH__
#include "../app/actions.hpp"
#include "../app/constants.hpp"
#include "../app/util.hpp"
#include <algorithm>
#include <string>

namespace astranas::ui {
namespace palette = astranas::ui::palette;
using astranas::app::detect_local_install_candidate;
using astranas::app::format_size;
using astranas::app::format_speed;
using astranas::app::title_id_text;

const char* tab_text(Tab tab) {
    switch (tab) {
        case Tab::Remote: return "NAS 文件";
        case Tab::Local: return "本机文件";
        case Tab::Cache: return "安装缓存";
        case Tab::Installed: return "已安装";
        case Tab::Settings: return "设置";
    }
    return "未知";
}

Tab cycle_tab(Tab tab, int delta, bool show_cache) {
    const Tab visible_with_cache[] = {Tab::Remote, Tab::Local, Tab::Cache, Tab::Installed, Tab::Settings};
    const Tab visible_direct[] = {Tab::Remote, Tab::Local, Tab::Installed, Tab::Settings};
    const Tab* list = show_cache ? visible_with_cache : visible_direct;
    const int count = show_cache ? 5 : 4;
    int index = 0;
    for (int i = 0; i < count; ++i) if (list[i] == tab) { index = i; break; }
    index = (index + delta + count) % count;
    return list[index];
}

namespace {
bool status_contains(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }
Color status_bg(const std::string& s) {
    if (status_contains(s,"失败") || status_contains(s,"错误") || status_contains(s,"无法")) return palette::danger_soft;
    if (status_contains(s,"取消") || status_contains(s,"警告")) return palette::warning_soft;
    if (status_contains(s,"成功") || status_contains(s,"完成") || status_contains(s,"已连接") || status_contains(s,"已保存")) return palette::success_soft;
    return palette::accent_soft;
}
Color status_fg(const std::string& s) {
    if (status_contains(s,"失败") || status_contains(s,"错误") || status_contains(s,"无法")) return palette::danger;
    if (status_contains(s,"取消") || status_contains(s,"警告")) return palette::warning;
    if (status_contains(s,"成功") || status_contains(s,"完成") || status_contains(s,"已连接") || status_contains(s,"已保存")) return palette::success;
    return palette::accent;
}
void draw_shell(Gui& gui, Tab tab, const std::string& protocol, const std::string& status, bool show_cache) {
    gui.clear(palette::background);
    gui.fill_rect(0,0,Gui::kWidth,68,palette::navy);
    const std::string product = astranas::app::kDisplayName;
    gui.text(40,12,29,palette::white,product,360);
    gui.text(58+gui.text_width(29,product),20,18,palette::white,std::string("v")+astranas::app::kVersion);
    gui.text(750,10,17,palette::white,"L/R 切页   - Wi-Fi 帮助   + 正常退出");
    const std::string p = protocol.empty() ? "离线" : protocol;
    const int pw = std::max(92,gui.text_width(17,p)+28);
    gui.round_rect(Gui::kWidth-40-pw,35,pw,26,13,Color{255,255,255,42});
    gui.text(Gui::kWidth-40-pw+14,38,16,palette::white,p);
    const Tab with_cache[] = {Tab::Remote,Tab::Local,Tab::Cache,Tab::Installed,Tab::Settings};
    const Tab direct[] = {Tab::Remote,Tab::Local,Tab::Installed,Tab::Settings};
    const Tab* tabs = show_cache ? with_cache : direct;
    const int count = show_cache ? 5 : 4;
    const int gap = 10;
    const int width = (1200-gap*(count-1))/count;
    for (int i=0;i<count;++i) {
        const Tab t=tabs[i]; const int x=40+i*(width+gap); const bool active=t==tab;
        gui.round_rect(x,80,width,44,10,active?palette::accent:palette::surface);
        if(!active) gui.stroke_rect(x,80,width,44,1,palette::border);
        const std::string l=tab_text(t);
        gui.text(x+(width-gui.text_width(18,l))/2,90,18,active?palette::white:palette::text,l);
    }
    gui.round_rect(40,138,1200,46,10,status_bg(status));
    gui.fill_rect(40,138,5,46,status_fg(status));
    gui.text(58,149,18,status_fg(status),status.empty()?"就绪":status,1160);
}
void panel(Gui& gui,int x,int y,int w,int h,const std::string& title) {
    gui.round_rect(x,y,w,h,12,palette::surface);
    gui.stroke_rect(x,y,w,h,1,palette::border);
    gui.text(x+18,y+13,20,palette::text,title,w-36);
    gui.divider(x+18,y+48,w-36);
}
std::size_t begin_for(std::size_t selected,std::size_t count,std::size_t rows) {
    if(count<=rows||selected<rows) return 0;
    return std::min(selected-rows+1,count-rows);
}
const char* candidate_label(InstallCandidateKind kind) {
    switch(kind) {
        case InstallCandidateKind::HomebrewNro: return "NRO";
        case InstallCandidateKind::NintendoPackageNeedsBackend: return "安装包";
        default: return "文件";
    }
}
void badge(Gui& gui,int x,int y,const std::string& label,bool folder,bool active) {
    const Color bg=active?palette::accent:(folder?palette::folder:palette::file);
    const int w=std::max(54,gui.text_width(14,label)+18);
    gui.round_rect(x,y,w,25,12,bg);
    gui.text(x+9,y+4,14,palette::white,label,w-18);
}
std::size_t remote_queue_position(const std::vector<astranas::app::RemoteInstallQueueEntry>& queue,const std::string& path) {
    const auto it=std::find_if(queue.begin(),queue.end(),[&](const auto& item){return item.entry.path==path;});
    return it==queue.end()?0:static_cast<std::size_t>(std::distance(queue.begin(),it))+1;
}
std::size_t local_selection_position(const std::vector<std::string>& selection,const std::string& path) {
    const auto it=std::find(selection.begin(),selection.end(),path);
    return it==selection.end()?0:static_cast<std::size_t>(std::distance(selection.begin(),it))+1;
}
void selection_box(Gui& gui,int x,int y,bool installable,bool selected) {
    if (!installable) return;
    gui.round_rect(x,y,28,28,6,selected?palette::success:palette::surface_alt);
    gui.stroke_rect(x,y,28,28,1,selected?palette::success:palette::border);
    if (selected) gui.text(x+8,y+5,15,palette::white,"X");
}
void queue_preview(Gui& gui,const std::vector<std::string>& labels,int x,int y,int width) {
    if(labels.empty()) { gui.text(x,y,16,palette::muted,"尚未勾选安装包",width); return; }
    const std::size_t limit=std::min<std::size_t>(4,labels.size());
    for(std::size_t i=0;i<limit;++i)
        gui.text(x,y+static_cast<int>(i)*27,15,palette::text,"#"+std::to_string(i+1)+"  "+labels[i],width);
    if(labels.size()>limit) gui.text(x,y+static_cast<int>(limit)*27,14,palette::muted,"… 另有 "+std::to_string(labels.size()-limit)+" 项",width);
}
} // namespace

void draw_remote(Gui& gui,const std::vector<RemoteDirEntry>& entries,std::size_t selected,const std::string& current,
                 const std::vector<astranas::app::RemoteInstallQueueEntry>& queue,const std::string& protocol,const std::string& status,bool show_cache) {
    gui.begin(); draw_shell(gui,Tab::Remote,protocol,status,show_cache);
    panel(gui,40,198,830,430,"NAS 文件浏览");
    gui.text(58,252,16,palette::muted,"当前位置："+current,790);
    const auto begin=begin_for(selected,entries.size(),8),end=std::min(entries.size(),begin+8);
    if(entries.empty()) gui.text(58,315,19,palette::muted,"当前文件夹为空");
    for(std::size_t i=begin;i<end;++i) {
        const int y=285+static_cast<int>(i-begin)*42; const auto& e=entries[i]; const bool active=i==selected;
        if(active) gui.round_rect(52,y,806,38,8,palette::selected);
        const auto kind=detect_install_candidate(e.path); const bool installable=!e.is_dir&&kind!=InstallCandidateKind::None;
        const auto order=remote_queue_position(queue,e.path);
        selection_box(gui,64,y+5,installable,order>0);
        badge(gui,106,y+7,kind!=InstallCandidateKind::None?candidate_label(kind):(e.is_dir?"文件夹":"文件"),e.is_dir,active);
        gui.text(225,y+7,17,palette::text,e.name,430);
        if(!e.is_dir) gui.text(660,y+8,16,palette::muted,format_size(e.size),105);
        if(order>0) { const std::string t="#"+std::to_string(order); gui.round_rect(782,y+7,64,25,12,palette::warning); gui.text(782+(64-gui.text_width(14,t))/2,y+11,14,palette::white,t); }
    }
    panel(gui,890,198,350,430,"已选择安装包");
    gui.text(910,258,22,palette::text,std::to_string(queue.size())+" 项 · #号即实际顺序",300);
    std::vector<std::string> labels; labels.reserve(queue.size()); for(const auto& e:queue) labels.push_back(e.entry.name);
    queue_preview(gui,labels,910,302,300);
    gui.paragraph(910,445,15,palette::muted,"X 直接勾选/取消；Y 打开页面操作。批量执行前可左右调整顺序。网络直装仍显示下载与安装写入速度。",300,23,6);
    gui.button_hint(48,654,"A","打开 / 文件操作");
    gui.button_hint(300,654,"B","返回上级",palette::danger);
    gui.button_hint(500,654,"X","勾选 / 取消",palette::success);
    gui.button_hint(742,654,"Y","页面操作",palette::accent);
    gui.end();
}

void draw_local(Gui& gui,const std::vector<LocalEntry>& entries,std::size_t selected,const std::string& current,
                const std::vector<std::string>& selection,const std::string& location,const std::string& protocol,
                const std::string& status,bool show_cache) {
    gui.begin(); draw_shell(gui,Tab::Local,protocol,status,show_cache);
    panel(gui,40,198,830,430,"本机文件浏览");
    gui.text(58,246,16,palette::muted,"存储位置："+location,260);
    gui.text(310,246,16,palette::muted,"路径："+current,535);
    const auto begin=begin_for(selected,entries.size(),8),end=std::min(entries.size(),begin+8);
    if(entries.empty()) gui.text(58,315,19,palette::muted,"当前文件夹为空");
    for(std::size_t i=begin;i<end;++i) {
        const int y=285+static_cast<int>(i-begin)*42; const auto& e=entries[i]; const bool active=i==selected;
        if(active) gui.round_rect(52,y,806,38,8,palette::selected);
        const auto kind=detect_local_install_candidate(e); const bool installable=kind!=InstallCandidateKind::None;
        const auto order=local_selection_position(selection,e.path);
        selection_box(gui,64,y+5,installable,order>0);
        badge(gui,106,y+7,kind!=InstallCandidateKind::None?candidate_label(kind):(e.is_dir?"文件夹":"文件"),e.is_dir,active);
        gui.text(225,y+7,17,palette::text,e.name,430);
        if(!e.is_dir) gui.text(660,y+8,16,palette::muted,format_size(e.size),105);
        if(order>0) { const std::string t="#"+std::to_string(order); gui.round_rect(782,y+7,64,25,12,palette::success); gui.text(782+(64-gui.text_width(14,t))/2,y+11,14,palette::white,t); }
    }
    panel(gui,890,198,350,430,"已选择安装包");
    gui.text(910,258,22,palette::text,std::to_string(selection.size())+" 项 · 按选择顺序",300);
    std::vector<std::string> labels; labels.reserve(selection.size());
    for(const auto& path:selection) labels.push_back(astranas::app::basename_of(path));
    queue_preview(gui,labels,910,302,300);
    gui.paragraph(910,445,15,palette::muted,"X 直接勾选/取消；Y 打开批量、全选/反选、切换存储等页面操作。移动模式进入后，状态栏会提示 Y → 移动到当前目录。",300,23,6);
    gui.button_hint(48,654,"A","打开 / 文件操作");
    gui.button_hint(300,654,"B","返回 / 取消移动",palette::danger);
    gui.button_hint(500,654,"X","勾选 / 取消",palette::success);
    gui.button_hint(742,654,"Y","页面操作",palette::accent);
    gui.end();
}

void draw_batch_queue(Gui& gui,const std::string& title,const std::vector<std::string>& items,std::size_t selected) {
    gui.begin();
    gui.clear(palette::background);
    gui.fill_rect(0,0,Gui::kWidth,70,palette::navy);
    gui.text(40,16,28,palette::white,title,720);
    gui.text(850,20,17,palette::white,std::to_string(items.size())+" 项");
    gui.round_rect(90,104,1100,510,18,palette::surface);
    gui.stroke_rect(90,104,1100,510,1,palette::border);
    gui.text(126,132,17,palette::muted,"↑↓ 选择项目   ←→ 调整安装顺序   X 移除   Y 清空全部",1020);
    const auto begin=begin_for(selected,items.size(),7),end=std::min(items.size(),begin+7);
    for(std::size_t i=begin;i<end;++i) {
        const int y=196+static_cast<int>(i-begin)*48; const bool active=i==selected;
        if(active) gui.round_rect(118,y,1044,42,8,palette::selected);
        const std::string order="#"+std::to_string(i+1);
        gui.round_rect(132,y+8,62,26,13,active?palette::accent:palette::warning);
        gui.text(132+(62-gui.text_width(14,order))/2,y+12,14,palette::white,order);
        gui.text(214,y+9,18,active?palette::accent:palette::text,items[i],910);
    }
    gui.button_hint(126,646,"A","按当前顺序开始",palette::accent);
    gui.button_hint(430,646,"B","返回继续选择",palette::danger);
    gui.button_hint(724,646,"X","移除当前项",palette::warning);
    gui.button_hint(988,646,"Y","清空",palette::danger);
    gui.end();
}

void draw_cache(Gui& gui,const std::vector<LocalEntry>& entries,std::size_t selected,const std::string& protocol,const std::string& status,bool show_cache) {
    gui.begin(); draw_shell(gui,Tab::Cache,protocol,status,show_cache);
    panel(gui,40,198,830,430,"安装缓存");
    gui.text(58,246,16,palette::muted,"关闭网络直装时使用缓存；直装不满足条件会明确报错，不会自动切换缓存",790);
    const auto begin=begin_for(selected,entries.size(),8),end=std::min(entries.size(),begin+8);
    if(entries.empty()) gui.text(58,315,19,palette::muted,"缓存中没有文件");
    for(std::size_t i=begin;i<end;++i){const int y=285+static_cast<int>(i-begin)*42;const auto&e=entries[i];const bool active=i==selected;if(active)gui.round_rect(52,y,806,38,8,palette::selected);const auto kind=detect_local_install_candidate(e);badge(gui,64,y+7,kind!=InstallCandidateKind::None?candidate_label(kind):(e.is_dir?"文件夹":"文件"),e.is_dir,active);gui.text(185,y+7,17,palette::text,e.name,470);if(!e.is_dir)gui.text(680,y+8,16,palette::muted,format_size(e.size),130);}
    panel(gui,890,198,350,430,"缓存说明");
    gui.text(910,260,17,palette::muted,"当前项目"); gui.text(910,288,24,palette::text,std::to_string(entries.size())+" 项");
    gui.paragraph(910,342,16,palette::text,"缓存模式先把远端包完整写入本地，再读取安装。刷新与清空全部缓存统一放到 Y 页面操作。",300,24,8);
    gui.button_hint(48,654,"A","文件操作"); gui.button_hint(320,654,"Y","页面操作",palette::accent); gui.end();
}

void draw_installed(Gui&gui,const std::vector<astranas::installed::TitleEntry>&entries,std::size_t selected,const std::map<std::uint64_t,astranas::installed::IconBitmap>&icons,const std::string&protocol,const std::string&status,bool show_cache){
    gui.begin();draw_shell(gui,Tab::Installed,protocol,status,show_cache);
    panel(gui,40,198,830,430,"已安装游戏");
    gui.text(58,246,16,palette::muted,"只显示实际已安装的更新/DLC；DLC 数量来自系统 ContentMeta 状态",790);
    const auto begin=begin_for(selected,entries.size(),7),end=std::min(entries.size(),begin+7);
    if(entries.empty())gui.text(58,315,19,palette::muted,"没有缓存；首次进入会读取系统游戏列表");
    for(std::size_t i=begin;i<end;++i){
        const int y=282+static_cast<int>(i-begin)*48;const auto&e=entries[i];const bool active=i==selected;
        if(active)gui.round_rect(52,y,806,44,8,palette::selected);
        const auto icon=icons.find(e.application_id);
        if(icon!=icons.end())gui.image_rgb(62,y+4,36,36,icon->second.rgb.data(),icon->second.width,icon->second.height);
        else{gui.round_rect(62,y+4,36,36,6,palette::surface_alt);gui.text(70,y+13,14,palette::muted,"游");}
        const std::string label=e.name.empty()?title_id_text(e.application_id):e.name;
        gui.text(112,y+5,17,palette::text,label,450);
        std::string content="本体 v"+std::to_string(e.base_version);
        if(e.patch_version)content+=" · 更新 v"+std::to_string(e.patch_version);
        if(e.dlc_count)content+=" · DLC "+std::to_string(e.dlc_count);
        gui.text(570,y+6,14,palette::muted,content,280);
    }
    panel(gui,890,198,350,430,"所选游戏");
    if(selected<entries.size()){
        const auto&e=entries[selected];const auto icon=icons.find(e.application_id);
        if(icon!=icons.end())gui.image_rgb(910,258,96,96,icon->second.rgb.data(),icon->second.width,icon->second.height);
        const std::string label=e.name.empty()?title_id_text(e.application_id):e.name;
        gui.text(1018,258,19,palette::text,label,200);
        gui.text(910,376,15,palette::muted,"Title ID");gui.text(910,400,16,palette::text,title_id_text(e.application_id),300);
        std::string installed="本体 v"+std::to_string(e.base_version);
        if(e.patch_version)installed+=" · 更新 v"+std::to_string(e.patch_version);
        installed+=" · DLC "+std::to_string(e.dlc_count);
        gui.text(910,438,15,palette::muted,"已安装内容");gui.text(910,462,16,palette::text,installed,300);
        if(!e.display_version.empty()){gui.text(910,500,15,palette::muted,"显示版本");gui.text(910,524,17,palette::text,e.display_version,300);}
        gui.paragraph(910,564,14,palette::muted,"A 打开按实际状态生成的卸载菜单；不会主动删除游戏存档。",300,22,3);
    }
    gui.button_hint(48,654,"A","游戏操作");gui.button_hint(270,654,"Y","刷新游戏列表",palette::accent);gui.end();
}

const char* setting_label(std::size_t index){
    static const char* labels[]={"连接协议","SMB 配置","WebDAV 配置","失败重试次数","网络直装","下载后整包 SHA-256","NCA 内容 SHA-256","安装位置","安装后删除源文件","忽略固件要求","NCA 头签名校验","安装日志","测试当前连接","网络诊断","Wi-Fi 兼容帮助"};
    return index<kSettingsCount?labels[index]:"未知设置";
}
std::string setting_value(const AppConfig&c,std::size_t i,double speed){
    switch(i){case 0:return c.protocol=="webdav"?"WebDAV":"SMB";case 1:return c.smb.server.empty()?"未设置":(c.smb.server+(c.smb.share.empty()?" / 自动共享":" / "+c.smb.share));case 2:return c.webdav.server.empty()?"未设置":(std::string(c.webdav.tls?"HTTPS · ":"HTTP · ")+c.webdav.server);case 3:return std::to_string(c.download_retries)+" 次";case 4:return c.network_direct_install?"开启（仅直装）":"关闭（缓存安装）";case 5:return c.verify_sha256?"开启":"关闭";case 6:return c.verify_nca_content_hash?"开启":"关闭";case 7:return c.install_to_nand?"主机存储（NAND）":"SD 卡";case 8:return c.delete_source_after_install?"开启":"关闭";case 9:return c.ignore_required_firmware?"开启（谨慎）":"关闭";case 10:return c.validate_nca?"开启":"关闭";case 11:return "按 A 管理 / 上传";case 12:return "按 A 测试";case 13:return speed>0?("最近 "+format_speed(speed)):"按 A 运行";case 14:return "按 A 查看";}return{};
}
const char* setting_help(std::size_t i){
    static const char* help[]={
        "选择当前使用 SMB 或 WebDAV。两套连接资料分别保存，切换协议不会覆盖或清空另一套配置。",
        "打开 SMB 独立配置弹窗：服务器、端口、共享、远程路径、用户名和密码都只属于 SMB。",
        "打开 WebDAV 独立配置弹窗：服务器、端口、远程路径、账号、HTTPS 与 TLS 校验都只属于 WebDAV。",
        "网络读取失败后的自动重试次数，范围 0 到 5 次。直装开始后不会自动改成缓存安装。",
        "开启后只执行 SMB/WebDAV → RAM → NCM 网络直装；能力探测失败会明确显示原因。关闭后才使用缓存安装。",
        "仅缓存下载模式使用。开启后下载完成会额外完整读取一次安装包并计算 SHA-256。",
        "对安装过程中还原后的完整 NCA 做 Content ID/CNMT SHA-256；默认关闭以减少热路径计算。",
        "选择安装到 SD 卡或主机用户存储。",
        "本地/缓存源安装成功后删除准确源包；网络直装不会删除 NAS 原文件。",
        "可能导致应用无法运行，仅在明确了解风险时开启。",
        "保留 NCA3 结构、声明大小和 RSA-PSS 头签名校验，建议保持开启。",
        "每次安装结束只把最近日志保存到本机，不会自动连接 NAS 或上传。按 A 可手动上传最近日志、选择上传目录或清空目录。",
        "使用当前协议的已保存配置立即测试连接，不会修改 SMB/WebDAV 配置。",
        "纯 RAM 读取测速，不写 SD、不安装。更完整的 socket 缓冲扫描和安装层对照请使用 AstraNAS-NetDiag。",
        "查看 5 GHz Wi-Fi 常见兼容建议。独立 AstraNAS-NetDiag.nro 可做更干净的网络对照测试。"
    };
    return i<kSettingsCount?help[i]:"";
}
void draw_settings(Gui&gui,const AppConfig&config,std::size_t selected,const std::string&protocol,const std::string&status,double speed,bool show_cache){
    gui.begin();draw_shell(gui,Tab::Settings,protocol,status,show_cache);panel(gui,40,198,790,430,"连接与安装设置");const auto begin=begin_for(selected,kSettingsCount,8),end=std::min(kSettingsCount,begin+8);for(std::size_t i=begin;i<end;++i){const int y=252+static_cast<int>(i-begin)*44;const bool active=i==selected;if(active)gui.round_rect(52,y,766,40,8,palette::selected);gui.text(64,y+8,17,active?palette::accent:palette::text,setting_label(i),260);gui.text(340,y+8,16,palette::muted,setting_value(config,i,speed),455);}panel(gui,850,198,390,430,"当前设置说明");gui.text(872,258,20,palette::text,setting_label(selected),346);gui.paragraph(872,304,17,palette::text,setting_help(selected),346,27,8);gui.divider(872,524,346);gui.paragraph(872,544,15,palette::muted,"测速与安装日志都只在本机自动保存；只有你明确选择“上传最近日志”时才会连接 NAS 并上传。",346,23,4);gui.button_hint(48,654,"A","修改 / 切换 / 执行");gui.button_hint(354,654,"B","返回本机文件",palette::danger);gui.end();
}
} // namespace astranas::ui
#endif

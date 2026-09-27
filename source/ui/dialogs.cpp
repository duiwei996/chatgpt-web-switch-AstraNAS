// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialogs.hpp"
#ifdef __SWITCH__
#include "pages.hpp"
#include "../app/constants.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace astranas::ui {
namespace {
namespace palette = astranas::ui::palette;
struct ModalTouchTracker {
    bool active=false, dragged=false;
    int start_x=0, start_y=0, x=0, y=0;
};
bool modal_touch_tap(ModalTouchTracker& tracker, int& out_x, int& out_y) {
    HidTouchScreenState state{};
    const bool touching = hidGetTouchScreenStates(&state, 1) > 0 && state.count > 0;
    if (touching) {
        const int x = static_cast<int>(state.touches[0].x), y = static_cast<int>(state.touches[0].y);
        if (!tracker.active) { tracker.active=true; tracker.dragged=false; tracker.start_x=tracker.x=x; tracker.start_y=tracker.y=y; }
        else { tracker.x=x; tracker.y=y; if (std::abs(x-tracker.start_x)>18 || std::abs(y-tracker.start_y)>18) tracker.dragged=true; }
        return false;
    }
    if (!tracker.active) return false;
    const bool tap=!tracker.dragged; out_x=tracker.x; out_y=tracker.y; tracker.active=false; tracker.dragged=false; return tap;
}

bool address_only(const std::string& value) {
    return value.find("://") == std::string::npos && value.find('/') == std::string::npos &&
           value.find('\\') == std::string::npos;
}

bool parse_port_value(const std::string& value, int& port) {
    if (value.empty()) { port = 0; return true; }
    try {
        const int parsed = std::stoi(value);
        if (parsed < 1 || parsed > 65535) return false;
        port = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

std::string masked_value(const std::string& value) { return value.empty() ? "未设置" : "已设置"; }

void draw_profile_editor(Gui& gui, const std::string& title,
                         const std::vector<std::pair<std::string, std::string>>& rows,
                         std::size_t selected, const std::string& note) {
    gui.begin();
    gui.clear(palette::background);
    gui.fill_rect(0,0,Gui::kWidth,70,palette::navy);
    gui.text(40,16,28,palette::white,title);
    gui.round_rect(150,98,980,520,18,palette::surface);
    gui.stroke_rect(150,98,980,520,1,palette::border);
    const int start_y=132,row_h=54;
    for(std::size_t i=0;i<rows.size();++i){
        const int y=start_y+static_cast<int>(i)*row_h;
        const bool active=i==selected;
        gui.round_rect(184,y,912,44,9,active?palette::selected:palette::surface_alt);
        gui.text(204,y+11,18,active?palette::accent:palette::text,rows[i].first,250);
        gui.text(470,y+11,17,palette::muted,rows[i].second,600);
    }
    gui.text(184,570,15,palette::muted,note,890);
    gui.button_hint(184,632,"A","修改 / 保存",palette::accent);
    gui.button_hint(480,632,"B","取消",palette::danger);
    gui.end();
}
}

void draw_simple_message(Gui& gui, const std::string& title, const std::string& detail,
                         const std::string& primary_button, const std::string& primary_label,
                         const std::string& secondary_button, const std::string& secondary_label,
                         const std::string& header_title) {
    gui.begin();
    gui.clear(palette::background);
    gui.fill_rect(0,0,Gui::kWidth,70,palette::navy);
    gui.text(40,16,28,palette::white,header_title.empty() ? astranas::app::kDisplayName : header_title);
    gui.round_rect(190,128,900,456,18,palette::surface);
    gui.stroke_rect(190,128,900,456,1,palette::border);
    gui.text(232,172,28,palette::text,title,816);
    gui.divider(232,220,816);
    gui.paragraph(232,252,19,palette::text,detail,816,31,8);
    if(!primary_button.empty()) gui.button_hint(250,516,primary_button,primary_label,palette::accent);
    if(!secondary_button.empty()) gui.button_hint(550,516,secondary_button,secondary_label,palette::danger);
    gui.end();
}
bool confirm_action(Gui& gui, PadState& pad, bool& exit_requested,
                    const std::string& title, const std::string& detail,
                    const std::string& confirm_label, const std::string& cancel_label) {
    draw_simple_message(gui,title,detail,"A",confirm_label,"B",cancel_label);
    ModalTouchTracker touch;
    while(appletMainLoop()) {
        padUpdate(&pad); const u64 down=padGetButtonsDown(&pad);
        if(down&HidNpadButton_Plus){exit_requested=true;return false;}
        int tx=0,ty=0; const bool tap=modal_touch_tap(touch,tx,ty);
        if(tap&&ty>=490&&ty<=560&&tx>=220&&tx<520)return true;
        if(tap&&ty>=490&&ty<=560&&tx>=520&&tx<850)return false;
        if(down&HidNpadButton_A)return true;
        if(down&HidNpadButton_B)return false;
        svcSleepThread(10'000'000);
    }
    return false;
}
bool prompt_text(const char* title,const std::string& initial,std::string& output,bool password,bool username){
    SwkbdConfig keyboard{}; Result rc=swkbdCreate(&keyboard,0); if(R_FAILED(rc))return false;
    if(password)swkbdConfigMakePresetPassword(&keyboard); else if(username)swkbdConfigMakePresetUserName(&keyboard); else swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard,title); swkbdConfigSetInitialText(&keyboard,initial.c_str()); swkbdConfigSetStringLenMax(&keyboard,511);
    char buffer[512]{}; rc=swkbdShow(&keyboard,buffer,sizeof(buffer)); swkbdClose(&keyboard); if(R_FAILED(rc))return false; output=buffer; return true;
}
bool choose_protocol(Gui& gui,PadState& pad,InputRouter& input,bool& exit_requested,const std::string& current,std::string& selected){
    int choice=current=="webdav"?1:0; bool dirty=true;
    while(appletMainLoop()){
        if(dirty){
            gui.begin(); gui.clear(palette::background); gui.fill_rect(0,0,Gui::kWidth,70,palette::navy); gui.text(40,16,28,palette::white,"选择连接协议");
            const char* labels[]={"SMB","WebDAV"};
            for(int i=0;i<2;++i){ const bool active=i==choice; const int y=220+i*90; gui.round_rect(250,y,780,66,12,active?palette::selected:palette::surface); gui.stroke_rect(250,y,780,66,1,palette::border); gui.text(282,y+18,23,active?palette::accent:palette::text,labels[i]); }
            gui.button_hint(250,480,"A","确认"); gui.button_hint(480,480,"B","取消",palette::danger); gui.end(); dirty=false;
        }
        const auto frame=input.update(pad);
        if(frame.down&HidNpadButton_Plus){exit_requested=true;return false;}
        if(frame.nav&(HidNpadButton_Up|HidNpadButton_Down)) { choice=1-choice; dirty=true; }
        if(frame.touch_tap){ if(frame.touch_y>=220&&frame.touch_y<286){choice=0;dirty=true;} else if(frame.touch_y>=310&&frame.touch_y<376){choice=1;dirty=true;} else if(frame.touch_y>=460&&frame.touch_y<550&&frame.touch_x<450){selected=choice==0?"smb":"webdav";return true;} }
        if(frame.down&HidNpadButton_A){selected=choice==0?"smb":"webdav";return true;}
        if(frame.down&HidNpadButton_B)return false;
        svcSleepThread(10'000'000);
    }
    return false;
}
bool edit_smb_profile(Gui& gui,PadState& pad,InputRouter& input,bool& exit_requested,
                      const SmbProfileConfig& initial,SmbProfileConfig& output){
    SmbProfileConfig draft=initial;
    std::size_t choice=0;
    std::string note="A 修改字段；选择“保存 SMB 配置”后才写入。B 放弃本次修改。";
    bool dirty=true;
    while(appletMainLoop()){
        std::vector<std::pair<std::string,std::string>> rows={
            {"服务器",draft.server.empty()?"未设置":draft.server},
            {"端口",draft.port>0?std::to_string(draft.port):"自动（445）"},
            {"共享",draft.share.empty()?"自动发现":draft.share},
            {"远程路径",draft.root.empty()?"/":draft.root},
            {"用户名",draft.username.empty()?"未设置":draft.username},
            {"密码",masked_value(draft.password)},
            {"保存 SMB 配置","按 A 保存"},
        };
        if(dirty){draw_profile_editor(gui,"SMB 配置",rows,choice,note);dirty=false;}
        const auto frame=input.update(pad);
        if(frame.down&HidNpadButton_Plus){exit_requested=true;return false;}
        if(frame.nav_once&HidNpadButton_Up){choice=choice==0?rows.size()-1:choice-1;dirty=true;}
        if(frame.nav_once&HidNpadButton_Down){choice=(choice+1)%rows.size();dirty=true;}
        if(frame.touch_tap&&frame.touch_x>=184&&frame.touch_x<1096&&frame.touch_y>=132){
            const int row=(frame.touch_y-132)/54;
            if(row>=0&&static_cast<std::size_t>(row)<rows.size()){choice=static_cast<std::size_t>(row);dirty=true;}
        }
        if(frame.down&HidNpadButton_B)return false;
        if(!(frame.down&HidNpadButton_A))continue;
        std::string edited;
        switch(choice){
            case 0:
                if(prompt_text("SMB 服务器地址",draft.server,edited,false,false)){
                    if(!address_only(edited)){note="地址只填 IP 或域名，不要包含协议、端口或路径。";}
                    else{draft.server=edited;note="服务器地址已修改，尚未保存。";}
                    dirty=true;
                }
                break;
            case 1:
                if(prompt_text("SMB 端口（留空自动 445）",draft.port?std::to_string(draft.port):std::string{},edited,false,false)){
                    int port=0;if(!parse_port_value(edited,port))note="端口必须是 1 到 65535，留空表示自动。";
                    else{draft.port=port;note="端口已修改，尚未保存。";}dirty=true;
                }
                break;
            case 2:
                if(prompt_text("SMB 共享（留空自动发现）",draft.share,edited,false,false)){draft.share=edited;note="共享已修改，尚未保存。";dirty=true;}
                break;
            case 3:
                if(prompt_text("SMB 远程路径",draft.root,edited,false,false)){draft.root=edited.empty()?"/":edited;note="远程路径已修改，尚未保存。";dirty=true;}
                break;
            case 4:
                if(prompt_text("SMB 用户名",draft.username,edited,false,true)){draft.username=edited;note="用户名已修改，尚未保存。";dirty=true;}
                break;
            case 5:
                if(prompt_text("SMB 密码",draft.password,edited,true,false)){draft.password=edited;note="密码已修改，尚未保存。";dirty=true;}
                break;
            case 6: output=draft; return true;
        }
    }
    return false;
}

bool edit_webdav_profile(Gui& gui,PadState& pad,InputRouter& input,bool& exit_requested,
                         const WebDavProfileConfig& initial,WebDavProfileConfig& output){
    WebDavProfileConfig draft=initial;
    std::size_t choice=0;
    std::string note="A 修改字段；HTTPS/TLS 可直接切换；选择保存后才写入。";
    bool dirty=true;
    while(appletMainLoop()){
        std::vector<std::pair<std::string,std::string>> rows={
            {"服务器",draft.server.empty()?"未设置":draft.server},
            {"端口",draft.port>0?std::to_string(draft.port):(draft.tls?"自动（443）":"自动（80）")},
            {"远程路径",draft.root.empty()?"/":draft.root},
            {"用户名",draft.username.empty()?"未设置":draft.username},
            {"密码",masked_value(draft.password)},
            {"连接方式",draft.tls?"HTTPS":"HTTP"},
            {"TLS 证书验证",draft.tls_verify?"开启":"关闭"},
            {"保存 WebDAV 配置","按 A 保存"},
        };
        if(dirty){draw_profile_editor(gui,"WebDAV 配置",rows,choice,note);dirty=false;}
        const auto frame=input.update(pad);
        if(frame.down&HidNpadButton_Plus){exit_requested=true;return false;}
        if(frame.nav_once&HidNpadButton_Up){choice=choice==0?rows.size()-1:choice-1;dirty=true;}
        if(frame.nav_once&HidNpadButton_Down){choice=(choice+1)%rows.size();dirty=true;}
        if(frame.touch_tap&&frame.touch_x>=184&&frame.touch_x<1096&&frame.touch_y>=132){
            const int row=(frame.touch_y-132)/54;
            if(row>=0&&static_cast<std::size_t>(row)<rows.size()){choice=static_cast<std::size_t>(row);dirty=true;}
        }
        if(frame.down&HidNpadButton_B)return false;
        if(!(frame.down&HidNpadButton_A))continue;
        std::string edited;
        switch(choice){
            case 0:
                if(prompt_text("WebDAV 服务器地址",draft.server,edited,false,false)){
                    if(!address_only(edited))note="地址只填 IP 或域名，不要包含协议、端口或路径。";
                    else{draft.server=edited;note="服务器地址已修改，尚未保存。";}dirty=true;
                }
                break;
            case 1:
                if(prompt_text("WebDAV 端口（留空自动）",draft.port?std::to_string(draft.port):std::string{},edited,false,false)){
                    int port=0;if(!parse_port_value(edited,port))note="端口必须是 1 到 65535，留空表示自动。";
                    else{draft.port=port;note="端口已修改，尚未保存。";}dirty=true;
                }
                break;
            case 2:
                if(prompt_text("WebDAV 远程路径",draft.root,edited,false,false)){draft.root=edited.empty()?"/":edited;note="远程路径已修改，尚未保存。";dirty=true;}
                break;
            case 3:
                if(prompt_text("WebDAV 用户名",draft.username,edited,false,true)){draft.username=edited;note="用户名已修改，尚未保存。";dirty=true;}
                break;
            case 4:
                if(prompt_text("WebDAV 密码",draft.password,edited,true,false)){draft.password=edited;note="密码已修改，尚未保存。";dirty=true;}
                break;
            case 5: draft.tls=!draft.tls;draft.port=0;note="连接方式已切换，端口恢复自动。";dirty=true;break;
            case 6: draft.tls_verify=!draft.tls_verify;note="TLS 证书验证已切换。";dirty=true;break;
            case 7: output=draft; return true;
        }
    }
    return false;
}

void show_message_wait(Gui& gui,PadState& pad,bool& exit_requested,
                       const std::string& title,const std::string& detail,const std::string& header_title){
    draw_simple_message(gui,title,detail,"A","返回",{}, {},header_title);
    while(appletMainLoop()){
        padUpdate(&pad);const u64 down=padGetButtonsDown(&pad);
        if(down&HidNpadButton_Plus){exit_requested=true;return;}
        if(down&(HidNpadButton_A|HidNpadButton_B))return;
        svcSleepThread(10'000'000);
    }
}

int choose_action(Gui& gui,PadState& pad,InputRouter& input,bool& exit_requested,
                  const std::string& title,const std::string& detail,
                  const std::vector<std::string>& actions,const std::string& header_title){
    if(actions.empty())return -1;
    constexpr std::size_t kVisibleRows=5;
    std::size_t choice=0; bool dirty=true;
    while(appletMainLoop()){
        const std::size_t begin = choice < kVisibleRows ? 0 :
            std::min(choice-kVisibleRows+1, actions.size()>kVisibleRows?actions.size()-kVisibleRows:0);
        const std::size_t visible = std::min(kVisibleRows, actions.size()-begin);
        if(dirty){
            gui.begin(); gui.clear(palette::background); gui.fill_rect(0,0,Gui::kWidth,70,palette::navy);
            gui.text(40,16,28,palette::white,header_title.empty() ? astranas::app::kDisplayName : header_title);
            gui.round_rect(180,92,920,548,18,palette::surface); gui.stroke_rect(180,92,920,548,1,palette::border);
            gui.text(220,126,27,palette::text,title,840);
            gui.paragraph(220,170,16,palette::muted,detail,840,23,3);
            const int start_y=252; const int row_h=62;
            for(std::size_t row=0;row<visible;++row){
                const std::size_t i=begin+row;
                const int y=start_y+static_cast<int>(row)*row_h; const bool active=i==choice;
                gui.round_rect(220,y,840,50,10,active?palette::selected:palette::surface_alt);
                gui.stroke_rect(220,y,840,50,1,palette::border);
                gui.text(246,y+13,19,active?palette::accent:palette::text,actions[i],780);
            }
            if(actions.size()>kVisibleRows){
                gui.text(920,220,14,palette::muted,
                         std::to_string(choice+1)+"/"+std::to_string(actions.size()),120);
            }
            gui.button_hint(220,590,"A","选择"); gui.button_hint(448,590,"B","取消",palette::danger); gui.end(); dirty=false;
        }
        const auto frame=input.update(pad);
        if(frame.down&HidNpadButton_Plus){exit_requested=true;return -1;}
        if(frame.nav&HidNpadButton_Up){choice=choice==0?actions.size()-1:choice-1;dirty=true;}
        if(frame.nav&HidNpadButton_Down){choice=(choice+1)%actions.size();dirty=true;}
        if(frame.touch_tap){
            const int start_y=252,row_h=62;
            if(frame.touch_x>=220&&frame.touch_x<1060&&frame.touch_y>=start_y){
                const int row=(frame.touch_y-start_y)/row_h;
                if(row>=0&&static_cast<std::size_t>(row)<visible&&
                   frame.touch_y<start_y+(row+1)*row_h-12)
                    return static_cast<int>(begin+static_cast<std::size_t>(row));
            }
        }
        if(frame.down&HidNpadButton_A)return static_cast<int>(choice);
        if(frame.down&HidNpadButton_B)return -1;
        svcSleepThread(10'000'000);
    }
    return -1;
}

void draw_progress_page(Gui& gui,const std::string& title,const std::string& name,const std::string& detail,
                        std::uint64_t done,std::uint64_t total,double mib_per_sec,int attempt,int max_attempts){
    gui.begin(); gui.clear(palette::background); gui.fill_rect(0,0,Gui::kWidth,70,palette::navy); gui.text(40,16,28,palette::white,astranas::app::kDisplayName);
    gui.round_rect(130,120,1020,490,18,palette::surface); gui.stroke_rect(130,120,1020,490,1,palette::border);
    gui.text(176,162,28,palette::text,title,930); gui.text(176,214,19,palette::muted,name,930); gui.paragraph(176,250,17,palette::text,detail,930,27,3);
    const double ratio=total>0?static_cast<double>(done)/static_cast<double>(total):0.0; gui.progress_bar(176,348,928,24,ratio);
    char percent[32]{}; if(total>0)std::snprintf(percent,sizeof(percent),"%.1f%%",ratio*100.0);
    gui.text(176,390,20,palette::text,total>0?(std::to_string(done/1024/1024)+" MB / "+std::to_string(total/1024/1024)+" MB  /  "+percent):("已处理 "+std::to_string(done/1024/1024)+" MB"));
    if(mib_per_sec>0.0){char speed[64]{};std::snprintf(speed,sizeof(speed),"速度：%.2f MB/s",mib_per_sec);gui.text(176,432,19,palette::muted,speed);}
    if(attempt>0&&max_attempts>0)gui.text(176,468,18,palette::muted,"尝试次数："+std::to_string(attempt)+" / "+std::to_string(max_attempts));
    gui.button_hint(176,542,"B","取消",palette::danger); gui.end();
}
void show_wifi_notice(Gui& gui,PadState& pad,bool& exit_requested,bool allow_remind_later){
    const std::string detail="针对日版 Switch 1 在中国大陆连接 5 GHz NAS 的常见情况：\n\n- 路由器 5 GHz 优先固定到 36 / 40 / 44 / 48 信道。\n- 建议以 80 MHz + WPA2-AES 作为稳定基线。\n- 测速阶段先关闭自动信道，避免 DFS 或高信道造成断连。\n- 如果 5 GHz 网络不显示或不稳定，请检查 DFS 与 149+ 信道设置。";
    draw_simple_message(gui,"Wi-Fi 兼容建议",detail,"A",allow_remind_later?"知道了":"返回",allow_remind_later?"B":"",allow_remind_later?"下次再提醒":"");
    while(appletMainLoop()){
        padUpdate(&pad);const u64 down=padGetButtonsDown(&pad);if(down&HidNpadButton_Plus){exit_requested=true;return;}
        if(allow_remind_later&&(down&HidNpadButton_A)){FILE*fp=std::fopen("sdmc:/switch/AstraNAS/.wifi_notice_seen","wb");if(fp){std::fputs("seen\n",fp);std::fclose(fp);}return;}
        if(down&HidNpadButton_B)return; if(!allow_remind_later&&(down&HidNpadButton_A))return; svcSleepThread(10'000'000);
    }
}
} // namespace astranas::ui
#endif

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include <switch.h>
#include <cstdint>
#include <string>

namespace astranas::ui {
struct Color { std::uint8_t r,g,b,a=255; };
namespace palette {
extern const Color background,surface,surface_alt,navy,accent,accent_soft,selected,text,muted,border,white,success,success_soft,warning,warning_soft,danger,danger_soft,folder,file;
}
class Gui {
public:
    static constexpr int kWidth=1280,kHeight=720;
    Gui()=default;~Gui();
    bool initialize(std::string& error);void shutdown();void begin();void end();
    void clear(Color color);void fill_rect(int x,int y,int w,int h,Color color);void stroke_rect(int x,int y,int w,int h,int thickness,Color color);void round_rect(int x,int y,int w,int h,int radius,Color color);
    void text(int x,int y,int size,Color color,const std::string& value,int max_width=-1);int text_width(int size,const std::string& value)const;
    void paragraph(int x,int y,int size,Color color,const std::string& value,int max_width,int line_height,int max_lines);
    void divider(int x,int y,int w);void button_hint(int x,int y,const std::string& button,const std::string& label,Color accent=palette::accent);void progress_bar(int x,int y,int w,int h,double ratio);
    void image_rgb(int x,int y,int w,int h,const std::uint8_t* rgb,int src_w,int src_h);
private:
    Framebuffer framebuffer_{};std::uint32_t*pixels_=nullptr;int stride_=0;bool initialized_=false,font_service_ready_=false;
    void put_pixel(int x,int y,Color color);int glyph_advance(std::uint32_t codepoint,int size)const;static std::uint32_t decode_utf8(const std::string& value,std::size_t& index);
};
} // namespace astranas::ui
#endif

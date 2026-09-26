// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui.hpp"
#ifdef __SWITCH__
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace astranas::ui {
namespace {
FT_Library g_ft = nullptr;
struct FontSlot { PlFontData data{}; FT_Face face=nullptr; };
constexpr std::array<PlSharedFontType,6> kFontOrder={
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_Standard,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
    PlSharedFontType_NintendoExt,
};
std::array<FontSlot,kFontOrder.size()> g_fonts{};

FT_Face find_face(std::uint32_t codepoint){
    for(auto& slot:g_fonts)if(slot.face&&FT_Get_Char_Index(slot.face,codepoint)!=0)return slot.face;
    return nullptr;
}
FT_Face resolve_face(std::uint32_t& codepoint){
    if(FT_Face face=find_face(codepoint))return face;
    codepoint='?';
    return find_face(codepoint);
}

inline Color blend(Color dst, Color src) {
    const int a = src.a;
    const int inv = 255 - a;
    return Color{
        static_cast<std::uint8_t>((src.r * a + dst.r * inv) / 255),
        static_cast<std::uint8_t>((src.g * a + dst.g * inv) / 255),
        static_cast<std::uint8_t>((src.b * a + dst.b * inv) / 255), 255};
}
}

namespace palette {
const Color background{241,244,248,255};
const Color surface{255,255,255,255};
const Color surface_alt{247,249,252,255};
const Color navy{24,38,58,255};
const Color accent{45,111,221,255};
const Color accent_soft{229,238,253,255};
const Color selected{225,237,255,255};
const Color text{28,38,50,255};
const Color muted{99,112,128,255};
const Color border{214,222,232,255};
const Color white{255,255,255,255};
const Color success{42,130,82,255};
const Color success_soft{229,246,236,255};
const Color warning{183,112,21,255};
const Color warning_soft{255,244,222,255};
const Color danger{187,62,62,255};
const Color danger_soft{253,232,232,255};
const Color folder{236,170,35,255};
const Color file{79,105,132,255};
}

Gui::~Gui() { shutdown(); }
bool Gui::initialize(std::string& error) {
    if (initialized_) return true;
    NWindow* window = nwindowGetDefault();
    Result rc = framebufferCreate(&framebuffer_, window, kWidth, kHeight, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) { error = "无法创建图形画面"; return false; }
    framebufferMakeLinear(&framebuffer_);
    rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) { framebufferClose(&framebuffer_); error = "无法初始化系统字体服务"; return false; }
    font_service_ready_ = true;
    if (FT_Init_FreeType(&g_ft) != 0) { error = "无法初始化字体渲染器"; shutdown(); return false; }
    std::size_t loaded=0;
    for(std::size_t i=0;i<kFontOrder.size();++i){
        PlFontData data{};
        if(R_FAILED(plGetSharedFontByType(&data,kFontOrder[i]))||!data.address||!data.size)continue;
        FT_Face face=nullptr;
        if(FT_New_Memory_Face(g_ft,static_cast<const FT_Byte*>(data.address),static_cast<FT_Long>(data.size),0,&face)!=0)continue;
        g_fonts[i].data=data;g_fonts[i].face=face;++loaded;
    }
    if(!loaded||!find_face(0x4E2D)){ error = "无法加载可用的中文系统字体"; shutdown(); return false; }
    initialized_ = true; return true;
}
void Gui::shutdown() {
    for(auto& slot:g_fonts){if(slot.face){FT_Done_Face(slot.face);slot.face=nullptr;}slot.data={};}
    if (g_ft) { FT_Done_FreeType(g_ft); g_ft = nullptr; }
    if (font_service_ready_) { plExit(); font_service_ready_ = false; }
    if (initialized_) framebufferClose(&framebuffer_);
    initialized_ = false; pixels_ = nullptr; stride_ = 0;
}
void Gui::begin() {
    if (!initialized_) return;
    u32 stride_bytes=0;
    pixels_=static_cast<std::uint32_t*>(framebufferBegin(&framebuffer_, &stride_bytes));
    stride_=static_cast<int>(stride_bytes/sizeof(std::uint32_t));
}
void Gui::end() { if (initialized_ && pixels_) { framebufferEnd(&framebuffer_); pixels_=nullptr; } }
void Gui::put_pixel(int x,int y,Color color) {
    if(!pixels_||x<0||y<0||x>=kWidth||y>=kHeight)return;
    const std::uint32_t old=pixels_[y*stride_+x];
    Color dst{static_cast<std::uint8_t>(old&0xff),static_cast<std::uint8_t>((old>>8)&0xff),static_cast<std::uint8_t>((old>>16)&0xff),255};
    const Color c=color.a==255?color:blend(dst,color);
    pixels_[y*stride_+x]=static_cast<std::uint32_t>(c.r)|(static_cast<std::uint32_t>(c.g)<<8)|(static_cast<std::uint32_t>(c.b)<<16)|(0xffu<<24);
}
void Gui::clear(Color color){fill_rect(0,0,kWidth,kHeight,color);}
void Gui::fill_rect(int x,int y,int w,int h,Color color){
    if(!pixels_)return; const int x0=std::max(0,x),y0=std::max(0,y),x1=std::min(kWidth,x+w),y1=std::min(kHeight,y+h);
    for(int py=y0;py<y1;++py)for(int px=x0;px<x1;++px)put_pixel(px,py,color);
}
void Gui::stroke_rect(int x,int y,int w,int h,int thickness,Color color){
    fill_rect(x,y,w,thickness,color);fill_rect(x,y+h-thickness,w,thickness,color);fill_rect(x,y,thickness,h,color);fill_rect(x+w-thickness,y,thickness,h,color);
}
void Gui::round_rect(int x,int y,int w,int h,int radius,Color color){
    radius=std::max(0,std::min(radius,std::min(w,h)/2)); fill_rect(x+radius,y,w-2*radius,h,color);fill_rect(x,y+radius,radius,h-2*radius,color);fill_rect(x+w-radius,y+radius,radius,h-2*radius,color);
    for(int dy=0;dy<radius;++dy)for(int dx=0;dx<radius;++dx){const int rx=radius-1-dx,ry=radius-1-dy;if(rx*rx+ry*ry<=radius*radius){put_pixel(x+dx,y+dy,color);put_pixel(x+w-1-dx,y+dy,color);put_pixel(x+dx,y+h-1-dy,color);put_pixel(x+w-1-dx,y+h-1-dy,color);}}
}
std::uint32_t Gui::decode_utf8(const std::string& value,std::size_t& index){
    const unsigned char c0=static_cast<unsigned char>(value[index++]); if(c0<0x80)return c0;
    int extra=0;std::uint32_t cp=0;if((c0&0xe0)==0xc0){extra=1;cp=c0&0x1f;}else if((c0&0xf0)==0xe0){extra=2;cp=c0&0x0f;}else if((c0&0xf8)==0xf0){extra=3;cp=c0&0x07;}else return 0xfffd;
    for(int i=0;i<extra;++i){if(index>=value.size())return 0xfffd;const unsigned char cx=static_cast<unsigned char>(value[index++]);if((cx&0xc0)!=0x80)return 0xfffd;cp=(cp<<6)|(cx&0x3f);}return cp;
}
int Gui::glyph_advance(std::uint32_t codepoint,int size) const {
    FT_Face face=resolve_face(codepoint);
    if(!face)return size/2;
    FT_Set_Pixel_Sizes(face,0,size);
    if(FT_Load_Char(face,codepoint,FT_LOAD_DEFAULT)!=0)return size/2;
    return static_cast<int>(face->glyph->advance.x>>6);
}
int Gui::text_width(int size,const std::string& value) const { int width=0;std::size_t i=0;while(i<value.size()){const auto cp=decode_utf8(value,i);if(cp=='\n')break;width+=glyph_advance(cp,size);}return width; }
void Gui::text(int x,int y,int size,Color color,const std::string& value,int max_width){
    if(!pixels_)return;int pen=x;std::size_t i=0;
    while(i<value.size()){
        std::uint32_t cp=decode_utf8(value,i);
        if(cp=='\n')break;
        FT_Face face=resolve_face(cp);
        if(!face)continue;
        FT_Set_Pixel_Sizes(face,0,size);
        if(FT_Load_Char(face,cp,FT_LOAD_RENDER)!=0)continue;
        auto& bm=face->glyph->bitmap;
        const int advance=static_cast<int>(face->glyph->advance.x>>6);
        if(max_width>0&&pen-x+advance>max_width)break;
        const int gx=pen+face->glyph->bitmap_left,gy=y+size-face->glyph->bitmap_top;
        for(unsigned row=0;row<bm.rows;++row)for(unsigned col=0;col<bm.width;++col){const unsigned char a=bm.buffer[row*bm.pitch+col];if(a){Color c=color;c.a=static_cast<std::uint8_t>((static_cast<unsigned>(color.a)*a)/255);put_pixel(gx+static_cast<int>(col),gy+static_cast<int>(row),c);}}
        pen+=advance;
    }
}
void Gui::paragraph(int x,int y,int size,Color color,const std::string& value,int max_width,int line_height,int max_lines){
    std::string line;int cy=y,lines=0;std::size_t i=0;
    auto flush=[&](){if(!line.empty()&&lines<max_lines){text(x,cy,size,color,line,max_width);cy+=line_height;++lines;line.clear();}};
    while(i<value.size()&&lines<max_lines){const std::size_t start=i;const auto cp=decode_utf8(value,i);if(cp=='\n'){flush();continue;}const std::string bytes=value.substr(start,i-start);if(text_width(size,line+bytes)>max_width&&!line.empty())flush();if(lines<max_lines)line+=bytes;}
    flush();
}
void Gui::divider(int x,int y,int w){fill_rect(x,y,w,1,palette::border);}
void Gui::button_hint(int x,int y,const std::string& button,const std::string& label,Color accent){
    const int bw=std::max(30,text_width(15,button)+18);round_rect(x,y,bw,28,14,accent);text(x+(bw-text_width(15,button))/2,y+5,15,palette::white,button);text(x+bw+10,y+5,15,palette::text,label);
}
void Gui::progress_bar(int x,int y,int w,int h,double ratio){round_rect(x,y,w,h,h/2,palette::border);const int fill=static_cast<int>(std::max(0.0,std::min(1.0,ratio))*w);if(fill>0)round_rect(x,y,fill,h,h/2,palette::accent);}
void Gui::image_rgb(int x,int y,int w,int h,const std::uint8_t* rgb,int src_w,int src_h){
    if(!pixels_||!rgb||w<=0||h<=0||src_w<=0||src_h<=0)return;
    for(int dy=0;dy<h;++dy){const int sy=std::min(src_h-1,dy*src_h/h);for(int dx=0;dx<w;++dx){const int sx=std::min(src_w-1,dx*src_w/w);const auto* p=rgb+(static_cast<std::size_t>(sy)*src_w+sx)*3;put_pixel(x+dx,y+dy,Color{p[0],p[1],p[2],255});}}
}
} // namespace astranas::ui
#endif

// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.hpp"
#ifdef __SWITCH__
#include <algorithm>
#include <cstdlib>
namespace astranas::ui {
namespace {
constexpr int kStickDeadzone=0x3800,kTouchDragThreshold=18,kTouchRowStep=38;
constexpr u64 kDirections=HidNpadButton_Left|HidNpadButton_Right|HidNpadButton_Up|HidNpadButton_Down;
u64 stick_directions(const PadState& pad,unsigned index){
    const HidAnalogStickState stick=padGetStickPos(&pad,index);
    u64 buttons=0;
    if(stick.x<=-kStickDeadzone)buttons|=HidNpadButton_Left;
    if(stick.x>= kStickDeadzone)buttons|=HidNpadButton_Right;
    if(stick.y>= kStickDeadzone)buttons|=HidNpadButton_Up;
    if(stick.y<=-kStickDeadzone)buttons|=HidNpadButton_Down;
    return buttons;
}
}
InputRouter::InputRouter(){padRepeaterInitialize(&repeater_,24,6);}
void InputRouter::initialize_touch(){if(touch_initialized_)return;hidInitializeTouchScreen();touch_initialized_=true;}
InputFrame InputRouter::update(PadState& pad){
    InputFrame frame;
    padUpdate(&pad);
    frame.down=padGetButtonsDown(&pad);
    frame.held=padGetButtons(&pad);

    const u64 physical=frame.held&kDirections;
    const u64 left_stick=stick_directions(pad,0);
    const u64 right_stick=stick_directions(pad,1);
    const u64 sticks=left_stick|right_stick;
    const u64 stick_down=sticks&~stick_held_;
    stick_held_=sticks;
    const u64 nav_held=physical|sticks;
    frame.nav_once=(frame.down&kDirections)|stick_down;
    padRepeaterUpdate(&repeater_,nav_held);
    frame.nav=frame.nav_once|padRepeaterGetButtons(&repeater_);

    if(!touch_initialized_)return frame;
    HidTouchScreenState state{};
    const size_t states=hidGetTouchScreenStates(&state,1);
    const bool now=states>0&&state.count>0;
    if(now){
        const int x=static_cast<int>(state.touches[0].x),y=static_cast<int>(state.touches[0].y);
        if(!touching_){touching_=true;touch_dragged_=false;touch_start_x_=touch_last_x_=x;touch_start_y_=touch_last_y_=y;}
        else{
            if(std::abs(x-touch_start_x_)>kTouchDragThreshold||std::abs(y-touch_start_y_)>kTouchDragThreshold)touch_dragged_=true;
            const int delta_y=y-touch_last_y_;
            if(std::abs(delta_y)>=kTouchRowStep){frame.touch_scroll_rows=-(delta_y/kTouchRowStep);touch_last_y_=y;}
            touch_last_x_=x;
            if(!touch_dragged_)touch_last_y_=y;
        }
    }else if(touching_){
        if(!touch_dragged_){frame.touch_tap=true;frame.touch_x=touch_last_x_;frame.touch_y=touch_last_y_;}
        touching_=false;touch_dragged_=false;
    }
    return frame;
}
}
#endif

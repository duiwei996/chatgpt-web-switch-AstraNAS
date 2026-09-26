// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include <switch.h>
namespace astranas::ui {
struct InputFrame {
    u64 down=0, held=0, nav=0, nav_once=0;
    bool touch_tap=false;
    int touch_x=0,touch_y=0,touch_scroll_rows=0;
};
class InputRouter {
public: InputRouter(); void initialize_touch(); InputFrame update(PadState& pad);
private:
    PadRepeater repeater_{};
    u64 stick_held_=0;
    bool touch_initialized_=false,touching_=false,touch_dragged_=false;
    int touch_start_x_=0,touch_start_y_=0,touch_last_x_=0,touch_last_y_=0;
};
}
#endif

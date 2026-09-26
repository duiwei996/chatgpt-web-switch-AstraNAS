// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/runtime_helpers.hpp"
#include <cassert>

int main() {
    using astranas::app::runtime_detail::BatchQueueTouchAction;
    using astranas::app::runtime_detail::batch_queue_touch_action;
    assert(batch_queue_touch_action(126, 646) == BatchQueueTouchAction::Start);
    assert(batch_queue_touch_action(430, 646) == BatchQueueTouchAction::Back);
    assert(batch_queue_touch_action(724, 646) == BatchQueueTouchAction::Remove);
    assert(batch_queue_touch_action(988, 646) == BatchQueueTouchAction::Clear);
    assert(batch_queue_touch_action(500, 620) == BatchQueueTouchAction::None);
    assert(batch_queue_touch_action(-1, 646) == BatchQueueTouchAction::None);
    assert(batch_queue_touch_action(1280, 646) == BatchQueueTouchAction::None);
    assert(batch_queue_touch_action(500, 720) == BatchQueueTouchAction::None);
    return 0;
}

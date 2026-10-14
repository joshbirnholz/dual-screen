// A list moved by a finger: it follows the finger by the pixel, carries on
// when a moving finger lifts and slows to a stop, stops when caught, and a
// still finger is a tap rather than a drag.
//
// ListDrag keeps its own time, so the passes here are a few milliseconds
// apart in real time, the way frames are.

#include "test_support.hpp"

#include <chrono>
#include <thread>
#include <vector>

#include "ui/list_touch.hpp"

using namespace ds;

namespace {

const PixelRect kList{0.0f, 100.0f, 400.0f, 500.0f};
constexpr float kMaxScroll = 2000.0f;

PointerEvent press(float y) {
    PointerEvent event;
    event.x = 200.0f;
    event.y = y;
    event.press_x = 200.0f;
    event.press_y = y;
    event.pressed = true;
    event.normalized = false;
    return event;
}

PointerEvent release(float y, float from) {
    PointerEvent event = press(from);
    event.pressed = false;
    event.released = true;
    event.y = y;
    return event;
}

// One pass, a frame's time after the last.
void pass(ListDrag& drag, const std::vector<PointerEvent>& pointers, float& scroll) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    drag.update(pointers, kList, scroll, kMaxScroll);
}

}  // namespace

int main() {
    // A tap: down and up in one place is not a drag, and moves nothing.
    {
        ListDrag drag;
        float scroll = 300.0f;
        set_live_pointer(true, 200.0f, 400.0f);
        pass(drag, {press(400.0f)}, scroll);
        set_live_pointer(false, 200.0f, 400.0f);
        pass(drag, {release(400.0f, 400.0f)}, scroll);
        CHECK(!drag.release_was_drag());
        CHECK(!drag.dragging());
        CHECK_EQ(scroll, 300.0f);
    }

    // A flick up the list: it follows the finger exactly, and once the finger
    // lifts it keeps going, slower and slower, until it stops.
    {
        ListDrag drag;
        float scroll = 0.0f;
        set_live_pointer(true, 200.0f, 500.0f);
        pass(drag, {press(500.0f)}, scroll);
        float y = 500.0f;
        for (int step = 0; step < 6; ++step) {
            y -= 30.0f;
            set_live_pointer(true, 200.0f, y);
            pass(drag, {}, scroll);
        }
        CHECK_EQ(scroll, 180.0f);
        CHECK(drag.dragging());
        set_live_pointer(false, 200.0f, y);
        pass(drag, {release(y, 500.0f)}, scroll);
        CHECK(drag.release_was_drag());
        drag.settle();
        const float at_release = scroll;
        pass(drag, {}, scroll);
        const float first_step = scroll - at_release;
        CHECK_MSG(first_step > 0.0f, "the list stopped the moment the finger lifted");
        float before = scroll;
        pass(drag, {}, scroll);
        CHECK_MSG(scroll - before < first_step, "the glide did not slow");
        for (int step = 0; step < 400 && drag.dragging(); ++step) pass(drag, {}, scroll);
        CHECK_MSG(!drag.dragging(), "the glide never stopped");
        before = scroll;
        pass(drag, {}, scroll);
        CHECK_EQ(scroll, before);
        CHECK(scroll <= kMaxScroll);
    }

    // A finger put down on a gliding list stops it there, and lifting it
    // again is not a tap on the row underneath.
    {
        ListDrag drag;
        float scroll = 1000.0f;
        set_live_pointer(true, 200.0f, 200.0f);
        pass(drag, {press(200.0f)}, scroll);
        for (float y = 240.0f; y <= 440.0f; y += 40.0f) {
            set_live_pointer(true, 200.0f, y);
            pass(drag, {}, scroll);
        }
        set_live_pointer(false, 200.0f, 440.0f);
        pass(drag, {release(440.0f, 200.0f)}, scroll);
        drag.settle();
        pass(drag, {}, scroll);
        CHECK(drag.dragging());
        set_live_pointer(true, 200.0f, 300.0f);
        pass(drag, {press(300.0f)}, scroll);
        const float caught = scroll;
        pass(drag, {}, scroll);
        CHECK_EQ(scroll, caught);
        set_live_pointer(false, 200.0f, 300.0f);
        pass(drag, {release(300.0f, 300.0f)}, scroll);
        CHECK(drag.release_was_drag());
        drag.settle();
        pass(drag, {}, scroll);
        CHECK_EQ(scroll, caught);
    }

    // It never goes past either end, and the cursor moving the list stops a
    // glide.
    {
        ListDrag drag;
        float scroll = 20.0f;
        set_live_pointer(true, 200.0f, 200.0f);
        pass(drag, {press(200.0f)}, scroll);
        set_live_pointer(true, 200.0f, 500.0f);
        pass(drag, {}, scroll);
        CHECK_EQ(scroll, 0.0f);
        set_live_pointer(true, 200.0f, 150.0f);
        pass(drag, {}, scroll);
        set_live_pointer(true, 200.0f, 110.0f);
        pass(drag, {}, scroll);
        set_live_pointer(false, 200.0f, 110.0f);
        pass(drag, {release(110.0f, 200.0f)}, scroll);
        drag.settle();
        drag.stop();
        const float stopped = scroll;
        pass(drag, {}, scroll);
        CHECK_EQ(scroll, stopped);
        CHECK(!drag.dragging());
    }

    // The mouse wheel over the list: a notch down moves it three rows down,
    // and a notch up three back; turned anywhere else it moves nothing.
    {
        ListDrag drag;
        drag.set_wheel_row(40.0f);
        float scroll = 300.0f;
        set_live_wheel(-1.0f, 200.0f, 300.0f);
        pass(drag, {}, scroll);
        CHECK_EQ(scroll, 420.0f);
        set_live_wheel(2.0f, 200.0f, 300.0f);
        pass(drag, {}, scroll);
        CHECK_EQ(scroll, 180.0f);
        set_live_wheel(-1.0f, 200.0f, 50.0f);  // above the list
        pass(drag, {}, scroll);
        CHECK_EQ(scroll, 180.0f);
        set_live_wheel(10.0f, 200.0f, 300.0f);  // no further than the top
        pass(drag, {}, scroll);
        CHECK_EQ(scroll, 0.0f);
        set_live_wheel(0.0f, 0.0f, 0.0f);
    }

    return test::finish("list touch");
}

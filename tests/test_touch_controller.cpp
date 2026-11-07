// The controller drawn over the game on a phone with no pad. See
// input/touch_controller.hpp.

#include "test_support.hpp"

#include <algorithm>
#include <string>

#include "input/touch_controller.hpp"

namespace {

using ds::SystemId;
using ds::TouchController;

const TouchController::Control* find(const TouchController& touch, const std::string& label) {
    for (const TouchController::Control& control : touch.controls()) {
        if (label == control.label) return &control;
    }
    return nullptr;
}

const TouchController::Control* find_kind(const TouchController& touch, TouchController::Kind kind,
                                          bool right = false) {
    for (const TouchController::Control& control : touch.controls()) {
        if (control.kind == kind && control.right == right) return &control;
    }
    return nullptr;
}

bool round(const ds::PixelRect& rect) { return rect.w == rect.h; }

// Two discs by their centres; anything else by its box.
bool overlap(const ds::PixelRect& a, const ds::PixelRect& b) {
    if (round(a) && round(b)) {
        const float dx = (a.x + a.w * 0.5f) - (b.x + b.w * 0.5f);
        const float dy = (a.y + a.h * 0.5f) - (b.y + b.h * 0.5f);
        const float reach = (a.w + b.w) * 0.5f;
        return dx * dx + dy * dy < reach * reach;
    }
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

void test_no_two_controls_overlap_and_all_are_on_screen() {
    const float sizes[][2] = {{1170, 540}, {2340, 1080}, {1280, 800}, {2048, 1536},
                              {540, 1170}, {800, 1280}, {1536, 2048}};
    for (const SystemId system : {SystemId::Ds, SystemId::ThreeDs, SystemId::WiiU}) {
        for (const auto& size : sizes) {
            TouchController touch;
            touch.arrange(size[0], size[1], ds::touch_controls_for(system));
            const auto& controls = touch.controls();
            CHECK(controls.size() >= 10);
            for (std::size_t i = 0; i < controls.size(); ++i) {
                const ds::PixelRect& a = controls[i].rect;
                // All of it under the game, which has the top half.
                CHECK_MSG(a.x >= 0.0f && a.y >= TouchController::game_height(size[1]) && a.x + a.w <= size[0] &&
                              a.y + a.h <= size[1],
                          std::string(controls[i].label) + " off screen at " + std::to_string(size[0]) + "x" +
                              std::to_string(size[1]));
                for (std::size_t j = i + 1; j < controls.size(); ++j) {
                    CHECK_MSG(!overlap(a, controls[j].rect),
                              std::string(controls[i].label) + "/" + controls[j].label + " overlap at " +
                                  std::to_string(size[0]) + "x" + std::to_string(size[1]) + " for system " +
                                  std::to_string(static_cast<int>(system)));
                }
            }
        }
    }
}

void test_each_console_gets_its_own_controls() {
    TouchController ds_touch;
    ds_touch.arrange(540, 1170, ds::touch_controls_for(SystemId::Ds));
    CHECK(find(ds_touch, "Start") != nullptr);
    CHECK(find(ds_touch, "ZL") == nullptr);
    CHECK(find_kind(ds_touch, TouchController::Kind::Stick) == nullptr);

    TouchController three_ds;
    three_ds.arrange(540, 1170, ds::touch_controls_for(SystemId::ThreeDs));
    CHECK(find_kind(three_ds, TouchController::Kind::Stick) != nullptr);
    CHECK(find_kind(three_ds, TouchController::Kind::Stick, true) == nullptr);

    TouchController wii_u;
    wii_u.arrange(540, 1170, ds::touch_controls_for(SystemId::WiiU));
    CHECK(find(wii_u, "ZR") != nullptr);
    CHECK(find(wii_u, "+") != nullptr);
    CHECK(find_kind(wii_u, TouchController::Kind::Stick, true) != nullptr);
}

void test_a_finger_on_a_button_presses_it_and_nothing_else_hears_it() {
    TouchController touch;
    touch.arrange(540, 1170, ds::touch_controls_for(SystemId::Ds));
    const TouchController::Control* a = find(touch, "A");
    CHECK(a != nullptr);
    if (a == nullptr) return;
    // Hidden, it takes nothing.
    CHECK(!touch.finger_down(1, a->centre_x(), a->centre_y()));
    touch.set_shown(true);
    CHECK(touch.finger_down(1, a->centre_x(), a->centre_y()));
    // A is the east button, which SDL calls B.
    CHECK(touch.raw().buttons[SDL_CONTROLLER_BUTTON_B]);
    CHECK(!touch.raw().buttons[SDL_CONTROLLER_BUTTON_A]);
    // A finger on the game, in the top half, is the game's.
    CHECK(!touch.finger_down(2, 270.0f, 300.0f));
    CHECK(touch.finger_up(1));
    CHECK(!touch.raw().buttons[SDL_CONTROLLER_BUTTON_B]);
    CHECK(!touch.finger_up(2));
}

void test_a_thumb_slides_from_one_face_button_to_the_next() {
    TouchController touch;
    touch.arrange(540, 1170, ds::touch_controls_for(SystemId::Ds));
    touch.set_shown(true);
    const TouchController::Control* b = find(touch, "B");
    const TouchController::Control* a = find(touch, "A");
    CHECK(touch.finger_down(1, b->centre_x(), b->centre_y()));
    CHECK(touch.raw().buttons[SDL_CONTROLLER_BUTTON_A]);
    touch.finger_motion(1, a->centre_x(), a->centre_y());
    CHECK(touch.raw().buttons[SDL_CONTROLLER_BUTTON_B]);
    CHECK(!touch.raw().buttons[SDL_CONTROLLER_BUTTON_A]);
    // Off the cluster altogether, nothing, until it comes back.
    touch.finger_motion(1, 10.0f, 300.0f);
    CHECK(!touch.raw().buttons[SDL_CONTROLLER_BUTTON_B]);
    touch.finger_motion(1, a->centre_x(), a->centre_y());
    CHECK(touch.raw().buttons[SDL_CONTROLLER_BUTTON_B]);
}

void test_the_dpad_reads_directions_and_diagonals() {
    CHECK_EQ(TouchController::dpad_directions(0.0f, 0.0f, 100.0f), 0u);
    CHECK_EQ(TouchController::dpad_directions(0.0f, -80.0f, 100.0f), 1u);
    CHECK_EQ(TouchController::dpad_directions(0.0f, 80.0f, 100.0f), 2u);
    CHECK_EQ(TouchController::dpad_directions(-80.0f, 0.0f, 100.0f), 4u);
    CHECK_EQ(TouchController::dpad_directions(80.0f, 0.0f, 100.0f), 8u);
    CHECK_EQ(TouchController::dpad_directions(60.0f, -60.0f, 100.0f), 1u | 8u);

    TouchController touch;
    touch.arrange(540, 1170, ds::touch_controls_for(SystemId::Ds));
    touch.set_shown(true);
    const TouchController::Control* dpad = find_kind(touch, TouchController::Kind::DPad);
    CHECK(touch.finger_down(1, dpad->centre_x(), dpad->centre_y() - dpad->radius() * 0.7f));
    CHECK(touch.raw().buttons[SDL_CONTROLLER_BUTTON_DPAD_UP]);
    // Slid well past the pad, it still holds it, pushed that way.
    touch.finger_motion(1, dpad->centre_x() + dpad->radius() * 2.0f, dpad->centre_y());
    CHECK(touch.raw().buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT]);
    CHECK(!touch.raw().buttons[SDL_CONTROLLER_BUTTON_DPAD_UP]);
}

void test_a_stick_is_pushed_as_far_as_the_finger_goes() {
    TouchController touch;
    touch.arrange(540, 1170, ds::touch_controls_for(SystemId::WiiU));
    touch.set_shown(true);
    const TouchController::Control* left = find_kind(touch, TouchController::Kind::Stick);
    const TouchController::Control* right = find_kind(touch, TouchController::Kind::Stick, true);
    CHECK(touch.finger_down(1, left->centre_x(), left->centre_y()));
    CHECK_EQ(touch.raw().axes[SDL_CONTROLLER_AXIS_LEFTX], 0);
    touch.finger_motion(1, left->centre_x() + left->radius() * 3.0f, left->centre_y());
    CHECK_EQ(touch.raw().axes[SDL_CONTROLLER_AXIS_LEFTX], 32767);
    CHECK_EQ(touch.raw().axes[SDL_CONTROLLER_AXIS_LEFTY], 0);
    CHECK(touch.finger_down(2, right->centre_x(), right->centre_y() - right->radius() * 0.375f));
    CHECK(touch.raw().axes[SDL_CONTROLLER_AXIS_RIGHTY] < -15000);
    CHECK(touch.raw().axes[SDL_CONTROLLER_AXIS_RIGHTY] > -17500);
    // ZR is a trigger, all the way.
    const TouchController::Control* zr = find(touch, "ZR");
    CHECK(touch.finger_down(3, zr->centre_x(), zr->centre_y()));
    CHECK_EQ(touch.raw().axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT], 32767);
}

void test_the_menu_control_asks_once_on_release() {
    TouchController touch;
    touch.arrange(540, 1170, ds::touch_controls_for(SystemId::Ds));
    touch.set_shown(true);
    const TouchController::Control* menu = find_kind(touch, TouchController::Kind::Menu);
    CHECK(touch.finger_down(5, menu->centre_x(), menu->centre_y()));
    CHECK(!touch.take_menu_press());
    CHECK(touch.finger_up(5));
    CHECK(touch.take_menu_press());
    CHECK(!touch.take_menu_press());
}

void test_zl_and_zr_sit_over_l_and_r() {
    TouchController touch;
    touch.arrange(1170, 2532, ds::touch_controls_for(SystemId::WiiU));
    CHECK(find(touch, "ZL")->rect.y < find(touch, "L")->rect.y);
    CHECK(find(touch, "ZR")->rect.y < find(touch, "R")->rect.y);
    // Under the game, and no smaller than they need to be: a phone's bottom
    // half has room for a GamePad's controls at nearly full size.
    float top = 2532.0f;
    for (const TouchController::Control& control : touch.controls()) top = std::min(top, control.rect.y);
    CHECK(top >= TouchController::game_height(2532.0f));
    CHECK(top < TouchController::game_height(2532.0f) + 1170.0f * 0.05f);
}

void test_hiding_it_lets_go_of_everything() {
    TouchController touch;
    touch.arrange(540, 1170, ds::touch_controls_for(SystemId::Ds));
    touch.set_shown(true);
    const TouchController::Control* l = find(touch, "L");
    CHECK(touch.finger_down(1, l->centre_x(), l->centre_y()));
    CHECK(touch.raw().buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER]);
    touch.set_shown(false);
    CHECK(!touch.held());
    CHECK(!touch.raw().buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER]);
    // Its finger lifting afterwards is nobody's.
    CHECK(!touch.finger_up(1));
}

}  // namespace

int main() {
    test_no_two_controls_overlap_and_all_are_on_screen();
    test_each_console_gets_its_own_controls();
    test_a_finger_on_a_button_presses_it_and_nothing_else_hears_it();
    test_a_thumb_slides_from_one_face_button_to_the_next();
    test_the_dpad_reads_directions_and_diagonals();
    test_a_stick_is_pushed_as_far_as_the_finger_goes();
    test_the_menu_control_asks_once_on_release();
    test_zl_and_zr_sit_over_l_and_r();
    test_hiding_it_lets_go_of_everything();
    return test::finish("touch-controller");
}

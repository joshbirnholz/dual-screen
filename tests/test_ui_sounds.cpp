#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "audio/ui_sounds.hpp"
#include "net/protocol.hpp"
#include "render/embedded_assets.hpp"
#include "test_support.hpp"
#include "ui/launcher_screen.hpp"

using namespace ds;

namespace {

void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

// A pack in the format make_ui_sounds.py writes, each sound `frames` long and
// every sample `value`.
std::vector<uint8_t> make_pack(const std::vector<std::string>& names, uint32_t frames, int16_t value) {
    std::vector<uint8_t> out = {'D', 'S', 'U', 'S'};
    put32(out, 1);
    put32(out, 48000);
    put32(out, static_cast<uint32_t>(names.size()));
    for (const std::string& name : names) {
        out.push_back(static_cast<uint8_t>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
        put32(out, frames);
    }
    for (std::size_t i = 0; i < names.size() * frames * 2; ++i) {
        out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value) & 0xff));
        out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8));
    }
    return out;
}

const std::vector<std::string> kAll = {"move_cursor", "confirm", "cancel", "error", "open_quick_menu"};

void test_the_carried_pack_has_every_sound_at_48k() {
    UiSoundPack pack;
    std::string error;
    CHECK_MSG(decode_ui_sound_pack(assets::kUiSounds, assets::kUiSounds_size, pack, &error),
              "the pack inside the binary did not decode: " + error);
    CHECK(pack.rate == 48000);
    for (int s = 0; s < kUiSoundCount; ++s) {
        const std::size_t frames = pack.clips[static_cast<std::size_t>(s)].size() / 2;
        // Trimmed to the sound: none of them is the second of silence the
        // recordings came with, and none is nothing.
        CHECK_MSG(frames > 48000 / 50 && frames < 48000,
                  std::string(ui_sound_name(static_cast<UiSound>(s))) + " is " + std::to_string(frames) +
                      " frames");
    }
}

void test_a_pack_cut_short_or_missing_a_sound_is_refused() {
    std::vector<uint8_t> whole = make_pack(kAll, 10, 100);
    UiSoundPack pack;
    std::string error;
    CHECK(decode_ui_sound_pack(whole.data(), whole.size(), pack, &error));
    CHECK(pack.clips[0].size() == 20 && pack.clips[0][0] == 100);

    std::vector<uint8_t> cut(whole.begin(), whole.end() - 2);
    CHECK(!decode_ui_sound_pack(cut.data(), cut.size(), pack, &error));
    CHECK_MSG(error.find("cut short") != std::string::npos, error);

    std::vector<std::string> all_but_one(kAll.begin(), kAll.end() - 1);
    std::vector<uint8_t> missing = make_pack(all_but_one, 10, 100);
    CHECK(!decode_ui_sound_pack(missing.data(), missing.size(), pack, &error));
    CHECK_MSG(error.find("open_quick_menu") != std::string::npos, error);

    std::vector<uint8_t> other = whole;
    other[4] = 2;
    CHECK(!decode_ui_sound_pack(other.data(), other.size(), pack, &error));
}

void test_the_mixer_sums_voices_and_clips() {
    std::vector<uint8_t> bytes = make_pack(kAll, 4, 20000);
    UiSoundPack pack;
    CHECK(decode_ui_sound_pack(bytes.data(), bytes.size(), pack, nullptr));
    UiSoundMixer mixer;
    mixer.set_pack(&pack);

    int16_t out[16] = {};
    mixer.mix(out, 8);
    CHECK_MSG(out[0] == 0, "silence with nothing playing");

    mixer.start(UiSound::Confirm);
    mixer.mix(out, 2);
    CHECK(out[0] == 20000 && out[3] == 20000);
    mixer.start(UiSound::Cancel);
    mixer.mix(out, 4);
    // Two frames left of the first, four of the second: summed, then bent
    // under full scale rather than cut off flat at it.
    CHECK_MSG(out[0] > 32000 && out[0] <= 32767, std::to_string(out[0]));
    CHECK(out[1] == out[0] && out[2] == out[0]);
    CHECK(out[4] == 20000);
    CHECK(mixer.playing() == 0);
}

void test_the_limit_leaves_ordinary_levels_alone_and_bends_loud_ones() {
    CHECK(UiSoundMixer::limit(10000) == 10000);
    CHECK(UiSoundMixer::limit(-24000) == -24000);
    CHECK(UiSoundMixer::limit(-50000) == -UiSoundMixer::limit(50000));
    int16_t last = 0;
    for (int32_t x = 0; x <= 100000; x += 500) {
        const int16_t y = UiSoundMixer::limit(x);
        CHECK_MSG(y >= last, "the limit never turns a louder sum into a quieter one");
        last = y;
    }
    CHECK(UiSoundMixer::limit(1000000) <= 32767);
}

void test_a_held_cursor_restarts_its_sound_rather_than_stacking() {
    std::vector<uint8_t> bytes = make_pack(kAll, 100, 1);
    UiSoundPack pack;
    CHECK(decode_ui_sound_pack(bytes.data(), bytes.size(), pack, nullptr));
    UiSoundMixer mixer;
    mixer.set_pack(&pack);
    for (int i = 0; i < 5; ++i) mixer.start(UiSound::MoveCursor);
    CHECK(mixer.playing() == 1);
    for (int i = 0; i < 20; ++i) mixer.start(UiSound::Confirm);
    CHECK_MSG(mixer.playing() == 8, "more sounds than voices takes the voices, and no more");
}

UiSoundView view_at(int screen, uint64_t focus, uint64_t rest = 0) {
    UiSoundView view;
    view.screen = screen;
    view.focus = focus;
    view.rest = rest;
    return view;
}

void test_a_press_sounds_only_when_it_changed_something() {
    UiSoundJudge judge;
    CHECK(!judge.observe(view_at(1, 0), 0.0));

    // The cursor moves: its sound.
    judge.note_input({UiAction::Down}, false, 0.1);
    auto sound = judge.observe(view_at(1, 1), 0.1);
    CHECK(sound && *sound == UiSound::MoveCursor);

    // At the end of the list it does not: nothing.
    judge.note_input({UiAction::Down}, false, 0.2);
    CHECK(!judge.observe(view_at(1, 1), 0.2));

    // A choice that opens another screen.
    judge.note_input({UiAction::Confirm}, false, 0.3);
    sound = judge.observe(view_at(2, 0), 0.3);
    CHECK(sound && *sound == UiSound::Confirm);

    // Back.
    judge.note_input({UiAction::Cancel}, false, 0.4);
    sound = judge.observe(view_at(1, 1), 0.4);
    CHECK(sound && *sound == UiSound::Cancel);

    // Nothing pressed, something changed: not this judge's to sound.
    CHECK(!judge.observe(view_at(1, 2), 0.5));
}

void test_a_press_waits_for_the_other_device_but_not_for_ever() {
    UiSoundJudge judge;
    judge.observe(view_at(1, 0), 0.0);
    judge.note_input({UiAction::Right}, false, 1.0);
    CHECK(!judge.observe(view_at(1, 0), 1.0));
    CHECK(!judge.observe(view_at(1, 0), 1.03));
    auto sound = judge.observe(view_at(1, 1), 1.05);
    CHECK_MSG(sound && *sound == UiSound::MoveCursor, "the change arrived a few frames after the press");

    judge.note_input({UiAction::Right}, false, 2.0);
    CHECK(!judge.observe(view_at(1, 1), 2.0));
    CHECK_MSG(!judge.observe(view_at(1, 2), 2.7), "a change long after the press is not the press's");
}

void test_a_tap_is_a_move_or_a_choice_by_what_it_did() {
    UiSoundJudge judge;
    judge.observe(view_at(1, 0), 0.0);
    judge.note_input({}, true, 0.1);
    auto sound = judge.observe(view_at(1, 3), 0.1);
    CHECK(sound && *sound == UiSound::MoveCursor);
    judge.note_input({}, true, 0.2);
    sound = judge.observe(view_at(1, 3, 7), 0.2);
    CHECK(sound && *sound == UiSound::Confirm);
}

void test_a_popups_cancel_sounds_like_backing_out() {
    UiSoundJudge judge;
    UiSoundView asking = view_at(1, 1, 5);
    asking.on_cancel = true;
    judge.observe(asking, 0.0);
    judge.note_input({UiAction::Confirm}, false, 0.1);
    auto sound = judge.observe(view_at(1, 1, 6), 0.1);
    CHECK_MSG(sound && *sound == UiSound::Cancel, "A on Cancel is B");

    UiSoundView on_quit = view_at(1, 2, 5);
    judge.observe(on_quit, 0.2);
    judge.note_input({UiAction::Confirm}, false, 0.3);
    sound = judge.observe(view_at(1, 2, 7), 0.3);
    CHECK(sound && *sound == UiSound::Confirm);
}

void test_the_quick_menu_and_errors_have_their_own_sounds() {
    UiSoundJudge judge;
    UiSoundView game = view_at(0, 0);
    game.in_game = true;
    UiSoundView menu = view_at(1, 0);
    menu.menu = true;
    judge.observe(game, 0.0);

    // A and B in a game are the game's.
    judge.note_input({UiAction::Confirm}, false, 0.1);
    CHECK(!judge.observe(game, 0.1));

    // Opened by whoever: the quick menu's own sound.
    auto sound = judge.observe(menu, 0.2);
    CHECK(sound && *sound == UiSound::OpenQuickMenu);
    judge.note_input({UiAction::Cancel}, false, 0.3);
    sound = judge.observe(game, 0.3);
    CHECK_MSG(sound && *sound == UiSound::Cancel, "closing the menu is backing out");

    // However it was closed: A on Continue, or the other device.
    judge.observe(menu, 0.32);
    judge.note_input({UiAction::Confirm}, false, 0.34);
    sound = judge.observe(game, 0.34);
    CHECK(sound && *sound == UiSound::Cancel);
    judge.observe(menu, 0.36);
    sound = judge.observe(game, 0.38);
    CHECK(sound && *sound == UiSound::Cancel);

    UiSoundView failed = view_at(5, 0);
    failed.error = true;
    judge.note_input({UiAction::Confirm}, false, 0.4);
    sound = judge.observe(failed, 0.4);
    CHECK(sound && *sound == UiSound::Error);
    CHECK_MSG(!judge.observe(failed, 0.5), "an error is sounded as it appears, once");
}

void test_the_launcher_describes_its_cursor() {
    LauncherScreen launcher;
    launcher.open(false);
    UiSoundView before;
    launcher.describe_for_sound(before);
    launcher.update({UiAction::Down}, {}, 1280.0f, 720.0f);
    UiSoundView after;
    launcher.describe_for_sound(after);
    CHECK(after.focus != before.focus);
    CHECK(after.rest == before.rest);
    // Pushed past the top, the cursor stays -- and so does what it says.
    LauncherScreen top;
    top.open(false);
    UiSoundView a;
    top.describe_for_sound(a);
    top.update({UiAction::Up}, {}, 1280.0f, 720.0f);
    UiSoundView b;
    top.describe_for_sound(b);
    CHECK(a.focus == b.focus && a.rest == b.rest);
}

void test_a_sound_crosses_the_wire_as_itself() {
    const std::pair<UiSound, net::MenuSound> pairs[] = {
        {UiSound::MoveCursor, net::MenuSound::MoveCursor},
        {UiSound::Confirm, net::MenuSound::Confirm},
        {UiSound::Cancel, net::MenuSound::Cancel},
        {UiSound::Error, net::MenuSound::Error},
        {UiSound::OpenQuickMenu, net::MenuSound::OpenQuickMenu},
    };
    for (const auto& [sound, wire] : pairs) {
        CHECK(ui_sound_to_wire(sound) == static_cast<uint8_t>(wire));
        CHECK(ui_sound_from_wire(static_cast<uint8_t>(wire)) == sound);
    }
    CHECK(!ui_sound_from_wire(0) && !ui_sound_from_wire(6));
    const uint8_t unknown = 9;
    net::MenuSoundMessage back;
    CHECK(!net::decode_menu_sound(&unknown, 1, back));
}

}  // namespace

int main() {
    test_the_carried_pack_has_every_sound_at_48k();
    test_a_pack_cut_short_or_missing_a_sound_is_refused();
    test_the_mixer_sums_voices_and_clips();
    test_the_limit_leaves_ordinary_levels_alone_and_bends_loud_ones();
    test_a_held_cursor_restarts_its_sound_rather_than_stacking();
    test_a_press_sounds_only_when_it_changed_something();
    test_a_press_waits_for_the_other_device_but_not_for_ever();
    test_a_tap_is_a_move_or_a_choice_by_what_it_did();
    test_the_quick_menu_and_errors_have_their_own_sounds();
    test_a_popups_cancel_sounds_like_backing_out();
    test_the_launcher_describes_its_cursor();
    test_a_sound_crosses_the_wire_as_itself();
    return test::finish("ui_sounds");
}

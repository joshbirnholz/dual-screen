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

const std::vector<std::string> kAll = {"move_cursor",     "confirm",    "cancel",    "error",
                                       "open_quick_menu", "save_state", "toggle_on", "toggle_off",
                                       "bump"};

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

// A style the pack does not have plays the one it has.
void test_a_style_the_pack_lacks_plays_the_one_it_has() {
    UiSoundPack first;
    std::string error;
    CHECK(decode_ui_sound_pack(assets::kUiSounds, assets::kUiSounds_size, first, &error, "click"));
    UiSoundPack unknown;
    CHECK(decode_ui_sound_pack(assets::kUiSounds, assets::kUiSounds_size, unknown, &error, "kazoo"));
    CHECK(unknown.clips[1] == first.clips[1]);
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
    CHECK_MSG(error.find("bump") != std::string::npos, error);

    std::vector<uint8_t> other = whole;
    other[4] = 3;
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

    // At the end of the list it does not: nothing, the cursor not being a
    // ring here to bump -- see test_a_ring_against_the_end_bumps().
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

// A save state made chimes, in place of the A press's own sound -- whichever
// device's pad pressed it, and with no press here at all.
void test_a_save_state_made_chimes() {
    UiSoundJudge judge;
    judge.observe(view_at(1, 2), 0.0);
    judge.note_input({UiAction::Confirm}, false, 0.1);
    UiSoundView saved = view_at(1, 2, 9);
    saved.saves = 1;
    auto sound = judge.observe(saved, 0.1);
    CHECK_MSG(sound && *sound == UiSound::SaveState, "a save state made did not chime");
    CHECK_MSG(!judge.observe(saved, 0.15), "the A press sounded as well, after the chime");
    UiSoundView again = saved;
    again.saves = 2;
    sound = judge.observe(again, 0.5);
    CHECK(sound && *sound == UiSound::SaveState);
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

// A switch flipped sounds which way it went; a press that does anything else
// on a switch's row -- moves off it, opens a page -- sounds as it always did.
void test_a_switch_flipped_sounds_on_or_off() {
    UiSoundJudge judge;
    UiSoundView off = view_at(1, 4, 1);
    off.toggle = 0;
    UiSoundView on = view_at(1, 4, 2);
    on.toggle = 1;
    judge.observe(off, 0.0);
    judge.note_input({UiAction::Confirm}, false, 0.1);
    auto sound = judge.observe(on, 0.1);
    CHECK_MSG(sound && *sound == UiSound::ToggleOn, "a switch turned on did not sound it");
    judge.note_input({}, true, 0.2);
    sound = judge.observe(off, 0.2);
    CHECK_MSG(sound && *sound == UiSound::ToggleOff, "a switch tapped off did not sound it");
    // A direction never flips a switch; were one to change under it, that
    // is not a switch's sound.
    judge.note_input({UiAction::Right}, false, 0.3);
    sound = judge.observe(on, 0.3);
    CHECK_MSG(!sound || (*sound != UiSound::ToggleOn && *sound != UiSound::ToggleOff),
              "a direction sounded a switch");

    // Moving onto a switch that is the other way is a move.
    UiSoundView next_row = view_at(1, 5, 2);
    next_row.toggle = 0;
    judge.note_input({UiAction::Down}, false, 0.4);
    sound = judge.observe(next_row, 0.4);
    CHECK(sound && *sound == UiSound::MoveCursor);

    // A switch flipped with no press here -- the other device's -- is not
    // this judge's to sound, as with any other change.
    UiSoundView flipped = view_at(1, 5, 3);
    flipped.toggle = 1;
    CHECK(!judge.observe(flipped, 0.5));

    // A choice that changes something but not the switch is a choice.
    UiSoundView still_on = view_at(1, 5, 4);
    still_on.toggle = 1;
    judge.note_input({UiAction::Confirm}, false, 0.6);
    sound = judge.observe(still_on, 0.6);
    CHECK(sound && *sound == UiSound::Confirm);
}

UiSoundView ringed(int screen, uint64_t focus, uint64_t rest = 0) {
    UiSoundView view = view_at(screen, focus, rest);
    view.ring = true;
    return view;
}

// A direction that moves nothing while the cursor is a ring bumps, that way;
// once for a press, not on and on for one held there.
void test_a_ring_against_the_end_bumps() {
    UiSoundJudge judge;
    judge.observe(ringed(1, 0), 0.0);
    judge.note_input({UiAction::Up}, false, 0.1);
    auto sound = judge.observe(ringed(1, 0), 0.1);
    CHECK_MSG(sound && *sound == UiSound::Bump, "a ring pushed past the top did not bump");
    CHECK(judge.bump_direction() == UiAction::Up);
    // Only on the view just after: nothing changing later is no second bump.
    CHECK(!judge.observe(ringed(1, 0), 0.15));

    // Held there, repeating: no more bumps, said so or not.
    judge.note_input({UiAction::Right}, false, 1.0);
    sound = judge.observe(ringed(1, 0), 1.0);
    CHECK(sound && *sound == UiSound::Bump && judge.bump_direction() == UiAction::Right);
    judge.note_input({UiAction::Right}, false, 1.4, true);
    CHECK_MSG(!judge.observe(ringed(1, 0), 1.4), "a held direction went on bumping");
    judge.note_input({UiAction::Right}, false, 1.475);
    CHECK_MSG(!judge.observe(ringed(1, 0), 1.475), "the other device's held direction went on bumping");
    // Pressed again, after letting go: a bump again.
    judge.note_input({UiAction::Right}, false, 2.0);
    sound = judge.observe(ringed(1, 0), 2.0);
    CHECK(sound && *sound == UiSound::Bump);

    // A direction that moves the cursor is a move; one that changes
    // anything else is not a bump.
    judge.note_input({UiAction::Down}, false, 2.5);
    sound = judge.observe(ringed(1, 1), 2.5);
    CHECK(sound && *sound == UiSound::MoveCursor);
    judge.note_input({UiAction::Down}, false, 3.0);
    sound = judge.observe(ringed(1, 1, 7), 3.0);
    CHECK(!sound || *sound != UiSound::Bump);

    // The cursor not a ring -- the quick menu's sections -- never bumps.
    judge.observe(view_at(1, 1), 3.5);
    judge.note_input({UiAction::Up}, false, 4.0);
    CHECK_MSG(!judge.observe(view_at(1, 1), 4.0), "a cursor that is not a ring bumped");
    // Nor does A that does nothing.
    judge.observe(ringed(1, 1), 4.5);
    judge.note_input({UiAction::Confirm}, false, 5.0);
    CHECK(!judge.observe(ringed(1, 1), 5.0));
}

// A direction held down a list bumps once as it reaches the end, then
// rests there; back up and down to the end again, once more.
void test_a_held_direction_bumps_once_at_the_end() {
    UiSoundJudge down;
    std::optional<UiSound> sound;
    down.observe(ringed(1, 0), 1.0);
    // Held down a list, the cursor moving, then reaching the end: one bump
    // as it gets there, and none after.
    down.note_input({UiAction::Down}, false, 1.6);
    sound = down.observe(ringed(1, 1), 1.6);
    CHECK(sound && *sound == UiSound::MoveCursor);
    down.note_input({UiAction::Down}, false, 2.0, true);
    sound = down.observe(ringed(1, 2), 2.0);
    CHECK(sound && *sound == UiSound::MoveCursor);
    down.note_input({UiAction::Down}, false, 2.075, true);
    sound = down.observe(ringed(1, 2), 2.075);
    CHECK_MSG(sound && *sound == UiSound::Bump, "a held direction reaching the end did not bump");
    down.note_input({UiAction::Down}, false, 2.15, true);
    CHECK_MSG(!down.observe(ringed(1, 2), 2.15), "a held direction at the end bumped twice");
    // Held up from there and back down to the end: once more.
    down.note_input({UiAction::Up}, false, 2.6);
    down.observe(ringed(1, 1), 2.6);
    down.note_input({UiAction::Down}, false, 3.0);
    down.observe(ringed(1, 2), 3.0);
    down.note_input({UiAction::Down}, false, 3.4, true);
    sound = down.observe(ringed(1, 2), 3.4);
    CHECK(sound && *sound == UiSound::Bump);

}

void test_a_bumps_way_crosses_the_wire() {
    for (const UiAction way : {UiAction::Up, UiAction::Down, UiAction::Left, UiAction::Right}) {
        net::MenuSoundMessage sent;
        sent.sound = net::MenuSound::Bump;
        sent.bump_direction = bump_direction_to_wire(way);
        std::vector<uint8_t> buffer;
        net::encode_menu_sound(sent, buffer);
        // Past the length and the type.
        net::MenuSoundMessage back;
        CHECK(net::decode_menu_sound(buffer.data() + 6, buffer.size() - 6, back));
        CHECK(back.sound == net::MenuSound::Bump);
        CHECK(bump_direction_from_wire(back.bump_direction) == way);
    }
    CHECK(!bump_direction_from_wire(0) && !bump_direction_from_wire(5));
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
        {UiSound::SaveState, net::MenuSound::SaveState},
        {UiSound::ToggleOn, net::MenuSound::ToggleOn},
        {UiSound::ToggleOff, net::MenuSound::ToggleOff},
        {UiSound::Bump, net::MenuSound::Bump},
    };
    for (const auto& [sound, wire] : pairs) {
        CHECK(ui_sound_to_wire(sound) == static_cast<uint8_t>(wire));
        CHECK(ui_sound_from_wire(static_cast<uint8_t>(wire)) == sound);
    }
    CHECK(!ui_sound_from_wire(0) && !ui_sound_from_wire(10));
    const uint8_t unknown = 10;
    net::MenuSoundMessage back;
    CHECK(!net::decode_menu_sound(&unknown, 1, back));
}

}  // namespace

int main() {
    test_the_carried_pack_has_every_sound_at_48k();
    test_a_style_the_pack_lacks_plays_the_one_it_has();
    test_a_pack_cut_short_or_missing_a_sound_is_refused();
    test_the_mixer_sums_voices_and_clips();
    test_the_limit_leaves_ordinary_levels_alone_and_bends_loud_ones();
    test_a_held_cursor_restarts_its_sound_rather_than_stacking();
    test_a_press_sounds_only_when_it_changed_something();
    test_a_press_waits_for_the_other_device_but_not_for_ever();
    test_a_tap_is_a_move_or_a_choice_by_what_it_did();
    test_the_quick_menu_and_errors_have_their_own_sounds();
    test_a_popups_cancel_sounds_like_backing_out();
    test_a_save_state_made_chimes();
    test_a_switch_flipped_sounds_on_or_off();
    test_a_ring_against_the_end_bumps();
    test_a_held_direction_bumps_once_at_the_end();
    test_a_bumps_way_crosses_the_wire();
    test_the_launcher_describes_its_cursor();
    test_a_sound_crosses_the_wire_as_itself();
    return test::finish("ui_sounds");
}

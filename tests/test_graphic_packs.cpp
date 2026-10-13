// The Wii U quick menu's Graphics Packs page, driven the way a player drives
// it -- presses in, the changes it asks Cemu for out: how the packs are
// grouped, what a press on a pack and on a preset asks for, where the cursor
// goes when Cemu's answer arrives, and when the page says a change waits for
// the game to be reset. Nothing here draws, so none of it needs a GL context.

#include "test_support.hpp"

#include <string>
#include <vector>

#include "ui/graphic_packs_screen.hpp"

using namespace ds;

namespace {

constexpr float kWidth = 1280.0f;
constexpr float kHeight = 800.0f;

// As Cemu lists a game with some of the community's packs installed, in the
// order it happens to have loaded them.
std::vector<ExternalGraphicPack> game_packs() {
    std::vector<ExternalGraphicPack> packs(4);
    packs[0].id = "fps";
    packs[0].path = "Some Game/Mods/FPS++";
    packs[0].name = "FPS++";
    packs[0].choices.push_back({"", {"30FPS", "60FPS"}, 0});
    packs[1].id = "graphics";
    packs[1].path = "Some Game/Graphics";
    packs[1].name = "Graphics";
    packs[1].enabled = true;
    packs[1].restart_needed = true;
    packs[1].choices.push_back({"TV Resolution", {"720p", "1080p", "1440p"}, 1});
    packs[1].choices.push_back({"Shadows", {"Low", "High"}, -1});
    packs[2].id = "bloom";
    packs[2].path = "Some Game/Enhancements/No Bloom";
    packs[2].name = "No Bloom";
    packs[3].id = "aa";
    packs[3].path = "Some Game/Enhancements/Anti-Aliasing";
    packs[3].name = "Anti-Aliasing";
    return packs;
}

GraphicPacksScreenResult press(GraphicPacksScreen& screen, UiAction action) {
    return screen.update({action}, {}, kWidth, kHeight);
}

std::vector<std::string> describe(const std::vector<GraphicPackLine>& lines) {
    std::vector<std::string> out;
    for (const GraphicPackLine& line : lines) {
        const char* kind = line.kind == GraphicPackLine::Kind::Heading ? "# "
                           : line.kind == GraphicPackLine::Kind::Pack  ? ""
                                                                       : "  ";
        out.push_back(kind + line.text);
    }
    return out;
}

void test_packs_are_grouped_as_cemu_files_them() {
    const std::vector<std::string> lines = describe(graphic_pack_lines(game_packs()));
    // The game's name, which every pack shares, is left off; packs in no
    // group come first; each group alphabetically; a pack that is on shows
    // its presets, one with no name of its own called "Preset".
    const std::vector<std::string> expected = {
        "Graphics",       "  TV Resolution", "  Shadows",
        "# Enhancements", "Anti-Aliasing",   "No Bloom",
        "# Mods",         "FPS++",
    };
    CHECK_MSG(lines == expected, "the packs were not laid out as Cemu's window files them");

    std::vector<ExternalGraphicPack> packs = game_packs();
    packs[0].enabled = true;
    const std::vector<std::string> with_fps = describe(graphic_pack_lines(packs));
    CHECK(with_fps.back() == "  Preset");
}

void test_a_pack_is_turned_on_and_its_presets_appear() {
    GraphicPacksScreen screen;
    screen.open();
    CHECK(screen.is_open());
    // Nothing to press until Cemu answers.
    CHECK(!press(screen, UiAction::Confirm).change);
    screen.set_packs(game_packs());

    // Down past Graphics' two presets to Anti-Aliasing.
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    GraphicPacksScreenResult result = press(screen, UiAction::Confirm);
    CHECK_MSG(result.change.has_value(), "A on a pack asked Cemu for nothing");
    if (result.change) {
        CHECK(result.change->id == "aa");
        CHECK(result.change->kind == ExternalGraphicPackChange::Kind::Enabled);
        CHECK(result.change->enabled);
    }
    CHECK_MSG(!screen.restart_pending(), "a pack that changes at once said it waits for a reset");
    // Until Cemu, having read it on the way in, answers that it does.
    std::vector<ExternalGraphicPack> answered = game_packs();
    answered[3].enabled = true;
    answered[3].restart_needed = true;
    screen.set_packs(answered);
    CHECK_MSG(screen.restart_pending(), "Cemu's word that the pack waits for a reset went unsaid");

    // Left turns it off again, as either direction flips a two-value row.
    result = press(screen, UiAction::Left);
    CHECK(result.change && !result.change->enabled);

    // FPS++ on: its preset shows under it, and the cursor stays on the pack.
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    CHECK_EQ(screen.focus(), 5);
    result = press(screen, UiAction::Confirm);
    CHECK(result.change && result.change->id == "fps" && result.change->enabled);
    CHECK_EQ(screen.focus(), 5);
    press(screen, UiAction::Down);
    CHECK_EQ(screen.focus(), 6);
    // The list does not wrap.
    press(screen, UiAction::Down);
    CHECK_EQ(screen.focus(), 6);
    result = press(screen, UiAction::Right);
    CHECK_MSG(result.change.has_value(), "a preset row asked for nothing");
    if (result.change) {
        CHECK(result.change->kind == ExternalGraphicPackChange::Kind::Preset);
        CHECK(result.change->category.empty());
        CHECK(result.change->preset == "60FPS");
    }
}

void test_presets_step_both_ways_and_say_when_they_wait_for_a_reset() {
    GraphicPacksScreen screen;
    screen.open();
    screen.set_packs(game_packs());
    press(screen, UiAction::Down);  // TV Resolution, on 1080p
    GraphicPacksScreenResult result = press(screen, UiAction::Right);
    CHECK(result.change && result.change->category == "TV Resolution" &&
          result.change->preset == "1440p");
    // Values go round, as Settings' do.
    result = press(screen, UiAction::Right);
    CHECK(result.change && result.change->preset == "720p");
    result = press(screen, UiAction::Left);
    CHECK(result.change && result.change->preset == "1440p");
    CHECK_MSG(screen.restart_pending(), "a resolution pack's change did not say it waits for a reset");

    // None chosen yet counts from the first, which is what is shown.
    press(screen, UiAction::Down);
    result = press(screen, UiAction::Confirm);
    CHECK(result.change && result.change->category == "Shadows" && result.change->preset == "High");
}

void test_the_cursor_stays_put_when_cemu_answers() {
    GraphicPacksScreen screen;
    screen.open();
    screen.set_packs(game_packs());
    // On Shadows, Graphics' second preset.
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    CHECK_EQ(screen.focus(), 2);

    // Cemu's answer, with FPS++ turned on elsewhere and the packs in a new
    // order: still on Shadows.
    std::vector<ExternalGraphicPack> packs = game_packs();
    packs[0].enabled = true;
    std::swap(packs[0], packs[3]);
    screen.set_packs(packs);
    CHECK_EQ(screen.focus(), 2);

    // Shadows hidden by a condition: the nearest row, not off the end.
    packs[3].choices.pop_back();
    for (ExternalGraphicPack& pack : packs) {
        if (pack.id == "graphics") pack.choices.pop_back();
    }
    screen.set_packs(packs);
    CHECK(screen.focus() >= 0 && screen.focus() < 7);

    // Nothing at all for this game: no row, and A asks for nothing.
    screen.set_packs({});
    CHECK_EQ(screen.focus(), 0);
    CHECK(!press(screen, UiAction::Confirm).change);
}

void test_back_and_the_menu_button_leave() {
    GraphicPacksScreen screen;
    screen.open();
    screen.set_packs(game_packs());
    CHECK(press(screen, UiAction::Cancel).close_requested);
    CHECK(!screen.is_open());
    screen.open();
    CHECK(press(screen, UiAction::ToggleMenu).close_requested);
    // A menu's face buttons are not this page's.
    screen.open();
    screen.set_packs(game_packs());
    const GraphicPacksScreenResult result = press(screen, UiAction::MenuQuit);
    CHECK(!result.close_requested && !result.change);
}

}  // namespace

int main() {
    test_packs_are_grouped_as_cemu_files_them();
    test_a_pack_is_turned_on_and_its_presets_appear();
    test_presets_step_both_ways_and_say_when_they_wait_for_a_reset();
    test_the_cursor_stays_put_when_cemu_answers();
    test_back_and_the_menu_button_leave();
    return test::finish("graphic-packs");
}

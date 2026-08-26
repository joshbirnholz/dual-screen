#include <SDL.h>

#include <vector>

#include "audio/ui_sounds.hpp"
#include "render/embedded_assets.hpp"
#include "test_support.hpp"

using namespace ds;

namespace {

void test_every_sound_the_shell_asks_for_is_in_the_pack() {
    std::vector<const int16_t*> samples;
    std::vector<std::size_t> frames;
    CHECK_MSG(decode_ui_sound_pack(assets::kUiSoundsBin, assets::kUiSoundsBin_size, samples, frames),
              "the packed interface sounds could not be read");
    CHECK_EQ(samples.size(), static_cast<std::size_t>(UiSound::Count));

    // A silent or missing effect would still "play", so each one is checked for
    // having something in it rather than merely being present.
    for (std::size_t index = 0; index < samples.size(); ++index) {
        CHECK_MSG(frames[index] > 1000,
                  "sound " + std::to_string(index) + " is only " +
                      std::to_string(frames[index]) + " samples long");
        int loudest = 0;
        for (std::size_t step = 0; step < frames[index]; ++step) {
            const int level = samples[index][step] < 0 ? -samples[index][step] : samples[index][step];
            if (level > loudest) loudest = level;
        }
        CHECK_MSG(loudest > 1000,
                  "sound " + std::to_string(index) + " never gets above " +
                      std::to_string(loudest) + ", which is silence");
    }
}

void test_a_pack_that_is_not_one_is_refused() {
    std::vector<const int16_t*> samples;
    std::vector<std::size_t> frames;
    const uint8_t nonsense[] = {'N', 'O', 'P', 'E', 1, 0, 0, 0};
    CHECK_MSG(!decode_ui_sound_pack(nonsense, sizeof(nonsense), samples, frames),
              "something that is not a sound pack was read as one");

    // Cut with its index intact but most of its samples missing, so the entry
    // bounds are what has to catch it rather than the header length: the index
    // says where samples are that are not there, and reading past the end of
    // the pack would be reading past the end of the binary.
    const std::size_t past_the_index = 8 + static_cast<std::size_t>(UiSound::Count) * 8 + 128;
    std::vector<uint8_t> cut(assets::kUiSoundsBin, assets::kUiSoundsBin + past_the_index);
    CHECK_MSG(!decode_ui_sound_pack(cut.data(), cut.size(), samples, frames),
              "a pack cut short was read as if all of it had arrived");
}

// The shell's own effects come out of whichever device the game's sound does.
// Playing them on both, side by side, is one click too many.
void test_muting_silences_what_is_already_sounding() {
    // Nobody has brought SDL's audio up: the app asks for video and events at
    // startup and the game's own output only opens a device once a game runs,
    // so the player has to be able to stand one up by itself or a launcher
    // makes no sound at all.
    UiSounds sounds;
    std::string error;
    CHECK_MSG(sounds.initialize(&error),
              "the sound player could not open a device on its own: " + error);
    CHECK_MSG(!sounds.muted(), "a fresh player started out silent");

    sounds.play(UiSound::Confirm);
    CHECK_MSG(sounds.sounding(), "asking for an effect did not start one");

    sounds.set_muted(true);
    CHECK_MSG(!sounds.sounding(),
              "an effect kept sounding after this device stopped being the one making sound");
    // And nothing new starts while it stays that way.
    sounds.play(UiSound::MoveCursor);
    sounds.start_loop(UiSound::ConnectingLoop);
    CHECK_MSG(!sounds.sounding(), "a muted device still started an effect");

    sounds.set_muted(false);
    sounds.play(UiSound::Confirm);
    CHECK_MSG(sounds.sounding(), "the device would not make a sound again");
    sounds.shutdown();
}

}  // namespace

int main() {
    // The dummy driver, as the rest of the suite uses. Deliberately no
    // SDL_Init here: bringing the subsystem up is the player's own job.
    SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
    test_every_sound_the_shell_asks_for_is_in_the_pack();
    test_a_pack_that_is_not_one_is_refused();
    test_muting_silences_what_is_already_sounding();
    SDL_Quit();
    return test::finish("ui-sounds");
}

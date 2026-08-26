#include <SDL.h>

#include <vector>

#include "audio/audio_output.hpp"
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

// The effects are mixed into whatever buffer is about to be played, at that
// buffer's rate, so they cost no device of their own.
void test_effects_are_mixed_into_someone_elses_buffer() {
    UiSounds sounds;
    std::string error;
    CHECK(sounds.initialize(&error));
    sounds.play(UiSound::Confirm);
    CHECK(sounds.sounding());

    // The rate a DS core actually asks for, which is nothing like the one the
    // effects are packed at: silence in, something audible out.
    constexpr int kCoreRate = 32728;
    constexpr std::size_t kFrames = 512;
    std::vector<int16_t> buffer(kFrames * 2, 0);
    sounds.render_into(buffer.data(), kFrames, kCoreRate, 2);

    int loudest = 0;
    for (const int16_t sample : buffer) {
        const int level = sample < 0 ? -sample : sample;
        if (level > loudest) loudest = level;
    }
    CHECK_MSG(loudest > 0, "nothing was mixed into the buffer the game was about to play");

    // Both channels carry it, rather than one being left silent.
    int left = 0;
    int right = 0;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        left += buffer[frame * 2] != 0 ? 1 : 0;
        right += buffer[frame * 2 + 1] != 0 ? 1 : 0;
    }
    CHECK_MSG(left > 0 && left == right, "the effect came out of one channel only");

    // And a muted device puts nothing into it at all.
    sounds.set_muted(true);
    sounds.play(UiSound::Confirm);
    std::vector<int16_t> quiet(kFrames * 2, 0);
    sounds.render_into(quiet.data(), kFrames, kCoreRate, 2);
    for (const int16_t sample : quiet) {
        CHECK_MSG(sample == 0, "a muted device mixed an effect into the game's buffer");
    }
    sounds.shutdown();
}

// The Switch has one audio device and the game's sound has the better claim on
// it. Asking for one while the effects hold it has to succeed.
void test_the_game_can_take_the_only_device() {
    UiSounds sounds;
    std::string error;
    CHECK(sounds.initialize(&error));

    AudioOutput game;
    game.set_effects(&sounds);
    CHECK_MSG(game.open(32728.0, &error),
              "the game could not open a device the effects were holding: " + error);
    CHECK(game.is_open());

    // Still audible: they are mixed into the game's stream from here.
    sounds.play(UiSound::Confirm);
    CHECK_MSG(sounds.sounding(), "the effects went quiet once the game took the device");

    game.close();
    // And they are audible on their own again once the game is done.
    sounds.play(UiSound::Cancel);
    CHECK(sounds.sounding());
    sounds.shutdown();
}

// The launcher makes its sounds through a device of its own, before any game
// has opened one. That path has to actually reach the hardware: an effect that
// is started and never rendered leaves a menu silent.
void test_a_device_of_its_own_actually_plays() {
    UiSounds sounds;
    std::string error;
    CHECK_MSG(sounds.initialize(&error),
              "the sound player could not open a device on its own: " + error);
    CHECK_MSG(sounds.device_rate() > 0 && sounds.device_channels() > 0,
              "a device was opened without saying what shape it is, so the callback cannot know "
              "how much of the buffer is one frame");

    sounds.play(UiSound::Confirm);
    CHECK(sounds.sounding());

    // Rendered by the device's own callback thread, in real time: the effect
    // runs out only if something is pulling it. A second is far longer than any
    // of them and far longer than this needs.
    const uint32_t gave_up_at = SDL_GetTicks() + 2000;
    while (sounds.sounding() && SDL_GetTicks() < gave_up_at) {
        SDL_Delay(10);
    }
    CHECK_MSG(!sounds.sounding(),
              "the device never rendered a frame of the effect, so nothing came out of it");
    sounds.shutdown();
}

// The game's tail is dropped when the menu goes up, so the sound of it going up
// is not queued behind a quarter second of a game that has already stopped.
void test_dropping_the_games_tail_lets_an_effect_out() {
    UiSounds sounds;
    std::string error;
    CHECK(sounds.initialize(&error));

    AudioOutput game;
    game.set_effects(&sounds);
    constexpr int kCoreRate = 32728;
    CHECK_MSG(game.open(static_cast<double>(kCoreRate), &error), "no device for the game: " + error);

    // A queue as deep as one gets before the game pauses.
    const std::size_t deep = static_cast<std::size_t>(kCoreRate) / 5;
    std::vector<int16_t> tail(deep * 2, 0);
    game.push(tail.data(), deep);
    const std::size_t behind = game.queued_frames();
    CHECK_MSG(behind > 0, "nothing was queued, so there is no tail to be stuck behind");

    sounds.play(UiSound::OpenQuickMenu);
    game.pump_effects();
    CHECK_MSG(game.queued_frames() <= behind,
              "an effect was queued behind the game's tail rather than waiting for it to drain");

    game.flush();
    CHECK_EQ(game.queued_frames(), static_cast<std::size_t>(0));
    game.pump_effects();
    CHECK_MSG(game.queued_frames() > 0,
              "the effect still would not go out once the game's tail was dropped");

    game.close();
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
    test_effects_are_mixed_into_someone_elses_buffer();
    test_the_game_can_take_the_only_device();
    test_a_device_of_its_own_actually_plays();
    test_dropping_the_games_tail_lets_an_effect_out();
    SDL_Quit();
    return test::finish("ui-sounds");
}

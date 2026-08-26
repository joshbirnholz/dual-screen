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

}  // namespace

int main() {
    test_every_sound_the_shell_asks_for_is_in_the_pack();
    test_a_pack_that_is_not_one_is_refused();
    return test::finish("ui-sounds");
}

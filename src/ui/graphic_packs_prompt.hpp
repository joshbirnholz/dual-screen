#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/popup.hpp"
#include "ui/sound_view.hpp"
#include "update/graphic_packs.hpp"

namespace ds {

// The popups that go with Settings' row for Cemu's graphic packs, in Wii U
// (update/graphic_packs.hpp): how far a download has got, an update offered,
// and how it went. The row itself says only which packs are here -- a
// download's progress is this popup's, as Updates' are UpdatePrompt's.
class GraphicPacksPrompt {
public:
    // Null is a device that plays no Wii U games, which has no such row.
    void set_downloader(update::GraphicPacksDownloader* downloader) { downloader_ = downloader; }
    bool available() const { return downloader_ != nullptr; }

    // The row was chosen. `game_running`: Cemu is playing a game, which has
    // read its graphic packs already, so they are left alone until it closes,
    // as Cemu's own window leaves them.
    void choose(bool game_running);

    // What the row is called, and what it says beside it: which packs are
    // here ("v981"), "Not downloaded", "Checking...", or a newer one found.
    std::string row_label() const;
    std::string row_value() const;

    // Every frame.
    void poll();

    bool open() const { return popup_ != Popup::None; }
    // While open, takes every press.
    void update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers, float output_width,
                float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs, float output_width,
                float output_height);
    void describe_for_sound(UiSoundView& view) const;

private:
    enum class Popup { None, Offer, Progress, Notice };
    using Phase = update::GraphicPacksDownloader::Phase;

    void show_notice(std::string message, std::vector<std::string> lines = {});
    void start_install();
    std::vector<PopupButton> buttons() const;
    PopupText words() const;
    PopupGeometry geometry(float output_width, float output_height) const;

    update::GraphicPacksDownloader* downloader_ = nullptr;
    Popup popup_ = Popup::None;
    PopupCursor cursor_;
    std::string message_;
    std::vector<std::string> lines_;
    // The check whose answer the row's press is waiting on, and the install
    // the progress popup is showing -- by round -- and whether that install
    // is the first packs here or newer ones in place of others.
    int waiting_round_ = 0;
    int install_round_ = 0;
    bool fresh_ = false;
    update::GraphicPacksDownloader::Status status_;
};

}  // namespace ds

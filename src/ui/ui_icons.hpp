#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/embedded_assets.hpp"
#include "render/png_image.hpp"
#include "render/quad_renderer.hpp"
#include "render/texture.hpp"
#include "util/log.hpp"

namespace ds {

// The cells of assets/ui_icons.png, in order -- see assets/make_ui_icons.py.
// Each is coverage only, tinted for whatever it is drawn on.
enum class UiIcon {
    Get,
    Send,
    History,
    Delete,
    SteamDeck,
    Switch,
    Pc,
    Sync,
    Android,
    WiiUGamePad,
    Tv,
    // What a Wii U game has a player as.
    WiiUPro,
    ClassicPro,
    WiiRemote,
    WiiRemoteNunchuk,
    WiiRemoteClassic,
    // A controller picked up, to be moved up or down the line.
    Reorder,
    // A Wii Remote a game has held on its side -- see WiiUMapping::sideways.
    WiiRemoteSideways,
    Count
};

// The icon for what a Wii U game has a player as, in Cemu's words or the
// menus' -- see wii_u_type_name() -- or Count for none.
inline UiIcon wii_u_type_icon(const std::string& type) {
    if (type == "Pro Controller" || type == "Wii U Pro Controller") return UiIcon::WiiUPro;
    if (type == "Classic Controller") return UiIcon::ClassicPro;
    if (type == "Wii Remote") return UiIcon::WiiRemote;
    if (type == "Wii Remote + Nunchuk") return UiIcon::WiiRemoteNunchuk;
    if (type == "Wii Remote + Classic Controller" || type == "Wii Classic Controller") {
        return UiIcon::WiiRemoteClassic;
    }
    if (type == "Wii U GamePad") return UiIcon::WiiUGamePad;
    return UiIcon::Count;
}

// The strip, loaded into `texture` the first time it is wanted. False, once
// and with a line in the log, when it will not load.
inline bool load_ui_icons(Texture& texture, bool& tried) {
    if (texture.valid()) return true;
    if (tried) return false;
    tried = true;
    DecodedImage image;
    std::string error;
    if (!decode_png(assets::kUiIconsPng, assets::kUiIconsPng_size, image, &error)) {
        log_warn("the menus' icons would not load: " + error);
        return false;
    }
    std::vector<uint8_t> coverage(image.pixels.size());
    for (std::size_t index = 0; index < image.pixels.size(); ++index) {
        coverage[index] = static_cast<uint8_t>(image.pixels[index] >> 24);
    }
    texture.upload_alpha8(coverage.data(), image.width, image.height);
    texture.set_linear_filtering(true);
    return texture.valid();
}

// One cell, square, into `rect`. A device sits in a band across the middle of
// its cell, half as tall as it is wide.
inline void draw_ui_icon(QuadRenderer& quads, const Texture& texture, UiIcon icon,
                         const PixelRect& rect, const Color& color) {
    const float cells = static_cast<float>(static_cast<int>(UiIcon::Count));
    const float cell = static_cast<float>(static_cast<int>(icon));
    quads.draw_alpha_mask(rect, texture, color, cell / cells, 0.0f, (cell + 1.0f) / cells, 1.0f);
}

}  // namespace ds

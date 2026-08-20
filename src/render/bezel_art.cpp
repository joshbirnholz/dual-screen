#include "render/bezel_art.hpp"

#include "render/embedded_assets.hpp"
#include "render/png_image.hpp"

namespace ds {

bool BezelArt::load(std::string* error) {
    DecodedImage image;
    if (!decode_png(assets::kDsBezelPng, assets::kDsBezelPng_size, image, error)) {
        return false;
    }
    if (!image.valid()) {
        if (error != nullptr) *error = "the console artwork decoded to nothing";
        return false;
    }
    // Always smoothed, whatever the game's own setting is: this is a photograph
    // of plastic rather than pixel art, and it is drawn at whatever size the
    // window happens to be.
    texture_.set_linear_filtering(true);
    texture_.upload_xrgb8888(image.pixels.data(), image.width, image.height, image.width);
    return texture_.valid();
}

}  // namespace ds

#include "render/bezel_art.hpp"

#include "render/embedded_assets.hpp"
#include "render/png_image.hpp"

namespace ds {

bool BezelArt::load_one(const unsigned char* png,
                        unsigned long size,
                        Texture& out,
                        std::string* error) {
    DecodedImage image;
    if (!decode_png(png, size, image, error)) {
        return false;
    }
    if (!image.valid()) {
        if (error != nullptr) *error = "the console artwork decoded to nothing";
        return false;
    }
    // Always smoothed, whatever the game's own setting is: this is a photograph
    // of plastic rather than pixel art, and it is drawn at whatever size the
    // window happens to be.
    out.set_linear_filtering(true);
    out.upload_xrgb8888(image.pixels.data(), image.width, image.height, image.width);
    return out.valid();
}

bool BezelArt::load(std::string* error) {
    // Both are attempted whichever fails, so one broken picture does not take
    // the other console's away with it.
    std::string first_error;
    const bool ds_ok =
        load_one(assets::kDsBezelPng, assets::kDsBezelPng_size, ds_, &first_error);
    std::string three_ds_error;
    const bool three_ds_ok = load_one(assets::kThreeDsBezelPng, assets::kThreeDsBezelPng_size,
                                      three_ds_, &three_ds_error);
    if (!ds_ok && error != nullptr) {
        *error = first_error;
    } else if (!three_ds_ok && error != nullptr) {
        *error = three_ds_error;
    }
    return ds_ok && three_ds_ok;
}

}  // namespace ds

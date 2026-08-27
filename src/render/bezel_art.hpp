#pragma once

#include <string>

#include "core/system.hpp"
#include "render/texture.hpp"

namespace ds {

// The console artwork the Standard layout draws its screens into -- one picture
// per console, since a 3DS is not a DS with different screens.
//
// Owned rather than kept in a global, because it holds GL textures and a
// texture has to die before the context that made it. The app creates one after
// the window is up and destroys it before the window goes down.
class BezelArt {
public:
    // Decodes the embedded images and uploads them. Failing is not fatal: the
    // layout still works, it just has nothing drawn behind the screens. A
    // console whose picture failed reports null and is drawn blank; the error
    // describes the first one that went wrong.
    bool load(std::string* error);
    void destroy() {
        ds_ = Texture();
        three_ds_ = Texture();
    }

    // The picture for one console, or null where there is none to draw.
    const Texture* texture(SystemId system) const {
        const Texture* art = nullptr;
        switch (system) {
            case SystemId::Ds: art = &ds_; break;
            case SystemId::ThreeDs: art = &three_ds_; break;
            // A box under a television and a pad in your hands is not a picture
            // two screens can be drawn inside.
            case SystemId::WiiU: break;
        }
        // A console with no picture drawn for it yet is blank around its
        // screens, which is what the layout already handles.
        return (art != nullptr && art->valid()) ? art : nullptr;
    }

private:
    bool load_one(const unsigned char* png, unsigned long size, Texture& out, std::string* error);

    Texture ds_;
    Texture three_ds_;
};

}  // namespace ds

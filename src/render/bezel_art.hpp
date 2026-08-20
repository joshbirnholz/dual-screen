#pragma once

#include <string>

#include "render/texture.hpp"

namespace ds {

// The console artwork the Standard layout draws its screens into.
//
// Owned rather than kept in a global, because it holds a GL texture and a
// texture has to die before the context that made it. The app creates one after
// the window is up and destroys it before the window goes down.
class BezelArt {
public:
    // Decodes the embedded image and uploads it. Failing is not fatal: the
    // layout still works, it just has nothing drawn behind the screens.
    bool load(std::string* error);
    void destroy() { texture_ = Texture(); }

    const Texture* texture() const { return texture_.valid() ? &texture_ : nullptr; }

private:
    Texture texture_;
};

}  // namespace ds

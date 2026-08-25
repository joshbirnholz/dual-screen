#include "screen_quad.hpp"

namespace ds::sw {

ScreenQuad screen_quad_for(const PlacedScreen& placed) {
    ScreenQuad quad;
    quad.x = placed.rect.x;
    quad.y = placed.rect.y;
    quad.w = placed.rect.w;
    quad.h = placed.rect.h;

    switch (placed.rotation) {
        case ScreenRotation::None:
            return quad;
        case ScreenRotation::Clockwise:
            quad.angle_degrees = 90.0;
            break;
        case ScreenRotation::CounterClockwise:
            quad.angle_degrees = -90.0;
            break;
    }

    quad.w = placed.rect.h;
    quad.h = placed.rect.w;
    quad.x = placed.rect.x + (placed.rect.w - quad.w) * 0.5f;
    quad.y = placed.rect.y + (placed.rect.h - quad.h) * 0.5f;
    return quad;
}

}  // namespace ds::sw

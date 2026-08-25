#pragma once

#include <string>

#include "input/controller_mapping.hpp"
#include "render/text_renderer.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// Shown while joining a host, on whichever device is doing the joining.
//
// It exists because the host list looks identical whether or not a press
// landed: with the list still on screen and every button ignored until the
// handshake finishes, joining a host that is slow to answer is inadvertently
// indistinguishable from the button doing nothing, and the natural response is
// to press it again. This says which device is being joined and how to stop
// waiting.
void render_connecting_screen(QuadRenderer& quads,
                              TextRenderer& text,
                              float output_width,
                              float output_height,
                              const std::string& device_name,
                              const std::string& game_title,
                              const MenuButtonLabels& labels);

}  // namespace ds

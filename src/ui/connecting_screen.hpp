#pragma once

#include <string>
#include <vector>

#include "input/controller_mapping.hpp"
#include "render/button_glyphs.hpp"
#include "render/text_renderer.hpp"
#include "ui/control_guide.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// A device with nothing to draw yet, because what it is waiting for has not
// arrived. A heading, some lines under it, and whatever way out there is.
//
// Every one of these exists for the same reason: a window that is simply blank
// is indistinguishable from a window that is broken, and the natural response
// to either is to press something again.
void render_waiting_screen(QuadRenderer& quads,
                           TextRenderer& text,
                           const ButtonGlyphSheet& glyphs,
                           float output_width,
                           float output_height,
                           const std::string& heading,
                           const std::vector<std::string>& lines,
                           const std::vector<GuidePrompt>& prompts);

// Shown while joining a host, on whichever device is doing the joining.
//
// It exists because the host list looks identical whether or not a press
// landed: with the list still on screen and every button ignored until the
// handshake finishes, joining a host that is slow to answer is inadvertently
// indistinguishable from the button doing nothing, and the natural response is
// to press it again. This says which device is being joined and how to stop
// waiting.
// `note` is an extra line under the two, for when the waiting has a reason
// worth knowing -- see CompanionSession::waiting_on_local_network(). Empty
// means no line.
void render_connecting_screen(QuadRenderer& quads,
                              TextRenderer& text,
                              const ButtonGlyphSheet& glyphs,
                              float output_width,
                              float output_height,
                              const std::string& device_name,
                              const std::string& game_title,
                              const std::string& note = std::string());

}  // namespace ds

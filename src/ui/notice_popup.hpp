#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "layout/layout.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

namespace ds {

// A message with one answer: OK, which puts it away. A popup like every other
// (see ui/popup.hpp) -- over the game, for something said when there is no
// menu up at all, such as the game having just been resumed from where it was
// left (App::Impl's State::ResumeNotice, and the companion, which shows the
// same popup when the host says one is up), and over Data Management for what
// it has to report.
struct NoticePopupGeometry {
    PixelRect dialog;
    // Its OK button, for a finger to land on.
    PixelRect accept;
};
NoticePopupGeometry notice_popup_geometry(float output_width, float output_height,
                                          const std::string& message);

// Whether this pass's presses answer it. A or B -- it has only the one answer
// -- or a tap on its OK.
bool notice_popup_answered(const std::vector<UiAction>& actions,
                           const std::vector<PointerEvent>& pointers,
                           float output_width,
                           float output_height,
                           const std::string& message);

void render_notice_popup(QuadRenderer& quads,
                         TextRenderer& text,
                         const ButtonGlyphSheet& glyphs,
                         float output_width,
                         float output_height,
                         const std::string& message);

// What the popup says when a game has been resumed.
inline constexpr const char* kResumedNotice = "Resuming from save state.";

}  // namespace ds

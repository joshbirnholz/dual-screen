#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "layout/layout.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

namespace ds {

// A message over the game, with one answer: OK, which puts it away.
//
// Drawn the way the quick menu's own notices are -- the same box, the same
// dimming behind it -- but over the game itself rather than over the menu,
// for something that is said when there is no menu up at all: that the game
// has just been resumed from where it was left. See App::Impl's
// State::ResumeNotice, and the companion, which shows the same popup when
// the host says one is up.
struct NoticePopupGeometry {
    PixelRect dialog;
    // The row the OK prompt sits in, all of it, for a finger to land on.
    PixelRect accept;
};
NoticePopupGeometry notice_popup_geometry(float output_width, float output_height);

// Whether this pass's presses answer it. A or B -- it has only the one answer
// -- or a tap on its prompt.
bool notice_popup_answered(const std::vector<UiAction>& actions,
                           const std::vector<PointerEvent>& pointers,
                           float output_width,
                           float output_height);

void render_notice_popup(QuadRenderer& quads,
                         TextRenderer& text,
                         const ButtonGlyphSheet& glyphs,
                         float output_width,
                         float output_height,
                         const std::string& message);

// What the popup says when a game has been resumed.
inline constexpr const char* kResumedNotice = "Resuming from save state.";

}  // namespace ds

#include "ui/connecting_screen.hpp"

#include <cmath>

#include "ui/control_guide.hpp"
#include "ui/page_frame.hpp"

namespace ds {

void render_centred_message(QuadRenderer& quads,
                            TextRenderer& text,
                            float output_width,
                            float output_height,
                            const std::string& message) {
    const UiTheme& theme = ui_theme();
    const PageFrame frame = page_frame(output_width, output_height);
    const float scale = frame.scale * 1.1f;
    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);
    text.draw(quads, message, std::round(output_width * 0.5f),
              std::round((output_height - TextRenderer::line_height(scale)) * 0.5f), scale,
              theme.text, TextAlign::Center);
}

void render_waiting_screen(QuadRenderer& quads,
                           TextRenderer& text,
                           const ButtonGlyphSheet& glyphs,
                           float output_width,
                           float output_height,
                           const std::string& heading,
                           const std::vector<std::string>& lines,
                           const std::vector<GuidePrompt>& prompts,
                           std::vector<GuideHit>* out_hits) {
    const UiTheme& theme = ui_theme();
    // The same frame as every other page: see ui/page_frame.hpp.
    const PageFrame frame = page_frame(output_width, output_height);
    const float scale = frame.scale;
    const float line = frame.line;

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    draw_page_header(quads, text, frame, heading);

    // The first line is what is being waited for and the rest is detail, so
    // they are drawn at different weights. Wrapped rather than allowed to run
    // off the edge: one of these lines can be a filesystem path.
    float y = frame.content_top;
    bool first = true;
    for (const std::string& entry : lines) {
        if (entry.empty()) {
            if (!first) y += line * 0.6f;
            continue;
        }
        y += text.draw_wrapped(quads, entry, frame.panel.x, y, frame.panel.w,
                               first ? scale * 1.1f : scale, first ? theme.text : theme.text_dim);
        y += first ? line * 0.8f : line * 0.3f;
        first = false;
    }

    if (out_hits != nullptr) out_hits->clear();
    if (!prompts.empty()) {
        draw_page_guide(quads, text, glyphs, frame, prompts, out_hits);
    }
}

void render_connecting_screen(QuadRenderer& quads,
                              TextRenderer& text,
                              const ButtonGlyphSheet& glyphs,
                              float output_width,
                              float output_height,
                              const std::string& device_name,
                              const std::string& game_title,
                              const std::string& note,
                              std::vector<GuideHit>* out_hits) {
    // Named, because the point of this screen is knowing which of the devices
    // on the list is being waited for.
    std::vector<std::string> lines = {device_name.empty() ? std::string() : "to " + device_name,
                                      game_title};
    if (!note.empty()) {
        lines.push_back(std::string());
        lines.push_back(note);
    }
    render_waiting_screen(quads, text, glyphs, output_width, output_height, "Connecting", lines,
                          {{ButtonGlyph::B, "Stop waiting"}}, out_hits);
}

}  // namespace ds

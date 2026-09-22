#include "ui/connecting_screen.hpp"

#include <cmath>

#include "ui/control_guide.hpp"

namespace ds {

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
    const float scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(scale);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    const float centre = output_width * 0.5f;
    float y = std::round(output_height * 0.40f);

    text.draw(quads, heading, centre, y, scale * 2.0f, theme.text, TextAlign::Center);
    y += line * 3.0f;

    // The first line is what is being waited for and the rest is detail, so
    // they are drawn at different weights.
    const float room = output_width * 0.9f;
    bool first = true;
    for (const std::string& entry : lines) {
        if (entry.empty()) {
            continue;
        }
        float entry_scale = first ? scale * 1.2f : scale;
        // Shrunk rather than allowed to run off the edges. One of these lines
        // is a filesystem path, whose length is not this screen's to choose.
        const float natural = TextRenderer::measure_width(entry, entry_scale);
        if (natural > room && natural > 0.0f) {
            entry_scale *= room / natural;
        }
        text.draw(quads, entry, centre, y, entry_scale,
                  first ? theme.text : theme.text_dim, TextAlign::Center);
        y += first ? line * 2.0f : line * 1.4f;
        first = false;
    }

    if (out_hits != nullptr) out_hits->clear();
    if (!prompts.empty()) {
        draw_control_guide(quads, text, glyphs, prompts, centre,
                           output_height - line * 1.5f - control_guide_height(scale * 0.95f),
                           scale * 0.95f, theme.text, TextAlign::Center, out_hits);
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

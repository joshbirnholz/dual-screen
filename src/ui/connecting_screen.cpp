#include "ui/connecting_screen.hpp"

#include <cmath>

#include "ui/prompt.hpp"

namespace ds {

void render_connecting_screen(QuadRenderer& quads,
                              TextRenderer& text,
                              float output_width,
                              float output_height,
                              const std::string& device_name,
                              const std::string& game_title,
                              const MenuButtonLabels& labels) {
    const UiTheme& theme = ui_theme();
    const float scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(scale);

    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(theme.background);

    const float centre = output_width * 0.5f;
    float y = std::round(output_height * 0.40f);

    text.draw(quads, "Connecting", centre, y, scale * 2.0f, theme.text, TextAlign::Center);
    y += line * 3.0f;

    // Named, because the point of this screen is knowing which of the devices
    // on the list is being waited for.
    if (!device_name.empty()) {
        text.draw(quads, "to " + device_name, centre, y, scale * 1.2f, theme.text,
                  TextAlign::Center);
        y += line * 2.0f;
    }
    if (!game_title.empty()) {
        text.draw(quads, game_title, centre, y, scale, theme.text_dim, TextAlign::Center);
    }

    text.draw(quads, format_prompts({{labels.cancel, "Stop waiting"}}), centre,
              output_height - line * 3.0f, scale * 0.95f, theme.text_dim, TextAlign::Center);
}

}  // namespace ds

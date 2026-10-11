#include "ui/control_guide.hpp"

#include <algorithm>
#include <cmath>

#include "ui/list_touch.hpp"
#include "ui/ui_theme.hpp"

namespace ds {
namespace {

// The glyph is drawn taller than the text beside it: the artwork is a disc with
// a letter inside, so matching their heights would leave the letter much
// smaller than the words it sits next to.
constexpr float kGlyphScale = 1.9f;
constexpr float kGapScale = 0.4f;
constexpr float kSpacingScale = 1.4f;

float glyph_height(float scale) { return TextRenderer::line_height(scale) * kGlyphScale; }

// A touch chip's padding either side of its label, and how tall it is. The
// height is deliberately not the whole row: the row is tall enough for the
// arrow that stands above a stick-click glyph, and a chip drawn that tall
// would look like a different component rather than the same prompt.
// Wider than a square box wanted. A capsule's rounded ends take room from
// the label without looking like they have, so the same padding reads tighter
// than it did with corners.
constexpr float kChipPadScale = 0.85f;
constexpr float kChipHeightScale = 1.7f;

float chip_height(float scale) { return TextRenderer::line_height(scale) * kChipHeightScale; }
float chip_pad(float scale) { return TextRenderer::line_height(scale) * kChipPadScale; }

PromptStyle g_prompt_style = PromptStyle::Pad;
ConfirmButton g_confirm_button = ConfirmButton::East;

}  // namespace

void set_prompt_style(PromptStyle style) { g_prompt_style = style; }
void set_confirm_button(ConfirmButton choice) { g_confirm_button = choice; }
ConfirmButton confirm_button() { return g_confirm_button; }

ButtonGlyph glyph_as_drawn(ButtonGlyph glyph) {
    if (g_confirm_button != ConfirmButton::South) {
        return glyph;
    }
    // Only the pair, and only the picture: the prompt still means what it
    // meant, so a tap on it still does what it said.
    if (glyph == ButtonGlyph::A) return ButtonGlyph::B;
    if (glyph == ButtonGlyph::B) return ButtonGlyph::A;
    return glyph;
}
PromptStyle prompt_style() { return g_prompt_style; }
bool touch_hints() { return g_prompt_style == PromptStyle::Touch; }

PromptStyle prompt_style_for(bool television, int controllers, int full_gamepads) {
    if (television) {
        // Nothing to tap, so a pad or the remote and no third answer. The
        // remote is also the answer when SDL reports nothing at all: drawing
        // pad glyphs for a pad that is not there is the mistake this whole
        // distinction exists to avoid.
        return full_gamepads > 0 ? PromptStyle::Pad : PromptStyle::Remote;
    }
    return controllers > 0 ? PromptStyle::Pad : PromptStyle::Touch;
}

ButtonGlyph guide_button_at(const std::vector<GuideHit>& hits, float x, float y) {
    for (const GuideHit& hit : hits) {
        if (hit.rect.contains(x, y)) {
            return hit.glyph;
        }
    }
    return ButtonGlyph::Count;
}

ConfirmButton confirm_button_for_style(PromptStyle style) {
    // South for a remote, because that is the button it has. East everywhere
    // else, including on an Apple TV with a pad paired to it.
    return style == PromptStyle::Remote ? ConfirmButton::South : ConfirmButton::East;
}

std::vector<GuidePrompt> prompts_worth_drawing(const std::vector<GuidePrompt>& prompts) {
    const PromptStyle style = prompt_style();
    if (style == PromptStyle::Pad) {
        return prompts;
    }
    std::vector<GuidePrompt> kept;
    kept.reserve(prompts.size());
    for (const GuidePrompt& prompt : prompts) {
        // Both drop the prompt for the thing that is already highlighted: a
        // finger taps it, a remote clicks it. Only the remote drops the way
        // out, because only the remote has a button for it -- on a phone the
        // prompt *is* the way out.
        const bool covered =
            prompt.role == PromptRole::ActsOnSelection ||
            (style == PromptStyle::Remote && prompt.role == PromptRole::LeavesScreen);
        if (!covered) {
            kept.push_back(prompt);
        }
    }
    return kept;
}

// ButtonGlyph::A is the east button and ButtonGlyph::B the south one -- the
// names are Nintendo's letters, the meaning is the position.
ButtonGlyph confirm_glyph_for(ConfirmButton choice) {
    return choice == ConfirmButton::South ? ButtonGlyph::B : ButtonGlyph::A;
}

ButtonGlyph cancel_glyph_for(ConfirmButton choice) {
    return choice == ConfirmButton::South ? ButtonGlyph::A : ButtonGlyph::B;
}

bool ui_action_for_glyph(ButtonGlyph glyph, UiAction& out) {
    switch (glyph) {
        case ButtonGlyph::A: out = UiAction::Confirm; return true;
        case ButtonGlyph::B: out = UiAction::Cancel; return true;
        case ButtonGlyph::Plus: out = UiAction::Quit; return true;
        case ButtonGlyph::Minus: out = UiAction::ToggleMenu; return true;
        case ButtonGlyph::L: out = UiAction::PageUp; return true;
        case ButtonGlyph::R: out = UiAction::PageDown; return true;
        // X, Y and the stick clicks only ever appear in the quick menu, where
        // a press is forwarded to the host rather than acted on here. See
        // Client::handle_menu().
        default: return false;
    }
}

float control_guide_height(float scale) { return glyph_height(scale); }

float draw_control_guide(QuadRenderer& quads,
                         TextRenderer& text,
                         const ButtonGlyphSheet& glyphs,
                         const std::vector<GuidePrompt>& all_prompts,
                         float x,
                         float y,
                         float scale,
                         const Color& color,
                         TextAlign align,
                         std::vector<GuideHit>* out_hits) {
    const std::vector<GuidePrompt> prompts = prompts_worth_drawing(all_prompts);
    const float height = glyph_height(scale);
    const float glyph_width = glyphs.width_for(height);
    const float gap = TextRenderer::line_height(scale) * kGapScale;
    const float spacing = TextRenderer::line_height(scale) * kSpacingScale;
    const bool touch = touch_hints();
    const float pad = chip_pad(scale);

    // Between the two buttons of a pair.
    const float slash_width = TextRenderer::measure_width("/", scale);
    const float pair_gap = gap * 0.35f;
    const auto glyphs_width = [&](const GuidePrompt& prompt) {
        return prompt.pair == ButtonGlyph::Count
                   ? glyph_width
                   : glyph_width * 2.0f + slash_width + pair_gap * 2.0f;
    };
    // One prompt's width: its glyphs and its words, or a chip around the words.
    const auto prompt_width = [&](const GuidePrompt& prompt) {
        const float label = TextRenderer::measure_width(prompt.action, scale);
        return touch ? label + pad * 2.0f : glyphs_width(prompt) + gap + label;
    };

    float total = 0.0f;
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (index > 0) total += spacing;
        total += prompt_width(prompts[index]);
    }

    if (out_hits != nullptr) out_hits->clear();
    float pen_x = std::round(align == TextAlign::Right ? x - total : x - total * 0.5f);
    // Centred on the button itself rather than on the box it is drawn into,
    // which includes headroom for whatever stands above a glyph.
    const float label_y =
        y + height * glyphs.button_centre() - TextRenderer::line_height(scale) * 0.5f;
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (index > 0) pen_x += spacing;
        const float width = prompt_width(prompts[index]);
        if (touch) {
            // A chip: the words in a box that looks like it can be pressed,
            // because here it is the only thing that can be. Centred on the
            // same line the glyph's button sits on, so a row of these lands
            // exactly where a row of glyphs would have.
            const float box_height = chip_height(scale);
            const PixelRect box{pen_x, label_y + TextRenderer::line_height(scale) * 0.5f -
                                           box_height * 0.5f,
                                width, box_height};
            // The line and the fill are two capsules, the second drawn inside
            // the first -- there is no stroked capsule, and an outline built
            // from four straight quads would cut the corners off the thing it
            // is meant to be following.
            const float line = std::max(1.0f, std::round(menu_border(scale) * 0.5f));
            quads.draw_capsule(box, color);
            quads.draw_capsule(PixelRect{box.x + line, box.y + line, box.w - line * 2.0f,
                                         box.h - line * 2.0f},
                               ui_theme().panel);
            text.draw(quads, prompts[index].action, pen_x + width * 0.5f, label_y, scale, color,
                      TextAlign::Center);
            if (out_hits != nullptr) {
                // The chip itself, which is the whole of what is on screen.
                out_hits->push_back(GuideHit{prompts[index].glyph, box});
            }
        } else {
            // Held down on, by a finger or the mouse, and still there: the
            // prompt lights up behind itself, the way a Switch's own footer
            // does, for exactly as long as letting go would press it. Only a
            // press that began on it counts -- see tapped(). Only where the
            // row can be pressed at all, which is where its hits are kept.
            const PixelRect hit{pen_x, y, width, height};
            const LivePointer& pointer = live_pointer();
            if (out_hits != nullptr && pointer.down && hit.contains(pointer.press_x, pointer.press_y) &&
                hit.contains(pointer.x, pointer.y)) {
                const float pad_x = std::round(TextRenderer::line_height(scale) * 0.45f);
                const float pad_y = std::round(height * 0.12f);
                Color glow = ui_theme().accent;
                glow.a = 0.22f;
                quads.draw_rounded_rect(PixelRect{hit.x - pad_x, hit.y - pad_y, hit.w + pad_x * 2.0f,
                                                  hit.h + pad_y * 2.0f},
                                        std::round(scale * 3.0f), glow);
            }
            glyphs.draw(quads, glyph_as_drawn(prompts[index].glyph), pen_x, y, height, color);
            if (prompts[index].pair != ButtonGlyph::Count) {
                const float slash_x = pen_x + glyph_width + pair_gap;
                text.draw(quads, "/", slash_x, label_y, scale, color);
                glyphs.draw(quads, glyph_as_drawn(prompts[index].pair),
                            slash_x + slash_width + pair_gap, y, height, color);
            }
            text.draw(quads, prompts[index].action, pen_x + glyphs_width(prompts[index]) + gap,
                      label_y, scale, color);
            if (out_hits != nullptr) {
                // The glyph and its words together: a finger aims at the
                // prompt, not at the picture inside it.
                out_hits->push_back(
                    GuideHit{prompts[index].glyph, PixelRect{pen_x, y, width, height}});
            }
        }
        pen_x += width;
    }
    return height;
}

}  // namespace ds

// ---------------------------------------------------------------------------
// The live pointer -- see list_touch.hpp. Here with the prompt style, the other
// fact about the machine every screen reads.

namespace ds {
namespace {
LivePointer g_live_pointer;
}  // namespace

void set_live_pointer(bool down, float x, float y) {
    if (down && !g_live_pointer.down) {
        g_live_pointer.press_x = x;
        g_live_pointer.press_y = y;
    }
    g_live_pointer.down = down;
    g_live_pointer.x = x;
    g_live_pointer.y = y;
}

const LivePointer& live_pointer() { return g_live_pointer; }

}  // namespace ds

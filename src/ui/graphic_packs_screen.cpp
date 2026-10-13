#include "ui/graphic_packs_screen.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "ui/page_frame.hpp"
#include "ui/ui_theme.hpp"

namespace ds {
namespace {

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (const char c : path) {
        if (c == '/') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (!part.empty()) parts.push_back(part);
    return parts;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A pack's own choice, the one a pack with a single list of presets has, is
// named "" by Cemu, which calls it the active preset.
std::string choice_label(const ExternalGraphicPack::Choice& choice) {
    return choice.category.empty() ? std::string("Preset") : choice.category;
}

}  // namespace

std::vector<GraphicPackLine> graphic_pack_lines(const std::vector<ExternalGraphicPack>& packs) {
    // Each pack's folders and name, from its path, as Cemu's window files it.
    struct Placed {
        std::size_t pack;
        std::vector<std::string> folders;
        std::string label;
    };
    std::vector<Placed> placed;
    for (std::size_t index = 0; index < packs.size(); ++index) {
        std::vector<std::string> parts = split_path(packs[index].path);
        Placed entry{index, {}, packs[index].name};
        if (!parts.empty()) {
            entry.label = parts.back();
            parts.pop_back();
            entry.folders = std::move(parts);
        }
        placed.push_back(std::move(entry));
    }
    // The folders every pack shares are the game's name, which the page is
    // already about: the groups are what is left -- "Mods", "Enhancements".
    std::size_t shared = 0;
    if (!placed.empty()) {
        shared = placed.front().folders.size();
        for (const Placed& entry : placed) {
            std::size_t same = 0;
            while (same < shared && same < entry.folders.size() &&
                   entry.folders[same] == placed.front().folders[same]) {
                ++same;
            }
            shared = same;
        }
    }
    const auto group_of = [&](const Placed& entry) {
        std::string group;
        for (std::size_t part = shared; part < entry.folders.size(); ++part) {
            if (!group.empty()) group += " / ";
            group += entry.folders[part];
        }
        return group;
    };
    // Packs in no group first, then each group, each alphabetically.
    std::stable_sort(placed.begin(), placed.end(), [&](const Placed& a, const Placed& b) {
        const std::string group_a = lower(group_of(a));
        const std::string group_b = lower(group_of(b));
        if (group_a != group_b) return group_a < group_b;
        return lower(a.label) < lower(b.label);
    });

    std::vector<GraphicPackLine> lines;
    std::string group;
    for (const Placed& entry : placed) {
        const std::string here = group_of(entry);
        if (here != group && !here.empty()) {
            lines.push_back(GraphicPackLine{GraphicPackLine::Kind::Heading, here});
        }
        group = here;
        lines.push_back(GraphicPackLine{GraphicPackLine::Kind::Pack, entry.label, entry.pack});
        const ExternalGraphicPack& pack = packs[entry.pack];
        // A pack's presets only mean something while it is on, as in Cemu's
        // window; one that is off keeps the list short.
        if (!pack.enabled) continue;
        for (std::size_t choice = 0; choice < pack.choices.size(); ++choice) {
            lines.push_back(GraphicPackLine{GraphicPackLine::Kind::Choice,
                                            choice_label(pack.choices[choice]), entry.pack, choice});
        }
    }
    return lines;
}

void GraphicPacksScreen::open() {
    open_ = true;
    waiting_ = true;
    restart_pending_ = false;
    changed_.clear();
    packs_.clear();
    lines_.clear();
    focus_ = 0;
    scroll_ = 0.0f;
}

void GraphicPacksScreen::set_packs(std::vector<ExternalGraphicPack> packs) {
    // Where the cursor was, by what it was on rather than by row: a pack
    // turned on or off grows or shrinks the list above it.
    std::string id;
    std::string category;
    bool on_choice = false;
    if (const GraphicPackLine* line = focused_line()) {
        id = packs_[line->pack].id;
        on_choice = line->kind == GraphicPackLine::Kind::Choice;
        if (on_choice) category = packs_[line->pack].choices[line->choice].category;
    }
    waiting_ = false;
    packs_ = std::move(packs);
    for (const ExternalGraphicPack& pack : packs_) {
        if (pack.restart_needed && changed_.count(pack.id) != 0) restart_pending_ = true;
    }
    rebuild_lines();
    int focus = 0;
    for (const GraphicPackLine& line : lines_) {
        if (!line.focusable()) continue;
        const ExternalGraphicPack& pack = packs_[line.pack];
        const bool is_choice = line.kind == GraphicPackLine::Kind::Choice;
        if (pack.id == id && is_choice == on_choice &&
            (!is_choice || pack.choices[line.choice].category == category)) {
            focus_ = focus;
            return;
        }
        ++focus;
    }
    // Gone -- a preset hidden by another's condition -- so the nearest row.
    focus_ = std::max(0, std::min(focus_, focusable_count() - 1));
}

void GraphicPacksScreen::rebuild_lines() { lines_ = graphic_pack_lines(packs_); }

int GraphicPacksScreen::focusable_count() const {
    int count = 0;
    for (const GraphicPackLine& line : lines_) count += line.focusable() ? 1 : 0;
    return count;
}

int GraphicPacksScreen::line_of_focus(int focus) const {
    int index = 0;
    for (std::size_t line = 0; line < lines_.size(); ++line) {
        if (!lines_[line].focusable()) continue;
        if (index++ == focus) return static_cast<int>(line);
    }
    return -1;
}

const GraphicPackLine* GraphicPacksScreen::focused_line() const {
    const int line = line_of_focus(focus_);
    return line < 0 ? nullptr : &lines_[static_cast<std::size_t>(line)];
}

std::string GraphicPacksScreen::line_value(const GraphicPackLine& line) const {
    const ExternalGraphicPack& pack = packs_[line.pack];
    if (line.kind == GraphicPackLine::Kind::Pack) return pack.enabled ? "On" : "Off";
    if (line.kind != GraphicPackLine::Kind::Choice) return std::string();
    const ExternalGraphicPack::Choice& choice = pack.choices[line.choice];
    if (choice.presets.empty()) return std::string();
    // None chosen yet shows the first, as Cemu's window does.
    const int at = choice.active >= 0 ? choice.active : 0;
    return choice.presets[static_cast<std::size_t>(at)];
}

void GraphicPacksScreen::adjust(int direction, GraphicPacksScreenResult& result) {
    const GraphicPackLine* line = focused_line();
    if (line == nullptr) return;
    ExternalGraphicPack& pack = packs_[line->pack];
    ExternalGraphicPackChange change;
    change.id = pack.id;
    if (line->kind == GraphicPackLine::Kind::Pack) {
        // Two values, so either direction is the other one.
        pack.enabled = !pack.enabled;
        change.kind = ExternalGraphicPackChange::Kind::Enabled;
        change.enabled = pack.enabled;
        // Its presets come and go under it; the cursor stays on it.
        rebuild_lines();
    } else {
        ExternalGraphicPack::Choice& choice = pack.choices[line->choice];
        const int count = static_cast<int>(choice.presets.size());
        if (count < 2) return;
        const int at = choice.active >= 0 ? choice.active : 0;
        choice.active = ((at + direction) % count + count) % count;
        change.kind = ExternalGraphicPackChange::Kind::Preset;
        change.category = choice.category;
        change.preset = choice.presets[static_cast<std::size_t>(choice.active)];
    }
    if (pack.restart_needed) restart_pending_ = true;
    changed_.insert(pack.id);
    result.change = std::move(change);
}

GraphicPacksScreen::Geometry GraphicPacksScreen::compute_geometry(float output_width,
                                                                  float output_height) const {
    Geometry geometry;
    // The same frame as every other page: see ui/page_frame.hpp.
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    const float line = frame.line;
    geometry.panel = PixelRect{frame.panel.x, frame.panel.y, frame.panel.w,
                               frame.content_bottom - frame.panel.y};
    geometry.view_top = frame.content_top;
    geometry.view_height = geometry.panel.y + geometry.panel.h - frame.content_top;
    const float top = frame.content_top - scroll_;
    const float row_height = menu_row_height(line);
    const float gap = menu_row_gap(line);
    const float row_width = geometry.panel.w - scroll_bar_width(geometry.scale) - line * 0.6f;
    // A pack's presets sit in from it, so they read as its.
    const float indent = std::round(line * 1.6f);

    float y = top;
    for (std::size_t index = 0; index < lines_.size(); ++index) {
        switch (lines_[index].kind) {
            case GraphicPackLine::Kind::Heading:
                if (index > 0) y += line * 0.9f;
                geometry.lines.push_back(PixelRect{geometry.panel.x, y, row_width, line * 1.45f});
                y += line * 1.45f;
                break;
            case GraphicPackLine::Kind::Pack:
                geometry.lines.push_back(PixelRect{geometry.panel.x, y, row_width, row_height});
                y += row_height + gap;
                break;
            case GraphicPackLine::Kind::Choice:
                geometry.lines.push_back(
                    PixelRect{geometry.panel.x + indent, y, row_width - indent, row_height});
                y += row_height + gap;
                break;
        }
    }
    geometry.content_height = y + line * 0.4f - top;
    return geometry;
}

void GraphicPacksScreen::scroll_focus_into_view(const Geometry& geometry) {
    const int line = line_of_focus(focus_);
    if (line < 0 || static_cast<std::size_t>(line) >= geometry.lines.size()) return;
    // The first row shows the top of the page, heading and all, and the last
    // the bottom of it.
    if (focus_ == 0) {
        scroll_ = 0.0f;
        return;
    }
    if (focus_ == focusable_count() - 1) {
        scroll_ = geometry.max_scroll();
        return;
    }
    const PixelRect& row = geometry.lines[static_cast<std::size_t>(line)];
    // A pack's heading comes into view with it when it is the first of its
    // group.
    float row_top = row.y;
    if (line > 0 && lines_[static_cast<std::size_t>(line - 1)].kind == GraphicPackLine::Kind::Heading) {
        row_top = geometry.lines[static_cast<std::size_t>(line - 1)].y;
    }
    const float top = geometry.view_top;
    const float bottom = geometry.panel.y + geometry.panel.h;
    if (row_top < top) {
        scroll_ -= top - row_top;
    } else if (row.y + row.h > bottom) {
        scroll_ += row.y + row.h - bottom;
    }
    scroll_ = std::max(0.0f, std::min(scroll_, geometry.max_scroll()));
}

GraphicPacksScreenResult GraphicPacksScreen::update(const std::vector<UiAction>& pressed,
                                                    const std::vector<PointerEvent>& touched,
                                                    float output_width, float output_height) {
    GraphicPacksScreenResult result;
    if (!open_) return result;
    // A tap on Back at the foot of the page is Back pressed.
    std::vector<UiAction> actions = pressed;
    std::vector<PointerEvent> pointers = touched;
    take_guide_taps(guide_hits_, actions, pointers);

    const Geometry geometry = compute_geometry(output_width, output_height);
    const int rows = focusable_count();
    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up: focus_ = std::max(0, focus_ - 1); break;
            case UiAction::Down: focus_ = std::max(0, std::min(rows - 1, focus_ + 1)); break;
            case UiAction::Left: adjust(-1, result); break;
            case UiAction::Right:
            case UiAction::Confirm: adjust(1, result); break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu: result.close_requested = true; break;
            case UiAction::PageUp: scroll_ -= geometry.view_height * 0.8f; break;
            case UiAction::PageDown: scroll_ += geometry.view_height * 0.8f; break;
            // A menu's face buttons and the app's own keys: nothing here
            // answers them.
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
            case UiAction::MenuDisconnect:
            case UiAction::MenuSettings:
            case UiAction::OpenMenu:
            case UiAction::Quit:
            case UiAction::ToggleFilter:
            case UiAction::SwapLayout: break;
        }
    }

    const Geometry moved = compute_geometry(output_width, output_height);
    scroll_ = std::max(0.0f, std::min(scroll_, moved.max_scroll()));
    // The cursor brings the view with it when it is moved -- and only then,
    // so a list dragged by a finger stays where the finger left it.
    if (!actions.empty()) {
        drag_.stop();
        scroll_focus_into_view(moved);
    }
    drag_.update(pointers,
                 PixelRect{moved.panel.x, moved.view_top, moved.panel.w, moved.view_height},
                 scroll_, moved.max_scroll());
    const Geometry settled = compute_geometry(output_width, output_height);
    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released || drag_.release_was_drag()) continue;
        int focus = 0;
        for (std::size_t index = 0; index < lines_.size() && index < settled.lines.size(); ++index) {
            if (!lines_[index].focusable()) continue;
            // A first tap highlights a row; a second changes its value.
            if (tapped(pointer, settled.lines[index]) && tap_chooses(focus_, focus)) {
                adjust(1, result);
                break;
            }
            ++focus;
        }
    }
    drag_.settle();

    if (result.close_requested) open_ = false;
    return result;
}

void GraphicPacksScreen::render(QuadRenderer& quads, TextRenderer& text,
                                const ButtonGlyphSheet& glyphs, float output_width,
                                float output_height) {
    if (!open_) return;
    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const PageFrame frame = page_frame(output_width, output_height);
    draw_page_background(quads, output_width, output_height, over_game_);
    draw_page_header(quads, text, frame, "Graphics Packs");

    if (waiting_ || packs_.empty()) {
        // Said in the middle of the page, where the list would be.
        const std::string message = waiting_ ? "Asking Cemu for this game's graphics packs..."
                                             : "Cemu has no graphics packs for this game.";
        const PixelRect content = frame.content();
        text.draw(quads, TextRenderer::elide_right(message, content.w, scale),
                  content.x + content.w * 0.5f,
                  std::round(content.y + (content.h - line) * 0.5f), scale, theme.text_dim,
                  TextAlign::Center);
    }

    quads.set_clip(PixelRect{geometry.panel.x, geometry.view_top, geometry.panel.w,
                             geometry.view_height});
    const int focused_line_index = line_of_focus(focus_);
    for (std::size_t index = 0; index < lines_.size() && index < geometry.lines.size(); ++index) {
        const GraphicPackLine& entry = lines_[index];
        const PixelRect& row = geometry.lines[index];
        if (entry.kind == GraphicPackLine::Kind::Heading) {
            text.draw(quads, entry.text, row.x, std::round(row.y + line * 0.2f), scale, theme.text);
            continue;
        }
        const bool focused = static_cast<int>(index) == focused_line_index;
        draw_menu_row(quads, row, focused, scale);
        const float pad = menu_row_pad(line);
        const std::string value = line_value(entry);
        const float value_scale = scale * 0.9f;
        // Label on the left, value on the right; a long label gives way to
        // the value, which is the thing being chosen.
        const float value_width =
            std::min(TextRenderer::measure_width(value, value_scale), row.w * 0.5f);
        const float label_room = row.w - pad * 3.0f - value_width;
        text.draw(quads, TextRenderer::elide_right(entry.text, label_room, scale), row.x + pad,
                  page_row_text_y(row, scale), scale, menu_row_text(focused));
        if (!value.empty()) {
            text.draw(quads, TextRenderer::elide_right(value, row.w * 0.5f, value_scale),
                      row.x + row.w - pad, page_row_text_y(row, value_scale), value_scale,
                      focused ? theme.panel : theme.text_dim, TextAlign::Right);
        }
    }
    quads.clear_clip();

    // A pack whose textures are replaced (a resolution pack, mostly) is only
    // read as the game starts, so its change waits for Reset.
    if (restart_pending_) {
        draw_page_status(quads, text, glyphs, frame,
                         "Some changes will take effect when the game is reset.", theme.text_dim);
    }
    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen}},
                    &guide_hits_);

    if (geometry.max_scroll() > 0.5f) {
        const float span = std::max(geometry.view_height, geometry.content_height);
        draw_scroll_bar(quads,
                        PixelRect{geometry.panel.x, geometry.view_top, geometry.panel.w,
                                  geometry.view_height},
                        scroll_ / span, geometry.view_height / span, line, scale);
    }
}

void GraphicPacksScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    for (const ExternalGraphicPack& pack : packs_) {
        mix_sound_state(view.rest, pack.enabled ? 1u : 0u);
        for (const ExternalGraphicPack::Choice& choice : pack.choices) {
            mix_sound_state(view.rest, static_cast<uint64_t>(choice.active + 1));
        }
    }
}

}  // namespace ds

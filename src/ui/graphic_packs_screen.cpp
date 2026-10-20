#include "ui/graphic_packs_screen.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "ui/fitted_text.hpp"
#include "ui/page_frame.hpp"
#include "ui/ui_theme.hpp"

namespace ds {
namespace {

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string part;
    for (const char c : text) {
        if (c == separator) {
            parts.push_back(part);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    parts.push_back(part);
    return parts;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    for (std::string& part : split(path, '/')) {
        if (!part.empty()) parts.push_back(std::move(part));
    }
    return parts;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

bool graphic_pack_divider(const std::string& preset) {
    const std::size_t start = preset.find_first_not_of(' ');
    return start != std::string::npos && preset.compare(start, 2, "--") == 0;
}

std::string graphic_pack_divider_label(const std::string& preset) {
    // Every run of dashes goes ("-- 21:9 -- resolutions" is "21:9
    // resolutions"), but a lone one between words stays.
    std::string out;
    for (std::size_t index = 0; index < preset.size();) {
        if (preset[index] == '-' && index + 1 < preset.size() && preset[index + 1] == '-') {
            while (index < preset.size() && preset[index] == '-') ++index;
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        if (preset[index] == ' ' && (out.empty() || out.back() == ' ')) {
            ++index;
            continue;
        }
        out += preset[index++];
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

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
    }
    return lines;
}

void GraphicPacksScreen::open() {
    open_ = true;
    waiting_ = true;
    restart_pending_ = false;
    changed_.clear();
    packs_.clear();
    ++packs_revision_;
    lines_.clear();
    focus_ = 0;
    in_pack_ = false;
    item_ = 0;
    scroll_ = 0.0f;
    detail_scroll_ = 0.0f;
    mirrored_focus_ = -1;
    mirrored_item_ = -1;
    choice_.close();
    mirrored_choice_focus_ = -1;
}

void GraphicPacksScreen::set_packs(std::vector<ExternalGraphicPack> packs) {
    // Where the cursor was, by what it was on rather than by row: Cemu's list
    // comes in its own order, and a preset can hide another.
    std::string id;
    std::string category;
    const bool on_choice = in_pack_ && item_ > 0;
    if (const ExternalGraphicPack* pack = focused_pack()) {
        id = pack->id;
        if (on_choice && static_cast<std::size_t>(item_ - 1) < pack->choices.size()) {
            category = pack->choices[static_cast<std::size_t>(item_ - 1)].category;
        }
    }
    waiting_ = false;
    packs_ = std::move(packs);
    ++packs_revision_;
    for (const ExternalGraphicPack& pack : packs_) {
        if (pack.restart_needed && changed_.count(pack.id) != 0) restart_pending_ = true;
    }
    rebuild_lines();

    const int count = pack_count();
    int found = -1;
    for (int focus = 0; focus < count && found < 0; ++focus) {
        const int line = line_of_focus(focus);
        if (packs_[lines_[static_cast<std::size_t>(line)].pack].id == id) found = focus;
    }
    if (found < 0) {
        // Gone, so the nearest pack, and back to the list.
        focus_ = std::max(0, std::min(focus_, count - 1));
        in_pack_ = false;
        item_ = 0;
        return;
    }
    focus_ = found;
    if (on_choice) {
        const ExternalGraphicPack& pack = *focused_pack();
        int at = -1;
        for (std::size_t choice = 0; choice < pack.choices.size(); ++choice) {
            if (pack.choices[choice].category == category) at = static_cast<int>(choice) + 1;
        }
        // A choice hidden by another's condition: the nearest row left.
        item_ = at >= 0 ? at : std::min(item_, item_count() - 1);
    }
}

int GraphicPacksScreen::pack_count() const {
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

const ExternalGraphicPack* GraphicPacksScreen::focused_pack() const {
    const int line = line_of_focus(focus_);
    return line < 0 ? nullptr : &packs_[lines_[static_cast<std::size_t>(line)].pack];
}

ExternalGraphicPack* GraphicPacksScreen::focused_pack() {
    const int line = line_of_focus(focus_);
    return line < 0 ? nullptr : &packs_[lines_[static_cast<std::size_t>(line)].pack];
}

int GraphicPacksScreen::item_count() const {
    const ExternalGraphicPack* pack = focused_pack();
    return pack == nullptr ? 0 : 1 + static_cast<int>(pack->choices.size());
}

std::string GraphicPacksScreen::item_label(int item) const {
    const ExternalGraphicPack* pack = focused_pack();
    if (pack == nullptr) return std::string();
    if (item == 0) return "Enabled";
    const ExternalGraphicPack::Choice& choice = pack->choices[static_cast<std::size_t>(item - 1)];
    // A pack's only choice is named "" by Cemu, which calls it the active
    // preset.
    return choice.category.empty() ? std::string("Preset") : choice.category;
}

std::string GraphicPacksScreen::item_value(int item) const {
    const ExternalGraphicPack* pack = focused_pack();
    if (pack == nullptr) return std::string();
    if (item == 0) return pack->enabled ? "On" : "Off";
    const ExternalGraphicPack::Choice& choice = pack->choices[static_cast<std::size_t>(item - 1)];
    if (choice.presets.empty()) return std::string();
    // None chosen yet shows the first, as Cemu's window does.
    const int at = choice.active >= 0 ? choice.active : 0;
    return choice.presets[static_cast<std::size_t>(at)];
}

void GraphicPacksScreen::adjust(GraphicPacksScreenResult& result) {
    ExternalGraphicPack* pack = focused_pack();
    if (pack == nullptr || item_ < 0 || item_ >= item_count()) return;
    ExternalGraphicPackChange change;
    change.id = pack->id;
    if (item_ == 0) {
        pack->enabled = !pack->enabled;
        ++packs_revision_;
        change.kind = ExternalGraphicPackChange::Kind::Enabled;
        change.enabled = pack->enabled;
    } else {
        // A preset: its choices beside its row -- see open_choice().
        open_choice(compute_geometry(last_width_, last_height_));
        return;
    }
    if (pack->restart_needed) restart_pending_ = true;
    changed_.insert(pack->id);
    result.change = std::move(change);
}

void GraphicPacksScreen::open_choice(const Geometry& geometry) {
    const ExternalGraphicPack* pack = focused_pack();
    if (pack == nullptr || item_ < 1 || item_ >= item_count()) return;
    const ExternalGraphicPack::Choice& choice = pack->choices[static_cast<std::size_t>(item_ - 1)];
    if (choice.presets.size() < 2) return;
    const PixelRect row = item_ < static_cast<int>(geometry.items.size())
                              ? geometry.items[static_cast<std::size_t>(item_)]
                              : geometry.detail;
    // Dividers among them are headings, without their dashes.
    std::vector<std::string> labels = choice.presets;
    std::vector<bool> headings(labels.size(), false);
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (!graphic_pack_divider(labels[index])) continue;
        headings[index] = true;
        labels[index] = graphic_pack_divider_label(labels[index]);
    }
    // None chosen yet is the first, as it is shown.
    choice_.open(std::move(labels), choice.active >= 0 ? choice.active : 0, row);
    choice_.set_headings(std::move(headings));
}

void GraphicPacksScreen::choose(int index, GraphicPacksScreenResult& result) {
    ExternalGraphicPack* pack = focused_pack();
    if (pack == nullptr || item_ < 1 || item_ >= item_count()) return;
    ExternalGraphicPack::Choice& choice = pack->choices[static_cast<std::size_t>(item_ - 1)];
    if (index < 0 || index >= static_cast<int>(choice.presets.size())) return;
    if (graphic_pack_divider(choice.presets[static_cast<std::size_t>(index)])) return;
    if (index == (choice.active >= 0 ? choice.active : 0) && choice.active >= 0) return;
    choice.active = index;
    ++packs_revision_;
    ExternalGraphicPackChange change;
    change.id = pack->id;
    change.kind = ExternalGraphicPackChange::Kind::Preset;
    change.category = choice.category;
    change.preset = choice.presets[static_cast<std::size_t>(index)];
    if (pack->restart_needed) restart_pending_ = true;
    changed_.insert(pack->id);
    result.change = std::move(change);
}

GraphicPacksScreen::Geometry GraphicPacksScreen::compute_geometry(float output_width,
                                                                  float output_height) const {
    Geometry geometry;
    // The same frame as every other page, split as the quick menu is: the
    // list, a line, and what is highlighted in it.
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    const float line = frame.line;
    geometry.line = line;
    geometry.panel = frame.panel;
    geometry.view_top = frame.content_top;
    geometry.view_height = frame.content_bottom - frame.content_top;
    const float list_width = std::round(frame.panel.w * 0.38f);
    geometry.list = PixelRect{frame.panel.x, geometry.view_top, list_width, geometry.view_height};
    geometry.divider_x = std::round(frame.panel.x + list_width + line);
    const float detail_x = geometry.divider_x + line;
    geometry.detail = PixelRect{detail_x, geometry.view_top,
                                frame.panel.x + frame.panel.w - detail_x, geometry.view_height};

    // The list, as tall as the quick menu's own rows, short of its right-hand
    // edge by the scroll bar.
    const float row_height = menu_row_height(line);
    const float row_step = row_height + menu_row_gap(line);
    const float row_width = list_width - scroll_bar_width(geometry.scale) - line * 0.5f;
    float y = geometry.view_top - scroll_;
    for (std::size_t index = 0; index < lines_.size(); ++index) {
        if (lines_[index].kind == GraphicPackLine::Kind::Heading) {
            if (index > 0) y += line * 0.7f;
            geometry.lines.push_back(PixelRect{frame.panel.x, y, row_width, line * 1.5f});
            y += line * 1.5f;
        } else {
            geometry.lines.push_back(PixelRect{frame.panel.x, y, row_width, row_height});
            y += row_step;
        }
    }
    geometry.content_height = y + scroll_ - geometry.view_top;

    // The pack: its name, then its rows as the quick menu's Options are,
    // then what it says -- scrolled together, short of a scroll bar.
    const float detail_top = geometry.view_top - detail_scroll_;
    geometry.title_y = detail_top;
    float item_y = detail_top + TextRenderer::line_height(geometry.scale * 1.25f) + line * 0.8f;
    const float detail_width = geometry.detail.w - scroll_bar_width(geometry.scale) - line * 0.5f;
    for (int item = 0; item < item_count(); ++item) {
        geometry.items.push_back(PixelRect{geometry.detail.x, item_y, detail_width, row_height});
        item_y += row_step;
    }
    geometry.description_y = item_y + line * 0.9f;
    float end = geometry.description_y;
    if (const ExternalGraphicPack* pack = focused_pack()) {
        end += description_height(pack->description, detail_width, geometry.scale * 0.85f);
    }
    geometry.detail_height = end + line * 0.5f - detail_top;
    return geometry;
}

float GraphicPacksScreen::description_height(const std::string& description, float width,
                                             float scale) const {
    DescriptionHeight& cached = description_height_;
    if (cached.width == width && cached.scale == scale && cached.text == description) return cached.height;
    float height = 0.0f;
    for (const std::string& paragraph : split(description, '\n')) {
        height += paragraph.empty() ? TextRenderer::wrapped_line_step(scale) * 0.6f
                                    : TextRenderer::wrapped_line_step(scale) *
                                          static_cast<float>(TextRenderer::wrap(paragraph, width, scale).size());
    }
    cached = DescriptionHeight{description, width, scale, height};
    return height;
}

bool GraphicPacksScreen::same_packs(const std::vector<net::GraphicPacksViewPack>& wire) const {
    if (wire.size() != packs_.size()) return false;
    for (std::size_t index = 0; index < wire.size(); ++index) {
        const ExternalGraphicPack& here = packs_[index];
        const net::GraphicPacksViewPack& there = wire[index];
        if (here.id != there.id || here.enabled != there.enabled ||
            here.restart_needed != there.restart_needed || here.name != there.name ||
            here.path != there.path || here.description != there.description ||
            here.choices.size() != there.choices.size()) {
            return false;
        }
        for (std::size_t choice = 0; choice < here.choices.size(); ++choice) {
            if (here.choices[choice].category != there.choices[choice].category ||
                here.choices[choice].presets != there.choices[choice].presets ||
                here.choices[choice].active != there.choices[choice].active) {
                return false;
            }
        }
    }
    return true;
}

void GraphicPacksScreen::scroll_focus_into_view(const Geometry& geometry) {
    const int line = line_of_focus(focus_);
    if (line < 0 || static_cast<std::size_t>(line) >= geometry.lines.size()) return;
    // The first pack shows the top of the list, heading and all, and the last
    // the bottom of it.
    if (focus_ == 0) {
        scroll_ = 0.0f;
        return;
    }
    if (focus_ == pack_count() - 1) {
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
    const float bottom = geometry.view_top + geometry.view_height;
    if (row_top < top) {
        scroll_ -= top - row_top;
    } else if (row.y + row.h > bottom) {
        scroll_ += row.y + row.h - bottom;
    }
    scroll_ = std::max(0.0f, std::min(scroll_, geometry.max_scroll()));
}

void GraphicPacksScreen::scroll_item_into_view(const Geometry& geometry) {
    if (!in_pack_ || item_ < 0 || item_ >= static_cast<int>(geometry.items.size())) {
        detail_scroll_ = std::min(detail_scroll_, geometry.detail_max_scroll());
        return;
    }
    const PixelRect& row = geometry.items[static_cast<std::size_t>(item_)];
    const float top = geometry.view_top;
    const float bottom = geometry.view_top + geometry.view_height;
    // The first row brings the pack's name with it.
    if (item_ == 0) {
        detail_scroll_ = 0.0f;
    } else if (row.y < top) {
        detail_scroll_ -= top - row.y;
    } else if (row.y + row.h > bottom) {
        detail_scroll_ += row.y + row.h - bottom;
    }
    detail_scroll_ = std::max(0.0f, std::min(detail_scroll_, geometry.detail_max_scroll()));
}

bool GraphicPacksScreen::scroll(const std::vector<PointerEvent>& pointers, float output_width,
                                float output_height) {
    if (choice_.is_open()) return choice_.scroll(pointers, output_width, output_height);
    const Geometry geometry = compute_geometry(output_width, output_height);
    drag_.update(pointers, geometry.list, scroll_, geometry.max_scroll());
    const PixelRect detail_view{geometry.detail.x, geometry.view_top, geometry.detail.w,
                                geometry.view_height};
    detail_drag_.update(pointers, detail_view, detail_scroll_, geometry.detail_max_scroll());
    return drag_.release_was_drag() || detail_drag_.release_was_drag();
}

std::vector<float> GraphicPacksScreen::scroll_state(float output_width, float output_height) const {
    const Geometry geometry = compute_geometry(output_width, output_height);
    return {scroll_in_lines(scroll_, geometry.scale), scroll_in_lines(detail_scroll_, geometry.scale),
            choice_.is_open() ? choice_.scroll_rows() : 0.0f};
}

void GraphicPacksScreen::set_scroll_state(const std::vector<float>& state, float output_width,
                                          float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    scroll_ = std::clamp(scroll_from_lines(scroll_at(state, 0), geometry.scale), 0.0f, geometry.max_scroll());
    detail_scroll_ = std::clamp(scroll_from_lines(scroll_at(state, 1), geometry.scale), 0.0f,
                                geometry.detail_max_scroll());
    drag_.stop();
    detail_drag_.stop();
    if (choice_.is_open()) choice_.set_scroll_rows(scroll_at(state, 2));
}

int GraphicPacksScreen::entry_at(float x, float y, float output_width, float output_height) const {
    if (choice_.is_open()) {
        const int choice = choice_.choice_at(x, y, output_width, output_height);
        return choice >= 0 ? kChoiceEntryBase + choice : -1;
    }
    const Geometry geometry = compute_geometry(output_width, output_height);
    int focus = 0;
    for (std::size_t index = 0; index < lines_.size() && index < geometry.lines.size(); ++index) {
        if (!lines_[index].focusable()) continue;
        if (y >= geometry.list.y && geometry.list.contains(x, y) && geometry.lines[index].contains(x, y)) {
            return focus * 2;
        }
        ++focus;
    }
    const PixelRect detail_view{geometry.detail.x, geometry.view_top, geometry.detail.w,
                                geometry.view_height};
    for (std::size_t index = 0; index < geometry.items.size(); ++index) {
        if (detail_view.contains(x, y) && geometry.items[index].contains(x, y)) {
            return static_cast<int>(index) * 2 + 1;
        }
    }
    return -1;
}

bool GraphicPacksScreen::off_popup(float x, float y, float output_width,
                                   float output_height) const {
    return choice_.is_open() && !choice_.contains(x, y, output_width, output_height);
}

void GraphicPacksScreen::handle_tap(int entry, GraphicPacksScreenResult& result) {
    if (!open_) return;
    if (entry == kTapOffPopup) {
        // Off a preset's choices: put away, nothing changed.
        choice_.close();
        return;
    }
    if (entry < 0) return;
    if (entry >= kChoiceEntryBase) {
        if (!choice_.is_open()) return;
        const int index = entry - kChoiceEntryBase;
        if (index < 0 || index >= static_cast<int>(choice_.choices().size())) return;
        choice_.close();
        choose(index, result);
        return;
    }
    if (choice_.is_open()) return;
    const int index = entry / 2;
    if (entry % 2 == 0) {
        if (index >= pack_count()) return;
        const bool highlighted = !in_pack_ && focus_ == index;
        focus_ = index;
        in_pack_ = highlighted;
        item_ = 0;
        if (!highlighted) detail_scroll_ = 0.0f;
        scroll_focus_into_view(compute_geometry(last_width_, last_height_));
        return;
    }
    if (index >= item_count()) return;
    const bool highlighted = in_pack_ && item_ == index;
    in_pack_ = true;
    item_ = index;
    if (highlighted) adjust(result);
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

    last_width_ = output_width;
    last_height_ = output_height;
    const Geometry geometry = compute_geometry(output_width, output_height);
    // A preset's choices take every press until put away -- the buttons
    // here, a tap the one way every tap is heard, below.
    if (choice_.is_open()) {
        const ChoicePopupResult answer = choice_.update(actions, {}, output_width, output_height);
        if (answer.chosen >= 0) choose(answer.chosen, result);
        if (answer.close_everything) {
            result.close_requested = true;
            open_ = false;
            return result;
        }
        for (const int tap : take_taps(pointers, output_width, output_height)) handle_tap(tap, result);
        return result;
    }
    const int packs = pack_count();
    const int focus_before = focus_;
    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                if (in_pack_) {
                    item_ = std::max(0, item_ - 1);
                } else {
                    focus_ = std::max(0, focus_ - 1);
                }
                break;
            case UiAction::Down:
                if (in_pack_) {
                    item_ = std::max(0, std::min(item_count() - 1, item_ + 1));
                } else {
                    focus_ = std::max(0, std::min(packs - 1, focus_ + 1));
                }
                break;
            // Into the pack from the list, on its first row. Inside it, A
            // turns it on or off or opens a preset's choices; Right does
            // nothing there, values no longer being stepped from the side.
            case UiAction::Right:
            case UiAction::Confirm:
                if (in_pack_) {
                    if (action == UiAction::Confirm) adjust(result);
                } else if (packs > 0) {
                    in_pack_ = true;
                    item_ = 0;
                }
                break;
            // Back to the list from the pack's left-hand edge, as in the
            // quick menu.
            case UiAction::Left:
                in_pack_ = false;
                break;
            // B a step at a time; the menu's own button out of everything.
            case UiAction::Cancel:
                if (in_pack_) {
                    in_pack_ = false;
                } else {
                    result.close_requested = true;
                }
                break;
            case UiAction::ToggleMenu: result.close_requested = true; break;
            // L and R jump nowhere: a shoulder is not a way through a list.
            case UiAction::PageUp:
            case UiAction::PageDown: break;
            // A menu's face buttons and the app's own keys: nothing here
            // answers them.
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
            case UiAction::MenuDisconnect:
            case UiAction::MenuSettings:
            case UiAction::OpenMenu:
            case UiAction::Quit:
            case UiAction::ToggleFilter:
            case UiAction::NextLayout:
            case UiAction::SwapLayout: break;
        }
    }

    const Geometry moved = compute_geometry(output_width, output_height);
    scroll_ = std::max(0.0f, std::min(scroll_, moved.max_scroll()));
    detail_scroll_ = std::max(0.0f, std::min(detail_scroll_, moved.detail_max_scroll()));
    // The cursor brings the list with it when it is moved -- and only then,
    // so a list dragged by a finger stays where the finger left it. A new
    // pack shows from its top.
    if (!actions.empty()) {
        drag_.stop();
        detail_drag_.stop();
        if (focus_ != focus_before) detail_scroll_ = 0.0f;
        scroll_focus_into_view(moved);
        scroll_item_into_view(compute_geometry(output_width, output_height));
    }
    // A first tap highlights; a second on the same thing chooses it -- a
    // pack, by going into it, and a row in it, by changing it. Heard the one
    // way every tap is, this device's and the other's alike.
    for (const int tap : take_taps(pointers, output_width, output_height)) handle_tap(tap, result);
    drag_.settle();
    detail_drag_.settle();

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
    const float line = geometry.line;
    const float row_scale = scale * 1.1f;
    const PageFrame frame = page_frame(output_width, output_height);
    draw_page_background(quads, output_width, output_height, over_game_);
    draw_page_header(quads, text, frame, "Graphics Packs");
    const auto now = std::chrono::steady_clock::now();
    const int highlight = focus_ * 4096 + (in_pack_ ? item_ + 1 : 0);
    if (highlight != highlight_) {
        highlight_ = highlight;
        highlight_since_ = now;
    }
    const float highlight_seconds = std::chrono::duration<float>(now - highlight_since_).count();

    if (waiting_ || packs_.empty()) {
        // Said in the middle of the page, where the list would be.
        const std::string message = waiting_ ? "Asking Cemu for this game's graphics packs..."
                                             : "Cemu has no graphics packs for this game.";
        const PixelRect content = frame.content();
        text.draw(quads, TextRenderer::elide_right(message, content.w, scale),
                  content.x + content.w * 0.5f,
                  std::round(content.y + (content.h - line) * 0.5f), scale, theme.text_dim,
                  TextAlign::Center);
    } else {
        // The list, drawn as a Switch's settings draw their left-hand side:
        // the pack whose things are showing in the accent colour with a bar
        // beside it, and ringed while the cursor is on it.
        quads.set_clip(geometry.list);
        const int focused_line = line_of_focus(focus_);
        for (std::size_t index = 0; index < lines_.size() && index < geometry.lines.size(); ++index) {
            const GraphicPackLine& entry = lines_[index];
            const PixelRect& row = geometry.lines[index];
            if (entry.kind == GraphicPackLine::Kind::Heading) {
                text.draw(quads, TextRenderer::elide_right(entry.text, row.w, scale), row.x,
                          std::round(row.y + (row.h - line) * 0.5f), scale, theme.text_dim);
                continue;
            }
            const bool current = static_cast<int>(index) == focused_line;
            const bool focused = current && !in_pack_;
            draw_side_tab(quads, row, current, focused, scale);
            const Color& ink = current ? theme.accent : theme.text;
            const bool enabled = packs_[entry.pack].enabled;
            const float pad = line * 0.8f;
            const float baseline = row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f;
            // On, said at the right as Cemu's tree ticks it; off, nothing.
            float label_room = row.w - pad * 2.0f;
            if (enabled) {
                const float value_scale = scale * 0.9f;
                text.draw(quads, "On", row.x + row.w - pad,
                          row.y + (row.h - TextRenderer::line_height(value_scale)) * 0.5f,
                          value_scale, current ? theme.accent : theme.text_dim,
                          TextAlign::Right);
                label_room -= TextRenderer::measure_width("On", value_scale) + pad;
            }
            draw_fitted_text(quads, text, entry.text, row.x + pad, baseline, label_room, row_scale, ink,
                             TextAlign::Left, focused, highlight_seconds);
        }
        quads.clear_clip();
        if (geometry.max_scroll() > 0.5f) {
            const float span = std::max(geometry.view_height, geometry.content_height);
            draw_scroll_bar(quads, geometry.list, scroll_ / span, geometry.view_height / span, line,
                            scale);
        }
        quads.draw_solid(PixelRect{geometry.divider_x, geometry.view_top,
                                   std::max(1.0f, std::round(scale * 0.5f)), geometry.view_height},
                         theme.text_dim);

        // The pack highlighted: its name, its rows, then what it says.
        if (const ExternalGraphicPack* pack = focused_pack()) {
            const PixelRect& area = geometry.detail;
            quads.set_clip(area);
            const float title_scale = scale * 1.25f;
            const int line_index = line_of_focus(focus_);
            draw_fitted_text(quads, text, lines_[static_cast<std::size_t>(line_index)].text, area.x,
                             geometry.title_y, area.w - scroll_bar_width(scale), title_scale, theme.text,
                             TextAlign::Left, true, highlight_seconds);
            for (int item = 0; item < static_cast<int>(geometry.items.size()); ++item) {
                const PixelRect& row = geometry.items[static_cast<std::size_t>(item)];
                const bool focused = in_pack_ && item == item_;
                draw_menu_row(quads, row, focused, scale, item == static_cast<int>(geometry.items.size()) - 1,
                              in_pack_ && item == item_ + 1);
                const float pad = line * 0.8f;
                const float baseline = row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f;
                const std::string value = item_value(item);
                // Turning the pack on and off is a switch; a preset, whatever
                // it is called, is a choice.
                const bool toggle = item == 0;
                // The row's room shared between the setting and its value as
                // each needs; what still does not fit slides along while the
                // cursor is on it.
                const std::string label = item_label(item);
                const float room = row.w - pad * 3.0f;
                float label_room = 0.0f;
                float value_room = 0.0f;
                split_row_room(room, TextRenderer::measure_width(label, row_scale),
                               toggle ? line * 2.2f : TextRenderer::measure_width(value, row_scale), label_room,
                               value_room);
                if (toggle) label_room = room - line * 2.2f;
                // Both too long: the setting slides, then its value.
                const float speed = TextRenderer::line_height(row_scale) * kFittedTextLinesPerSecond;
                const float label_cycle =
                    fitted_text_cycle(TextRenderer::measure_width(label, row_scale) - label_room, speed);
                const float value_cycle =
                    toggle ? 0.0f
                           : fitted_text_cycle(TextRenderer::measure_width(value, row_scale) - value_room, speed);
                float label_seconds = 0.0f;
                float value_seconds = 0.0f;
                const bool label_turn =
                    fitted_text_turn(label_cycle, value_cycle, highlight_seconds, true, label_seconds);
                const bool value_turn =
                    fitted_text_turn(label_cycle, value_cycle, highlight_seconds, false, value_seconds);
                draw_fitted_text(quads, text, label, row.x + pad, baseline, label_room, row_scale,
                                 menu_row_text(focused), TextAlign::Left, focused && label_turn, label_seconds);
                if (toggle) {
                    draw_toggle(quads, row.x + row.w - pad, row.y + row.h * 0.5f, value == "On", line);
                } else {
                    draw_fitted_text(quads, text, value, row.x + row.w - pad, baseline, value_room, row_scale,
                                     theme.text_dim, TextAlign::Right, focused && value_turn, value_seconds);
                }
            }
            // Cemu's descriptions keep their own line breaks.
            float y = geometry.description_y;
            const float description_scale = scale * 0.85f;
            for (const std::string& paragraph : split(pack->description, '\n')) {
                if (y > area.y + area.h) break;
                if (paragraph.empty()) {
                    y += TextRenderer::wrapped_line_step(description_scale) * 0.6f;
                    continue;
                }
                y += text.draw_wrapped(quads, paragraph, area.x, y, area.w - scroll_bar_width(scale) - line * 0.5f,
                                       description_scale, theme.text_dim);
            }
            quads.clear_clip();
            if (geometry.detail_max_scroll() > 0.5f) {
                const float span = std::max(geometry.view_height, geometry.detail_height);
                draw_scroll_bar(quads,
                                PixelRect{area.x, geometry.view_top, area.w, geometry.view_height},
                                detail_scroll_ / span, geometry.view_height / span, line, scale);
            }
        }
    }

    // A pack whose textures are replaced (a resolution pack, mostly) is only
    // read as the game starts, so its change waits for Reset.
    if (restart_pending_) {
        draw_page_status(quads, text, glyphs, frame,
                         "Some changes will take effect when the game is restarted.", theme.text_dim);
    }
    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen}},
                    &guide_hits_);
    // A preset's choices, over its row wherever that is drawn now.
    if (choice_.is_open()) {
        const Geometry placed = compute_geometry(output_width, output_height);
        if (item_ >= 0 && item_ < static_cast<int>(placed.items.size())) {
            choice_.set_row(placed.items[static_cast<std::size_t>(item_)]);
        }
        choice_.render(quads, text, output_width, output_height);
    }
}

void GraphicPacksScreen::show_mirrored(const net::GraphicPacksViewMessage& view,
                                       float output_width, float output_height) {
    open_ = true;
    waiting_ = view.waiting;
    restart_pending_ = view.restart_pending;
    // Drawn every frame from the last thing the other device said, which is
    // the same packs nearly always: only taken in again when they are not.
    if (!same_packs(view.packs)) {
        packs_.clear();
        ++packs_revision_;
        for (const net::GraphicPacksViewPack& wire : view.packs) {
            ExternalGraphicPack pack;
            pack.id = wire.id;
            pack.path = wire.path;
            pack.name = wire.name;
            pack.description = wire.description;
            pack.enabled = wire.enabled;
            pack.restart_needed = wire.restart_needed;
            for (const net::GraphicPacksViewChoice& choice : wire.choices) {
                pack.choices.push_back({choice.category, choice.presets, choice.active});
            }
            packs_.push_back(std::move(pack));
        }
        rebuild_lines();
    }
    // Laid out from the same list, so the host's numbers mean the same
    // things here.
    focus_ = std::max(0, std::min(view.focus, pack_count() - 1));
    in_pack_ = view.in_pack && pack_count() > 0;
    item_ = std::max(0, std::min(view.item, item_count() - 1));
    // The other device's cursor brings the page with it when it moves;
    // otherwise the page is where a finger here left it -- see drag().
    if (focus_ != mirrored_focus_ || in_pack_ != mirrored_in_pack_ || item_ != mirrored_item_) {
        if (focus_ != mirrored_focus_) detail_scroll_ = 0.0f;
        scroll_focus_into_view(compute_geometry(output_width, output_height));
        scroll_item_into_view(compute_geometry(output_width, output_height));
        mirrored_focus_ = focus_;
        mirrored_in_pack_ = in_pack_;
        mirrored_item_ = item_;
    }
    const Geometry geometry = compute_geometry(output_width, output_height);
    scroll_ = std::max(0.0f, std::min(scroll_, geometry.max_scroll()));
    detail_scroll_ = std::max(0.0f, std::min(detail_scroll_, geometry.detail_max_scroll()));
    // The host's popup, with its cursor -- which brings the list with it
    // only when it moves, as the page's does: put there every pass, it put
    // the list back under the cursor as fast as a finger here could move it.
    if (view.choice_focus == 0) {
        choice_.close();
        mirrored_choice_focus_ = -1;
    } else {
        last_width_ = output_width;
        last_height_ = output_height;
        if (!choice_.is_open()) {
            open_choice(compute_geometry(output_width, output_height));
            mirrored_choice_focus_ = -1;
        }
        if (view.choice_focus - 1 != mirrored_choice_focus_) {
            choice_.set_focus(view.choice_focus - 1);
            mirrored_choice_focus_ = view.choice_focus - 1;
        }
    }
}

net::GraphicPacksViewMessage graphic_packs_view_of(const GraphicPacksScreen& screen) {
    net::GraphicPacksViewMessage view;
    view.open = screen.is_open();
    if (!view.open) return view;
    view.waiting = screen.waiting();
    view.restart_pending = screen.restart_pending();
    view.focus = screen.focus();
    view.in_pack = screen.in_pack();
    view.item = screen.item();
    view.choice_focus = screen.choosing() ? static_cast<uint16_t>(screen.choice_focus() + 1) : 0;
    for (const ExternalGraphicPack& pack : screen.packs()) {
        net::GraphicPacksViewPack wire;
        wire.id = pack.id;
        wire.path = pack.path;
        wire.name = pack.name;
        wire.description = pack.description;
        wire.enabled = pack.enabled;
        wire.restart_needed = pack.restart_needed;
        for (const ExternalGraphicPack::Choice& choice : pack.choices) {
            wire.choices.push_back({choice.category, choice.presets, choice.active});
        }
        view.packs.push_back(std::move(wire));
    }
    return view;
}

void render_mirrored_graphic_packs(GraphicPacksScreen& screen, QuadRenderer& quads,
                                   TextRenderer& text, const ButtonGlyphSheet& glyphs,
                                   float output_width, float output_height,
                                   const net::GraphicPacksViewMessage& view, bool over_game) {
    if (!over_game) {
        quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
        quads.clear(ui_theme().background);
    }
    screen.show_mirrored(view, output_width, output_height);
    screen.set_over_game(over_game);
    screen.render(quads, text, glyphs, output_width, output_height);
}

void GraphicPacksScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    mix_sound_state(view.focus, static_cast<uint64_t>(in_pack_ ? item_ + 1 : 0));
    mix_sound_state(view.focus, static_cast<uint64_t>(choice_.is_open() ? choice_.focus() + 1000 : 0));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    for (const ExternalGraphicPack& pack : packs_) {
        mix_sound_state(view.rest, pack.enabled ? 1u : 0u);
        for (const ExternalGraphicPack::Choice& choice : pack.choices) {
            mix_sound_state(view.rest, static_cast<uint64_t>(choice.active + 1));
        }
    }
    // A pack's first row is the switch that turns it on.
    const ExternalGraphicPack* pack = focused_pack();
    if (in_pack_ && item_ == 0 && !choice_.is_open() && pack != nullptr) view.toggle = pack->enabled ? 1 : 0;
}

}  // namespace ds

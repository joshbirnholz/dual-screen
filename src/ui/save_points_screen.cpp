#include "ui/save_points_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/control_guide.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

namespace {

const char* mode_label(int index) { return index == 0 ? "Load Save Point" : "Create Save Point"; }

}  // namespace

void SavePointsScreen::open(SavePointMode mode) {
    open_ = true;
    mode_ = mode;
    on_slots_ = true;
    confirm_ = Confirm::None;
    status_.clear();
    status_failed_ = false;
    slot_ = natural_slot();
}

void SavePointsScreen::close() {
    open_ = false;
    confirm_ = Confirm::None;
}

void SavePointsScreen::set_slots(std::vector<SavePointInfo> slots) {
    slots.resize(kSavePointSlots);
    slots_ = std::move(slots);
    pictures_dirty_.fill(true);
}

void SavePointsScreen::set_status(std::string status, bool failed) {
    status_ = std::move(status);
    status_failed_ = failed;
}

bool SavePointsScreen::slot_filled(int slot) const {
    return slot >= 0 && slot < static_cast<int>(slots_.size()) &&
           slots_[static_cast<std::size_t>(slot)].exists;
}

int SavePointsScreen::natural_slot() const {
    int newest = -1;
    int oldest = -1;
    for (int slot = 0; slot < kSavePointSlots; ++slot) {
        if (!slot_filled(slot)) {
            // Saving: the first empty slot loses nothing at all.
            if (mode_ == SavePointMode::Create) return slot;
            continue;
        }
        const int64_t when = slots_[static_cast<std::size_t>(slot)].saved_at;
        if (newest < 0 || when > slots_[static_cast<std::size_t>(newest)].saved_at) newest = slot;
        if (oldest < 0 || when < slots_[static_cast<std::size_t>(oldest)].saved_at) oldest = slot;
    }
    if (mode_ == SavePointMode::Load) return std::max(0, newest);
    return std::max(0, oldest);
}

void SavePointsScreen::choose_slot(SavePointsScreenResult& result) {
    const bool filled = slot_filled(slot_);
    if (mode_ == SavePointMode::Load) {
        // An empty slot has nothing to load; it is shown dimmed to say so.
        if (filled) confirm_ = Confirm::Load;
        return;
    }
    if (filled) {
        confirm_ = Confirm::Overwrite;
    } else {
        result.save_slot = slot_;
    }
}

SavePointsScreen::Geometry SavePointsScreen::compute_geometry(float output_width,
                                                              float output_height) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(geometry.scale);
    const float margin = std::round(output_width * 0.06f);
    geometry.panel = PixelRect{margin, std::round(output_height * 0.07f),
                               output_width - margin * 2.0f,
                               output_height - std::round(output_height * 0.14f)};
    const float top = geometry.panel.y + line * 2.6f;
    const float bottom = geometry.panel.y + geometry.panel.h - line * 3.0f;

    // The left column is as wide as its longer entry needs and no wider, so
    // the pictures get the rest.
    const float mode_width =
        TextRenderer::measure_width(mode_label(1), geometry.scale * 1.1f) + line * 2.0f;
    const float left_width = std::min(geometry.panel.w * 0.4f, mode_width);
    const float mode_height = line * 2.2f;
    for (int index = 0; index < 2; ++index) {
        geometry.modes[index] = PixelRect{geometry.panel.x, top + index * (mode_height + line * 0.3f),
                                          left_width, mode_height};
    }
    geometry.divider_x = std::round(geometry.panel.x + left_width + line * 1.0f);

    const float slots_x = geometry.divider_x + line * 1.0f;
    const float step = (bottom - top) / static_cast<float>(kSavePointSlots);
    for (int slot = 0; slot < kSavePointSlots; ++slot) {
        geometry.slots[static_cast<std::size_t>(slot)] =
            PixelRect{slots_x, top + slot * step, geometry.panel.x + geometry.panel.w - slots_x,
                      step - line * 0.3f};
    }

    const float dialog_width = std::round(output_width * 0.58f);
    const float dialog_height = std::round(line * 7.0f);
    geometry.dialog = PixelRect{std::round((output_width - dialog_width) * 0.5f),
                                std::round((output_height - dialog_height) * 0.5f), dialog_width,
                                dialog_height};
    const float row_height = line * 2.4f;
    const float row_y = geometry.dialog.y + geometry.dialog.h - row_height - line * 0.4f;
    geometry.accept = PixelRect{geometry.dialog.x, row_y, geometry.dialog.w * 0.5f, row_height};
    geometry.dismiss = PixelRect{geometry.dialog.x + geometry.dialog.w * 0.5f, row_y,
                                 geometry.dialog.w * 0.5f, row_height};
    return geometry;
}

SavePointsScreenResult SavePointsScreen::update(const std::vector<UiAction>& actions,
                                                const std::vector<PointerEvent>& pointers,
                                                float output_width,
                                                float output_height) {
    SavePointsScreenResult result;
    if (!open_) return result;
    const Geometry geometry = compute_geometry(output_width, output_height);

    if (confirm_ != Confirm::None) {
        bool accepted = false;
        bool dismissed = false;
        for (const UiAction action : actions) {
            if (action == UiAction::Confirm) accepted = true;
            if (action == UiAction::Cancel || action == UiAction::ToggleMenu) dismissed = true;
        }
        for (const PointerEvent& pointer : pointers) {
            if (tapped(pointer, geometry.accept)) accepted = true;
            else if (tapped(pointer, geometry.dismiss)) dismissed = true;
        }
        if (accepted) {
            if (confirm_ == Confirm::Load) {
                result.load_slot = slot_;
            } else {
                result.save_slot = slot_;
            }
            confirm_ = Confirm::None;
        } else if (dismissed) {
            confirm_ = Confirm::None;
        }
        return result;
    }

    const auto set_mode = [&](SavePointMode mode) {
        if (mode == mode_) return;
        mode_ = mode;
        status_.clear();
    };
    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
            case UiAction::Down: {
                const int direction = action == UiAction::Up ? -1 : 1;
                if (on_slots_) {
                    slot_ = (slot_ + kSavePointSlots + direction) % kSavePointSlots;
                } else {
                    set_mode(mode_ == SavePointMode::Load ? SavePointMode::Create
                                                          : SavePointMode::Load);
                }
                break;
            }
            case UiAction::Right:
                if (!on_slots_) {
                    on_slots_ = true;
                    slot_ = natural_slot();
                }
                break;
            case UiAction::Left: on_slots_ = false; break;
            case UiAction::Confirm:
                if (on_slots_) {
                    choose_slot(result);
                } else {
                    on_slots_ = true;
                    slot_ = natural_slot();
                }
                break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu:
                // One step at a time, the way NSO backs out: the slots, then
                // the list, then the screen.
                if (on_slots_) {
                    on_slots_ = false;
                } else {
                    result.close_requested = true;
                }
                break;
            case UiAction::OpenMenu:
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
            case UiAction::MenuDisconnect:
            case UiAction::MenuSettings:
            case UiAction::Quit:
            case UiAction::ToggleFilter:
            case UiAction::SwapLayout:
            case UiAction::PageUp:
            case UiAction::PageDown: break;
        }
        if (result.close_requested || result.save_slot >= 0 || confirm_ != Confirm::None) break;
    }

    if (!result.close_requested && result.save_slot < 0 && confirm_ == Confirm::None) {
        for (const PointerEvent& pointer : pointers) {
            if (!pointer.released) continue;
            bool hit = false;
            for (int index = 0; index < 2 && !hit; ++index) {
                if (!tapped(pointer, geometry.modes[index])) continue;
                set_mode(index == 0 ? SavePointMode::Load : SavePointMode::Create);
                on_slots_ = true;
                slot_ = natural_slot();
                hit = true;
            }
            for (int slot = 0; slot < kSavePointSlots && !hit; ++slot) {
                if (!tapped(pointer, geometry.slots[static_cast<std::size_t>(slot)])) continue;
                on_slots_ = true;
                slot_ = slot;
                choose_slot(result);
                hit = true;
            }
            if (hit) break;
        }
    }

    if (result.close_requested) close();
    return result;
}

void SavePointsScreen::render(QuadRenderer& quads,
                              TextRenderer& text,
                              const ButtonGlyphSheet& glyphs,
                              float output_width,
                              float output_height) {
    if (!open_) return;
    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);

    for (int slot = 0; slot < kSavePointSlots; ++slot) {
        const auto index = static_cast<std::size_t>(slot);
        if (!pictures_dirty_[index] || index >= slots_.size()) continue;
        pictures_dirty_[index] = false;
        const SavePointThumbnail& picture = slots_[index].thumbnail;
        if (slots_[index].exists && picture.valid()) {
            pictures_[index].upload_xrgb8888(picture.pixels.data(), picture.width, picture.height,
                                             picture.width);
        } else {
            pictures_[index] = Texture();
        }
    }

    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, theme.scrim);
    text.draw(quads, "Save Points", geometry.panel.x, geometry.panel.y, scale * 1.6f, theme.text);

    // The two things to do, right-aligned against the line the way NSO sets
    // them. The one in use stays marked while the cursor is over on the slots.
    for (int index = 0; index < 2; ++index) {
        const PixelRect& row = geometry.modes[index];
        const bool chosen = (index == 0) == (mode_ == SavePointMode::Load);
        const bool focused = chosen && !on_slots_;
        if (focused) {
            quads.draw_solid(row, theme.text);
        } else if (chosen) {
            quads.draw_solid(row, theme.panel_focused);
        }
        const float row_scale = scale * 1.1f;
        text.draw(quads, mode_label(index), row.x + row.w - line * 0.8f,
                  row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f, row_scale,
                  focused ? theme.background : (chosen ? theme.text : theme.text_dim),
                  TextAlign::Right);
    }
    const float slots_top = geometry.slots[0].y;
    const PixelRect& last = geometry.slots[kSavePointSlots - 1];
    quads.draw_solid(PixelRect{geometry.divider_x, slots_top, std::max(1.0f, std::round(scale * 0.5f)),
                               last.y + last.h - slots_top},
                     theme.text_dim);

    // Every picture the same size, whatever its console: a DS pair and a 3DS
    // pair are near enough the same shape side by side.
    const float pad = line * 0.3f;
    for (int slot = 0; slot < kSavePointSlots; ++slot) {
        const auto index = static_cast<std::size_t>(slot);
        const PixelRect& row = geometry.slots[index];
        const bool focused = on_slots_ && slot == slot_;
        const bool filled = slot_filled(slot);
        if (focused) quads.draw_solid(row, theme.text);

        const float box_height = row.h - pad * 2.0f;
        const float box_width = std::min(row.w * 0.5f, box_height * (8.0f / 3.0f));
        const PixelRect box{row.x + pad, row.y + pad, box_width, box_height};
        if (filled && pictures_[index].valid()) {
            const SavePointThumbnail& picture = slots_[index].thumbnail;
            // Fitted into the box without stretching.
            const float fit = std::min(box.w / picture.width, box.h / picture.height);
            const float width = picture.width * fit;
            const float height = picture.height * fit;
            quads.draw_solid(box, Color{0.0f, 0.0f, 0.0f, 1.0f});
            quads.draw_screen(PixelRect{box.x + (box.w - width) * 0.5f, box.y + (box.h - height) * 0.5f,
                                        width, height},
                              pictures_[index]);
        } else {
            quads.draw_solid(box, focused ? theme.text_dim : theme.panel);
        }

        const Color& ink = focused ? theme.background : theme.text;
        const Color dim = focused ? theme.panel : theme.text_dim;
        // Nothing to load in an empty slot, which says so by fading.
        const bool unusable = mode_ == SavePointMode::Load && !filled;
        const float text_x = box.x + box.w + line * 0.8f;
        const float name_line = TextRenderer::line_height(scale * 1.1f);
        const float date_line = TextRenderer::line_height(scale * 0.95f);
        const float name_step = name_line + line * 0.35f;
        const float name_y = row.y + (row.h - name_step - date_line) * 0.5f;
        text.draw(quads, "Save point " + std::to_string(slot + 1), text_x, name_y, scale * 1.1f,
                  unusable ? dim : ink);
        text.draw(quads, filled ? format_save_time(slots_[index].saved_at) : std::string("Empty"),
                  text_x, name_y + name_step, scale * 0.95f, dim);
    }

    const float guide_scale = scale * 0.9f;
    const float guide_y = geometry.panel.y + geometry.panel.h - control_guide_height(guide_scale);
    if (!status_.empty()) {
        text.draw(quads, status_, geometry.panel.x, guide_y + line * 0.1f, scale * 0.95f,
                  status_failed_ ? theme.danger : theme.success);
    }
    draw_control_guide(quads, text, glyphs,
                       {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen},
                        {ButtonGlyph::A, "OK", PromptRole::ActsOnSelection}},
                       geometry.panel.x + geometry.panel.w, guide_y, guide_scale, theme.text,
                       TextAlign::Right);

    if (confirm_ != Confirm::None) {
        quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height},
                         Color{0.0f, 0.0f, 0.0f, 0.5f});
        quads.draw_solid(geometry.dialog, theme.text);
        const char* question =
            confirm_ == Confirm::Load ? "Load this save point?" : "Overwrite this save point?";
        text.draw_wrapped(quads, question, geometry.dialog.x + geometry.dialog.w * 0.5f,
                          geometry.dialog.y + line * 1.4f, geometry.dialog.w - line * 2.0f,
                          scale * 1.25f, theme.background, TextAlign::Center);
        // Both answers named, as on the quick menu's own questions: here the
        // two prompts are the two answers.
        draw_control_guide(quads, text, glyphs, {{ButtonGlyph::A, "OK"}, {ButtonGlyph::B, "Back"}},
                           geometry.dialog.x + geometry.dialog.w * 0.5f, geometry.accept.y,
                           scale * 1.05f, theme.background);
    }
}

}  // namespace ds

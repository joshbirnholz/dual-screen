#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/ds_rom_info.hpp"
#include "forwarder/forwarder_icon.hpp"
#include "forwarder/forwarder_platform.hpp"
#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "render/texture.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/popup.hpp"

namespace ds {

struct ForwarderScreenResult {
    bool close_requested = false;
    // The SteamGridDB API key was typed in, or turned down by the site and
    // forgotten: the app keeps `api_key` in its settings.
    bool api_key_changed = false;
    std::string api_key;
};

// Forwarder Creation, reached from a ROM in the file picker: the title the
// HOME menu will show and its icon down the left, and on the right who made
// it, whether the capture button works, and Create Forwarder -- laid out the
// way Sphaira's own forwarder editor is. The title starts as the one the ROM
// gives itself and the publisher as the one its banner names; the icon as
// the ROM's own, or one found on SteamGridDB instead.
class ForwarderScreen {
public:
    explicit ForwarderScreen(ForwarderPlatform& platform);
    ~ForwarderScreen();
    ForwarderScreen(const ForwarderScreen&) = delete;
    ForwarderScreen& operator=(const ForwarderScreen&) = delete;

    void open(const std::string& rom_path, const DsRomInfo& info, const std::string& api_key);
    void close();
    bool is_open() const { return open_; }

    ForwarderScreenResult update(const std::vector<UiAction>& actions,
                                 const std::vector<PointerEvent>& pointers,
                                 float output_width, float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                float output_width, float output_height);

    // What is on screen, for tests and the log.
    enum class Page { Editor, Icons };
    enum class Overlay { None, IconSource, GameChoice, Busy, Notice };
    enum class Zone { Title, Icon, List };
    Page page() const { return page_; }
    Overlay overlay() const { return overlay_; }
    Zone zone() const { return zone_; }
    int list_focus() const { return list_focus_; }
    const ForwarderRequest& request() const { return request_; }
    const std::string& icon_source() const { return icon_source_; }
    const std::string& notice() const { return notice_; }
    std::size_t icon_count() const { return found_.size(); }
    // The games SteamGridDB matched the last search to, and which is shown.
    const std::vector<SteamGridGame>& games() const { return games_; }
    const std::string& match_name() const { return match_name_; }
    const std::string& game_choice_text() const { return game_choice_text_; }
    bool busy() const { return job_ != nullptr; }

    // The rows down the right.
    enum class Row { Publisher, Screenshots, VideoCapture, Create, Count };

private:
    struct Found {
        std::vector<uint8_t> jpeg;
        RgbaImage preview;
        Texture texture;
    };
    // One thing done off this thread: a search and its first pictures, more
    // pictures, or the install.
    struct Job {
        enum class Kind { Search, Pictures, More, Install } kind = Kind::Search;
        std::atomic<bool> done{false};
        std::string api_key;
        std::string query;
        SteamGridGame game;
        SteamGridSearch search;
        std::vector<std::string> urls;
        std::vector<std::vector<uint8_t>> icons;
        std::vector<RgbaImage> previews;
        ForwarderRequest request;
        bool ok = false;
        std::string error;
    };

    struct Geometry {
        PixelRect left;
        PixelRect title_box;
        PixelRect icon_box;
        float icon_label_y = 0.0f;
        PixelRect list;
        std::vector<PixelRect> rows;
    };
    Geometry editor_geometry(float output_width, float output_height) const;
    struct GridGeometry {
        PixelRect view;
        float cell = 0.0f;
        float gap = 0.0f;
        int columns = 1;
        float content_height = 0.0f;
    };
    GridGeometry grid_geometry(float output_width, float output_height) const;
    PixelRect grid_cell(const GridGeometry& grid, int index) const;
    int grid_count() const;
    bool has_more() const { return next_url_ < urls_.size(); }

    void update_editor(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                       float output_width, float output_height, ForwarderScreenResult& result);
    void update_icons(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                      float output_width, float output_height);
    void activate(ForwarderScreenResult& result);
    void edit_title();
    void choose_icon_source();
    bool ensure_api_key(ForwarderScreenResult& result);
    void search_steamgriddb(ForwarderScreenResult& result, const std::string& term);
    void search_another_name(ForwarderScreenResult& result);
    void show_pictures_of(const SteamGridGame& game);
    void choose_game(const std::string& message);
    void load_more();
    void create();
    void set_icon(std::vector<uint8_t> jpeg, const std::string& source);
    void show_notice(const std::string& message, bool closes = false);
    void start_job(std::unique_ptr<Job> job, const std::string& busy);
    void finish_job(ForwarderScreenResult& result);
    std::vector<PopupButton> source_buttons() const;
    std::vector<PopupButton> game_buttons() const;
    PopupGeometry game_popup(float output_width, float output_height) const;
    PopupGeometry source_popup(float output_width, float output_height) const;
    PopupGeometry busy_popup(float output_width, float output_height) const;
    void scroll_grid_to_focus(float output_width, float output_height);

    ForwarderPlatform& platform_;
    bool open_ = false;
    Page page_ = Page::Editor;
    Overlay overlay_ = Overlay::None;
    Zone zone_ = Zone::Icon;
    int list_focus_ = 0;

    ForwarderRequest request_;
    std::vector<uint8_t> rom_icon_;
    std::string icon_source_;
    std::string api_key_;
    Texture icon_texture_;
    bool icon_texture_dirty_ = true;

    std::string notice_;
    bool notice_closes_ = false;
    std::string busy_;
    PopupCursor popup_cursor_;

    // SteamGridDB: what it matched, every picture it has, and those fetched.
    std::string match_name_;
    int64_t match_id_ = 0;
    std::string search_term_;
    std::vector<SteamGridGame> games_;
    std::string game_choice_text_;
    std::vector<std::string> urls_;
    std::size_t next_url_ = 0;
    std::vector<std::unique_ptr<Found>> found_;
    int grid_focus_ = 0;
    float grid_scroll_ = 0.0f;
    ListDrag grid_drag_;

    std::unique_ptr<Job> job_;
    std::thread worker_;
    std::vector<GuideHit> guide_hits_;
};

}  // namespace ds

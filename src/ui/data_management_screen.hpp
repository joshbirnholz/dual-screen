#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/save_store.hpp"
#include "input/input_manager.hpp"
#include "net/save_exchange.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "render/texture.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"

namespace ds {

struct DataManagementResult {
    bool close_requested = false;
    // In a game, a change to the save the game has open: the game is to be
    // closed, this run on its save, and the game started again from it. Then
    // `restart_notice` said, or the error if it failed.
    std::function<bool(std::string* error)> restart_with;
    std::string restart_notice;
};

// Settings > Data Management: every game's save on this device, and for each,
// the page to get it from another device, send it to one, go back to an
// earlier one or delete it -- laid out like the Switch's own save data cloud
// page, with this device and the other side by side.
//
// Opened from the main menu, it starts at the list. Opened during a game, it
// goes straight to that game's page, and leaving the page leaves the screen.
//
// Nothing here happens without being chosen. Searching and every transfer run
// on a thread of their own, so the screen keeps drawing while the network is
// asked; the store's own operations are quick file moves and run here.
class DataManagementScreen {
public:
    // `own_instance` is this app's answer to a search, left out of its own.
    DataManagementScreen(SaveStore& store, std::string own_instance);
    ~DataManagementScreen();
    DataManagementScreen(const DataManagementScreen&) = delete;
    DataManagementScreen& operator=(const DataManagementScreen&) = delete;

    // With a game's ID, that game is being played and its page is all there is.
    void open(const std::string& game_in_play = std::string());
    void close();
    bool is_open() const { return open_; }

    // For tests: where the searches go.
    void set_search_for_test(const net::SaveSearch& search) { search_ = search; }
    // Something that goes up whenever another device sends a save here, so
    // what is on screen can be read again -- SaveExchangeService's count.
    void set_change_source(std::function<uint64_t()> source) { change_source_ = std::move(source); }

    DataManagementResult update(const std::vector<UiAction>& actions,
                                const std::vector<PointerEvent>& pointers,
                                float output_width,
                                float output_height);
    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height);

    // What is on screen, for tests and the log.
    enum class Page { List, Game, Earlier };
    enum class Overlay { None, Confirm, ResumeConfirm, Busy, Notice, Choice };
    Page page() const { return page_; }
    Overlay overlay() const { return overlay_; }
    const std::vector<SaveInfo>& saves() const { return saves_; }
    const std::vector<net::SaveDevice>& devices() const { return devices_; }
    bool searching() const { return job_ != nullptr; }
    // The comparison box's words for the device being shown, empty for none.
    std::string comparison_text() const;
    bool comparison_is_conflict() const;
    // The dialog's or popup's own words.
    const std::string& dialog_title() const { return dialog_title_; }
    const std::vector<std::string>& dialog_lines() const { return dialog_lines_; }
    const std::string& notice() const { return notice_; }
    // A conflict's question: which save to keep.
    const std::string& choice_title() const { return choice_title_; }
    int choice_focus() const { return choice_focus_; }
    bool syncing_all() const { return sync_all_; }
    // The game page's actions, as they read now.
    std::vector<std::string> action_labels() const;
    std::vector<bool> actions_enabled() const;
    // Which of them the cursor is on.
    int action_focus() const { return action_focus_; }

    // "N saves", for the row in Settings.
    static std::string describe_count(std::size_t count);

private:
    enum class Action { Sync, Get, Send, Earlier, Delete, Count };
    enum class Pending { None, Get, Send, Delete, Restore };

    struct RemoteState {
        bool answered = false;
        net::RemoteSaveStatus status;
    };

    // One piece of work off this thread: a search, then how each device's
    // save for a game stands; or one transfer.
    struct Job {
        enum class Kind { Search, Status, Fetch, Send, Plan } kind = Kind::Search;
        std::atomic<bool> done{false};
        std::string game_id;
        std::vector<net::SaveDevice> devices;
        std::map<std::string, RemoteState> remote;  // by instance
        // Sync All's: every device, and every save on it.
        std::vector<std::pair<net::SaveDevice, std::vector<net::RemoteSave>>> lists;
        net::SaveDevice device;
        SaveInfo info;
        std::vector<uint8_t> bytes;
        bool ok = false;
        std::string error;
    };

    // Which way a save has to go for both devices to have the newest.
    enum class SyncWay { Nothing, Get, Send, Conflict, Busy };
    static SyncWay sync_way(const SaveVersionState& here, const net::RemoteSaveStatus& there);

    // One game on one device, for Sync All.
    struct SyncItem {
        net::SaveDevice device;
        std::string game_id;
        std::string title;
        net::RemoteSaveStatus remote;
    };

    void reload_saves();
    void open_game(const std::string& game_id);
    // This game's save read again, after something here changed it.
    void refresh_game();
    void check_for_changes();
    void settle_action_focus();
    void start_job(std::unique_ptr<Job> job);
    void start_search();
    void start_status();
    void finish_job(DataManagementResult& result);
    const net::SaveDevice* device() const;
    const RemoteState* remote() const;
    SaveComparison comparison() const;
    bool in_game() const { return !game_in_play_.empty(); }
    bool this_game_in_play() const { return in_game() && game_.game_id == game_in_play_; }

    void activate(Action action, DataManagementResult& result);
    void start_sync(DataManagementResult& result);
    void show_choice(const std::string& title, const std::string& there_name,
                     const std::string& there_date, const std::string& here_date);
    void chosen(DataManagementResult& result);
    void start_sync_all();
    void advance_sync_all();
    void begin_sync_transfer(SyncWay way);
    void start_sync_job(SyncWay way);
    void skip_sync_item();
    void finish_sync_all();
    void start_transfer(Job::Kind kind, const net::SaveDevice& device, const std::string& game_id,
                        const std::string& busy);
    void ask(Pending pending);
    void confirmed(DataManagementResult& result);
    void carry_out(DataManagementResult& result);
    void show_notice(const std::string& message);
    void show_busy(const std::string& message);

    std::vector<GuidePrompt> guide_prompts() const;
    void handle_guide_tap(ButtonGlyph glyph, std::vector<UiAction>& actions) const;

    const Texture* game_icon(const SaveInfo& info);
    bool ensure_icons();

    SaveStore& store_;
    std::string own_instance_;
    std::string own_name_;
    std::string own_kind_;
    net::SaveSearch search_;

    bool open_ = false;
    std::string game_in_play_;
    Page page_ = Page::List;
    Overlay overlay_ = Overlay::None;
    int list_focus_ = 0;
    // How far the save list and Earlier Saves are scrolled, in pixels: a
    // finger moves them by the pixel.
    float list_scroll_ = 0.0f;
    int action_focus_ = 0;
    // Whether the player has moved it, or it is still wherever it started.
    bool action_focus_chosen_ = false;
    int earlier_focus_ = 0;
    float earlier_scroll_ = 0.0f;
    // The first row drawn, which row_rects_ start from.
    int row_first_ = 0;
    // A finger dragging the save list or Earlier Saves.
    ListDrag drag_;
    void drag_list(const std::vector<PointerEvent>& pointers, float output_width, float output_height);
    // Scrolls the save list or Earlier Saves just far enough to show the row
    // the cursor is on.
    void follow_focus(float output_width, float output_height);

    std::vector<SaveInfo> saves_;
    SaveInfo game_;
    std::vector<EarlierSave> earlier_;
    bool searched_ = false;
    std::vector<net::SaveDevice> devices_;
    std::size_t device_index_ = 0;
    std::string status_game_id_;
    std::map<std::string, RemoteState> remote_;

    Pending pending_ = Pending::None;
    std::string pending_earlier_;
    std::string dialog_title_;
    std::vector<std::string> dialog_lines_;
    // Which of dialog_lines_ is said plainly rather than dimmed.
    std::size_t dialog_plain_lines_ = 0;
    std::string dialog_accept_;
    std::string dialog_from_name_, dialog_from_date_, dialog_to_name_, dialog_to_date_;
    std::string notice_;
    std::string busy_;

    // Sync: set while a Get or Send comes from it rather than from its own
    // row, so no second question is asked before it goes.
    bool sync_mode_ = false;
    std::string choice_title_;
    std::string choice_line_;
    std::string choice_names_[2];
    std::string choice_dates_[2];
    int choice_focus_ = 0;

    // Sync All, while it runs.
    bool sync_all_ = false;
    // Sync All asked for while something else was being asked.
    bool plan_queued_ = false;
    std::vector<SyncItem> sync_items_;
    std::size_t sync_index_ = 0;
    SyncWay sync_pending_way_ = SyncWay::Nothing;
    int sync_done_ = 0;
    int sync_skipped_ = 0;
    int sync_failed_ = 0;
    std::vector<std::string> sync_device_names_;

    std::function<uint64_t()> change_source_;
    uint64_t seen_changes_ = 0;

    std::unique_ptr<Job> job_;
    std::thread worker_;
    // A search asked for while another job was running, and a status wanted
    // once the one running is done.
    bool search_queued_ = false;
    bool status_queued_ = false;

    std::vector<GuideHit> guide_hits_;
    // Where the rows were drawn, for a finger.
    std::vector<PixelRect> row_rects_;
    std::vector<PixelRect> choice_rects_;

    Texture ui_icons_;
    bool ui_icons_tried_ = false;
    std::map<std::string, Texture> game_icons_;
};

}  // namespace ds

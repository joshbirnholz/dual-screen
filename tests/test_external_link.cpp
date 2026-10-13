// The handover to a program that plays a console this app does not run.
//
// Both roles are driven here against each other, in one process. That is not
// quite the arrangement they will be used in -- the producer will be a fork of
// Cemu in a process of its own -- but everything that can be wrong about the
// protocol is wrong the same way either side of a process boundary, and this can
// be run on every build rather than only where an emulator is installed. The
// two-process version lives in the shell test beside it.

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "core/external_frame_link.hpp"

using namespace ds;

namespace {

constexpr int kTvWidth = 1280;
constexpr int kTvHeight = 720;
constexpr int kPadWidth = 854;
constexpr int kPadHeight = 480;

std::string socket_path() {
    const char* base = std::getenv("TMPDIR");
    return std::string(base != nullptr ? base : "/tmp") + "/dualscreen-link-test.sock";
}

void fill(std::vector<uint32_t>& out, int width, int height, uint32_t tint) {
    out.resize(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = tint + static_cast<uint32_t>(i % 251);
    }
}

ExternalStreamInfo wii_u_info() {
    ExternalStreamInfo info;
    info.top_width = kTvWidth;
    info.top_height = kTvHeight;
    info.bottom_width = kPadWidth;
    info.bottom_height = kPadHeight;
    info.audio_sample_rate = 48000;
    info.frames_per_second = 60.0;
    return info;
}

// Both ends up, handshaken, ready to be driven.
//
// The connecting happens on a thread of its own, and that is not a convenience:
// the producer waits to be handed the shared memory, and the only thing that
// hands it over is the consumer accepting. In the two processes these were
// written for, that is one side waiting a frame for the other. Driven from a
// single thread it is a deadlock, which is how this was found.
//
// Returns false having already reported why, so each test can stop rather than
// cascade.
bool bring_up(ExternalFrameLink& link, ExternalFrameProducer& producer,
              const ExternalStreamInfo& info = wii_u_info()) {
    std::string error;
    if (!link.listen(socket_path(), &error)) {
        test::skip("no shared memory or unix sockets here: " + error);
        return false;
    }

    std::string connect_error;
    bool connected = false;
    std::thread connecting([&]() {
        connected = producer.connect(socket_path(), info, &connect_error);
    });

    // Meanwhile, accept and read the introduction that follows it.
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!link.connected() && std::chrono::steady_clock::now() < give_up_at) {
        if (!link.poll(&error)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    connecting.join();

    CHECK_MSG(connected, "the producer could not connect: " + connect_error);
    CHECK_MSG(link.connected(), "the two ends never agreed on anything: " + error);
    return connected && link.connected();
}

// Reads until something arrives for `screen`, or until it is clear nothing
// will. The two ends are in different threads, so "publish then take" is not
// instantaneous the way it would be in one.
bool wait_for_frame(ExternalFrameLink& link, ScreenId screen, ExternalFrame& out) {
    std::string error;
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < give_up_at) {
        if (!link.poll(&error)) {
            return false;
        }
        if (link.take_frame(screen, out)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void test_two_screens_of_one_moment_arrive_whole() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    CHECK_EQ(link.info().top_width, kTvWidth);
    CHECK_EQ(link.info().bottom_height, kPadHeight);
    CHECK_EQ(link.info().audio_sample_rate, 48000);

    std::vector<uint32_t> tv;
    std::vector<uint32_t> pad;
    fill(tv, kTvWidth, kTvHeight, 0x00204060u);
    fill(pad, kPadWidth, kPadHeight, 0x00901010u);

    std::string error;
    CHECK(producer.publish(ScreenId::Top, 7, tv.data(), kTvWidth, kTvHeight, kTvWidth, &error));
    // Said to have been captured 50ms ago, as a game's readback would say.
    CHECK(producer.publish(ScreenId::Bottom, 7, pad.data(), kPadWidth, kPadHeight, kPadWidth,
                           &error, external_frame_clock_us() - 50000));

    ExternalFrame got;
    CHECK_MSG(wait_for_frame(link, ScreenId::Top, got), "no television frame arrived");
    CHECK_EQ(got.width, kTvWidth);
    CHECK_EQ(got.height, kTvHeight);
    CHECK_MSG(got.serial == 7u, "the television frame arrived under the wrong serial");
    CHECK_MSG(got.pixels == tv,
              "the television picture that arrived is not the one that was sent");

    CHECK_MSG(wait_for_frame(link, ScreenId::Bottom, got), "no GamePad frame arrived");
    CHECK_EQ(got.width, kPadWidth);
    CHECK_MSG(got.pixels == pad, "the GamePad picture that arrived is not the one that was sent");

    // How old each was, by the producer's own capture time.
    const ExternalFrameLink::FrameCounts pad_counts = link.take_frame_counts(ScreenId::Bottom);
    CHECK_EQ(pad_counts.timed_taken, 1u);
    CHECK_MSG(pad_counts.taken_age_us >= 50000 && pad_counts.taken_age_us < 5000000,
              "the GamePad frame's age does not count from when it was captured");
    CHECK_MSG(pad_counts.announced_age_us >= 50000 &&
                  pad_counts.announced_age_us <= pad_counts.taken_age_us,
              "the GamePad frame was older when announced than when taken");
    const ExternalFrameLink::FrameCounts tv_counts = link.take_frame_counts(ScreenId::Top);
    CHECK_EQ(tv_counts.timed_taken, 1u);
    CHECK_MSG(tv_counts.taken_age_us < 5000000,
              "a frame published without a capture time was not timed from its publishing");

    // Taken once. Asking again before anything new is published has to say so,
    // or a stalled producer would look like one running perfectly.
    CHECK_MSG(!link.take_frame(ScreenId::Top, got),
              "the same frame was handed out twice, so a stalled game would look like a running "
              "one");
}

// A producer never waits for a reader. When it laps one, the reader has to
// notice rather than hand out half of one picture and half of another.
//
// This has to be genuinely concurrent to mean anything. Publishing and then
// reading, one after the other, passes whether or not there is any protection
// at all -- which is what the first version of this test did, and what taking
// the sequence numbers out and watching it still pass revealed.
//
// What it establishes, honestly: that a reader under real pressure from a
// writer is never handed a mixture. What it does not establish is the narrower
// window the odd-numbered write phase exists for -- a writer whose whole copy
// falls between a reader's two checks of the sequence. That is a race this
// cannot be made to hit reliably, and a test that catches it one run in twenty
// would be worse than none. The phase is there because the window is real, not
// because anything below proves it.
void test_a_frame_caught_mid_write_is_dropped_rather_than_torn() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    // Every frame is one value repeated, so a mixture of two is obvious: any
    // pixel that disagrees with the first one is two frames in one buffer.
    std::atomic<bool> running{true};
    std::atomic<int> published{0};
    std::thread writing([&]() {
        std::vector<uint32_t> frame(static_cast<std::size_t>(kPadWidth) * kPadHeight);
        for (uint32_t serial = 1; running.load(); ++serial) {
            std::fill(frame.begin(), frame.end(), serial * 2654435761u);
            std::string error;
            if (!producer.publish(ScreenId::Bottom, serial, frame.data(), kPadWidth, kPadHeight,
                                  kPadWidth, &error)) {
                break;
            }
            published.store(static_cast<int>(serial));
        }
    });

    int examined = 0;
    int torn = 0;
    std::string error;
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (examined < 60 && std::chrono::steady_clock::now() < give_up_at) {
        if (!link.poll(&error)) break;
        ExternalFrame got;
        if (!link.take_frame(ScreenId::Bottom, got)) continue;
        ++examined;
        const uint32_t first = got.pixels.front();
        for (const uint32_t pixel : got.pixels) {
            if (pixel != first) {
                ++torn;
                break;
            }
        }
    }
    running.store(false);
    writing.join();

    CHECK_MSG(published.load() > 20,
              "the producer only managed " + std::to_string(published.load()) +
                  " frames, so nothing here was under any pressure");
    CHECK_MSG(examined > 5,
              "only " + std::to_string(examined) + " frames were read while " +
                  std::to_string(published.load()) +
                  " were being written, so this proved nothing about reading during a write");
    CHECK_MSG(torn == 0,
              std::to_string(torn) + " of " + std::to_string(examined) +
                  " frames were two pictures mixed together, so a reader can be handed a frame "
                  "being overwritten");
}

// Only the newest matters. A consumer that fell behind should skip to the
// present rather than work through a backlog of pictures nobody will see.
void test_falling_behind_skips_to_the_newest() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    std::vector<uint32_t> frame;
    std::string error;
    for (uint32_t serial = 1; serial <= 5; ++serial) {
        fill(frame, kPadWidth, kPadHeight, serial * 0x00020202u);
        CHECK(producer.publish(ScreenId::Bottom, serial, frame.data(), kPadWidth, kPadHeight,
                               kPadWidth, &error));
    }
    ExternalFrame got;
    CHECK(wait_for_frame(link, ScreenId::Bottom, got));
    CHECK_MSG(got.serial == 5u,
              "a consumer that fell behind was handed serial " + std::to_string(got.serial) +
                  " rather than the newest, so it would play out a backlog nobody wanted");
}

void test_audio_and_resets_go_the_way_they_are_meant_to() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    std::string error;
    const std::vector<int16_t> sent_tv = {1, -1, 300, -300, 4000, -4000};
    const std::vector<int16_t> sent_pad = {7, -7, 500, -500};
    CHECK(producer.send_audio(ScreenId::Top, sent_tv.data(), sent_tv.size() / 2, &error));
    CHECK(producer.send_audio(ScreenId::Bottom, sent_pad.data(), sent_pad.size() / 2, &error));

    std::vector<int16_t> heard_tv;
    std::vector<int16_t> heard_pad;
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((heard_tv.empty() || heard_pad.empty()) &&
          std::chrono::steady_clock::now() < give_up_at) {
        CHECK(link.poll(&error));
        if (heard_tv.empty()) link.take_audio(ScreenId::Top, heard_tv);
        if (heard_pad.empty()) link.take_audio(ScreenId::Bottom, heard_pad);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(heard_tv == sent_tv, "the TV audio that arrived is not the TV audio that was sent");
    CHECK_MSG(heard_pad == sent_pad,
              "the GamePad audio that arrived is not the GamePad audio that was sent, or leaked "
              "in with the TV's");
    link.take_audio(ScreenId::Top, heard_tv);
    link.take_audio(ScreenId::Bottom, heard_pad);
    CHECK_MSG(heard_tv.empty() && heard_pad.empty(),
              "audio was handed out twice, so it would be played twice");

    // And the other way: only the producer knows what restarting a game means.
    link.request_reset();
    bool reset = false;
    const auto reset_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!reset && std::chrono::steady_clock::now() < reset_by) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(reset, "the game was never told the player had asked for a reset");
    CHECK(producer.poll(reset, &error));
    CHECK_MSG(!reset, "one reset request was delivered twice");
}

// The pad goes the other way from everything else on this link, and it goes as
// latest-state rather than as events: a missed edge would be a button that
// sticks down for the rest of the session.
void test_the_pad_crosses_and_arrives_whole() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    std::string error;
    bool reset = false;
    CHECK(producer.poll(reset, &error));
    CHECK_MSG(!producer.have_input(),
              "the game was handed a pad before anything sent one, so a fork would stop reading "
              "its own controllers for no reason");

    ExternalInput sent;
    sent.set(ExternalButton::Zl, true);
    sent.set(ExternalButton::RightStick, true);
    sent.left_x = -30000;
    sent.right_y = 12345;
    sent.touching = true;
    sent.touch_x = 427;
    sent.touch_y = 240;
    link.send_input(sent);

    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!producer.have_input() && std::chrono::steady_clock::now() < give_up_at) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(producer.have_input(), "the pad never arrived");
    CHECK_MSG(producer.input() == sent,
              "the pad that arrived is not the pad that was sent: buttons " +
                  std::to_string(producer.input().buttons) + " against " +
                  std::to_string(sent.buttons) + ", touch " +
                  std::to_string(producer.input().touch_x) + "," +
                  std::to_string(producer.input().touch_y));
    // Negative deflection has to survive the trip as a signed number rather
    // than as 35536.
    CHECK_EQ(producer.input().left_x, static_cast<int16_t>(-30000));

    // Letting go arrives too, and is a different state rather than silence.
    ExternalInput released;
    link.send_input(released);
    const auto released_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (producer.input() != released && std::chrono::steady_clock::now() < released_by) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(producer.input() == released,
              "letting everything go never reached the game, so a button held as a session ended "
              "would stay held");
    CHECK_MSG(producer.have_input(),
              "an empty pad was reported as never having been sent one, which would put a fork "
              "back on its own controllers mid-game");
}

// The pad is only sent when it differs from the last one sent, which makes what
// was sent a fact about one session. A second game that opened on the same pad
// as the first would otherwise never be told about it.
void test_a_second_game_is_told_the_pad_from_the_beginning() {
    ExternalFrameLink link;
    std::string error;
    bool reset = false;

    ExternalInput pressed;
    pressed.set(ExternalButton::Zr, true);

    for (const char* attempt : {"first", "second"}) {
        ExternalFrameProducer producer;
        if (!bring_up(link, producer)) return;

        link.send_input(pressed);
        const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!producer.have_input() && std::chrono::steady_clock::now() < give_up_at) {
            CHECK(producer.poll(reset, &error));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_MSG(producer.have_input() && producer.input() == pressed,
                  std::string("the ") + attempt + " game was never handed the pad that was "
                  "already being held when it started");
        producer.close();
        // And this end notices, which is what clears what it thinks it has sent.
        for (int poll = 0; poll < 200 && link.connected(); ++poll) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!link.poll(&error)) break;
        }
    }
}

// The GamePad's motor crosses the other way from the pad -- producer to
// consumer -- and only on the edges: take_rumble() is a one-shot, so a caller
// forwarding it onward is never told "on" a second time for a motor that
// never turned off in between.
void test_rumble_crosses_only_on_the_edges() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    bool active = true;
    CHECK_MSG(!link.take_rumble(active), "the motor was reported as having changed before "
                                          "anything was ever sent");

    producer.send_rumble(true);
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool got_on = false;
    while (!got_on && std::chrono::steady_clock::now() < give_up_at) {
        std::string error;
        CHECK(link.poll(&error));
        active = false;
        if (link.take_rumble(active)) got_on = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(got_on, "the motor turning on never arrived");
    CHECK_MSG(active, "the motor turning on arrived as off");

    // Sent again with the same value: nothing crosses, because nothing
    // changed -- a companion holding this device does not need to be told
    // "still on" every time this is polled.
    producer.send_rumble(true);
    std::string error;
    for (int poll = 0; poll < 50; ++poll) {
        CHECK(link.poll(&error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    active = false;
    CHECK_MSG(!link.take_rumble(active),
              "the same motor state was delivered twice, which would be heard as a fresh pulse "
              "rather than one continuous one");

    producer.send_rumble(false);
    bool got_off = false;
    const auto off_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!got_off && std::chrono::steady_clock::now() < off_by) {
        CHECK(link.poll(&error));
        active = true;
        if (link.take_rumble(active)) got_off = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(got_off, "the motor turning off never arrived");
    CHECK_MSG(!active, "the motor turning off arrived as on");
}

// Each of the game's players crosses on its own, apart from the GamePad and
// from one another, and a player nothing feeds any more arrives as unplugged
// rather than as a pad with nothing pressed.
void test_players_cross_apart_from_the_gamepad_and_each_other() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    std::string error;
    bool reset = false;
    CHECK(producer.poll(reset, &error));
    for (int player = 0; player < kExternalPlayers; ++player) {
        CHECK_MSG(!producer.player_connected(player),
                  "a player was connected before anything was sent for it");
    }

    ExternalInput first;
    first.set(ExternalButton::A, true);
    first.left_x = -20000;
    ExternalInput eighth;
    eighth.set(ExternalButton::Plus, true);
    link.send_player_input(0, true, first);
    link.send_player_input(7, true, eighth);
    // Out of range: ignored rather than written somewhere it does not belong.
    link.send_player_input(8, true, first);
    link.send_player_input(-1, true, first);

    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!(producer.player_connected(0) && producer.player_connected(7)) &&
           std::chrono::steady_clock::now() < give_up_at) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(producer.player_connected(0) && producer.player_input(0) == first,
              "Player 1's pad did not arrive as sent");
    CHECK_MSG(producer.player_connected(7) && producer.player_input(7) == eighth,
              "Player 8's pad did not arrive as sent");
    CHECK_MSG(!producer.player_connected(1), "a player nobody sent turned up connected");
    CHECK_MSG(!producer.have_input(), "a player's pad arrived as the GamePad's");

    // Unplugged: the next state says nothing feeds Player 1.
    link.send_player_input(0, false, first);
    const auto unplugged_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (producer.player_connected(0) && std::chrono::steady_clock::now() < unplugged_by) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(!producer.player_connected(0), "a player nothing feeds stayed connected");
    CHECK_MSG(producer.player_input(0) == ExternalInput(),
              "a player nothing feeds kept the buttons it last had held, which would hold them "
              "down in the game for good");
}

// Each player's motor comes back on its own, and two changing at once are two
// edges, in order -- neither is lost to the other.
void test_players_rumble_on_their_own_edges() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    producer.send_player_rumble(2, true);
    producer.send_player_rumble(5, true);
    producer.send_player_rumble(2, true);  // unchanged: nothing crosses
    producer.send_player_rumble(2, false);

    std::vector<std::pair<int, bool>> got;
    std::string error;
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (got.size() < 3 && std::chrono::steady_clock::now() < give_up_at) {
        CHECK(link.poll(&error));
        int player = -1;
        bool active = false;
        while (link.take_player_rumble(player, active)) {
            got.emplace_back(player, active);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int poll = 0; poll < 30; ++poll) {
        CHECK(link.poll(&error));
        int player = -1;
        bool active = false;
        while (link.take_player_rumble(player, active)) {
            got.emplace_back(player, active);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::vector<std::pair<int, bool>> expected{{2, true}, {5, true}, {2, false}};
    CHECK_MSG(got == expected, "the players' motor edges did not arrive once each, in order (" +
                                   std::to_string(got.size()) + " arrived)");
    bool gamepad = false;
    CHECK_MSG(!link.take_rumble(gamepad), "a player's motor arrived as the GamePad's");
}

// The ordinary end of a session is the game being closed, and it has to look
// like that rather than like a stall.
void test_the_game_closing_ends_the_session() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    producer.close();
    std::string error;
    // Whatever is still buffered is read first; the end arrives after it.
    for (int attempt = 0; attempt < 200; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!link.poll(&error)) {
            CHECK_MSG(error.find("closed") != std::string::npos,
                      "the game closing was reported as '" + error + "'");
            return;
        }
    }
    CHECK_MSG(false, "the game closed and this end never noticed");
}

// A producer asking for more than the memory holds has to be refused where the
// numbers are, not by writing past the end of a slot.
void test_a_screen_too_big_to_carry_is_refused() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    std::string error;
    if (!link.listen(socket_path(), &error)) {
        test::skip("no shared memory or unix sockets here: " + error);
        return;
    }
    ExternalStreamInfo huge = wii_u_info();
    huge.top_width = ExternalSharedHeader::kMaxWidth * 2;

    std::thread connecting([&]() {
        std::string ignored;
        producer.connect(socket_path(), huge, &ignored);
    });
    bool refused = false;
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!refused && std::chrono::steady_clock::now() < give_up_at) {
        if (!link.poll(&error)) {
            refused = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    connecting.join();
    CHECK_MSG(refused, "a screen too big to carry was accepted");
    CHECK_MSG(error.find("cannot carry") != std::string::npos,
              "a screen too big to carry was refused without saying so: " + error);

    // And a publish of one, if it somehow got that far.
    ExternalFrameLink second;
    ExternalFrameProducer honest;
    if (!bring_up(second, honest)) return;
    std::vector<uint32_t> small;
    fill(small, 16, 16, 0u);
    CHECK_MSG(!honest.publish(ScreenId::Top, 1, small.data(), ExternalSharedHeader::kMaxWidth + 2,
                              16, 16, &error),
              "a frame wider than the memory was published rather than refused");
}

}  // namespace

// A producer rendering far above what the far end can show should reduce the
// picture itself rather than hand over pixels nobody will ever see. That only
// works if it is told the size, and if both ends agree on what "fits inside"
// means down to the pixel -- otherwise the consumer scales again on a CPU for
// the sake of a couple of rows.
void test_a_game_is_told_how_big_the_screen_is_worth_making() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    std::string error;
    bool reset = false;

    link.request_screen(ScreenId::Bottom, 1280, 800);
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (producer.requested_max_width() == 0 &&
           std::chrono::steady_clock::now() < give_up_at) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(producer.requested_screen().has_value() &&
                  *producer.requested_screen() == ScreenId::Bottom,
              "the screen itself stopped crossing once a size was sent with it");
    CHECK_MSG(producer.requested_max_width() == 1280 && producer.requested_max_height() == 800,
              "the game was never told how big the picture is actually worth making");

    // The arithmetic both ends have to agree on, at the sizes this exists
    // for: a 4K graphics pack feeding a handheld.
    int width = 0;
    int height = 0;
    fit_within(3840, 2160, 1280, 800, width, height);
    CHECK_MSG(width == 1280 && height == 720,
              "fitting 4K inside a 1280x800 display did not land on 720p");
    // Never upwards: a display bigger than the picture is not a reason to
    // invent detail that was never rendered.
    fit_within(854, 480, 1280, 800, width, height);
    CHECK_MSG(width == 854 && height == 480, "a small picture was stretched to fill a display");
    // And no cap at all leaves it alone.
    fit_within(3840, 2160, 0, 0, width, height);
    CHECK_MSG(width == 3840 && height == 2160, "an uncapped request still shrank the picture");
}

// Which screen the producer should show in its own window is a separate
// question from which one it should capture, and the interesting case is the
// one where the second question has no answer: a device playing on its own
// captures nothing at all, and the old rule -- "show whichever screen was not
// asked for" -- has nothing to work from. A handheld alone is a GamePad and
// wants the GamePad shown; a desktop alone is a television.
void test_a_game_is_told_which_screen_to_show_even_when_asked_for_none() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    std::string error;
    bool reset = false;

    // Nothing captured, and the GamePad shown -- one device, no companion.
    link.request_screen(std::nullopt, 0, 0, ScreenId::Bottom);
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!producer.shown_screen().has_value() &&
           std::chrono::steady_clock::now() < give_up_at) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(producer.shown_screen().has_value() &&
                  *producer.shown_screen() == ScreenId::Bottom,
              "a device on its own could not say which screen it is being");
    CHECK_MSG(!producer.requested_screen().has_value(),
              "asking for a screen to be shown was read as asking for it to be sent, which would "
              "make a lone device pay to capture a picture it is not watching");

    // And the two move independently: capturing the television while showing
    // the GamePad is the two-device case, and the one that was silently
    // getting the GamePad's picture labelled as the television's.
    link.request_screen(ScreenId::Top, 1280, 720, ScreenId::Bottom);
    const auto changed_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!producer.requested_screen().has_value() &&
           std::chrono::steady_clock::now() < changed_by) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(producer.requested_screen().has_value() &&
                  *producer.requested_screen() == ScreenId::Top,
              "the captured screen stopped crossing once a shown one was sent with it");
    CHECK_MSG(producer.shown_screen().has_value() && *producer.shown_screen() == ScreenId::Bottom,
              "the shown screen was lost when a captured one was sent alongside it");
}

// Asking for the same thing twice costs nothing -- this is sent from a poll
// loop that runs every frame, and a socket write per frame for a value that
// has not changed is exactly what send_input() already avoids.
void test_asking_for_the_same_screen_twice_says_nothing_the_second_time() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    std::string error;
    bool reset = false;
    link.request_screen(ScreenId::Top, 1280, 800);
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (producer.requested_max_width() == 0 &&
           std::chrono::steady_clock::now() < give_up_at) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(producer.requested_max_width() == 1280);

    // Thousands of repeats of the unchanged request, which would fill and
    // then block on the socket if any of them were actually written.
    for (int repeat = 0; repeat < 5000; ++repeat) {
        link.request_screen(ScreenId::Top, 1280, 800);
    }
    // A changed one still gets through, so the dedupe is not simply stuck.
    link.request_screen(ScreenId::Top, 640, 400);
    const auto changed_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (producer.requested_max_width() != 640 &&
           std::chrono::steady_clock::now() < changed_by) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(producer.requested_max_width() == 640 && producer.requested_max_height() == 400,
              "a changed request never reached the game");

    // Including a change to nothing but which screen the far end should show.
    // That one is the whole of what a player flipping this device between
    // being a television and being a GamePad changes, so a dedupe that only
    // watched the other three fields would swallow it entirely.
    link.request_screen(ScreenId::Top, 640, 400, ScreenId::Bottom);
    const auto shown_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!producer.shown_screen().has_value() &&
           std::chrono::steady_clock::now() < shown_by) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(producer.shown_screen().has_value() && *producer.shown_screen() == ScreenId::Bottom,
              "a change to only the shown screen was swallowed by the dedupe");
}

// How the device standing in for the GamePad is being held, all the way to
// the producer. Raw and unconverted on purpose -- see ExternalMotion -- so
// what this checks is that the numbers arrive as the numbers that were sent,
// down to the fractions a gyro at rest actually reports.
void test_how_the_pad_is_being_held_crosses_too() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    if (!bring_up(link, producer)) return;

    std::string error;
    bool reset = false;

    ExternalInput sent;
    sent.motion.present = true;
    sent.motion.gyro_x = -0.0125f;
    sent.motion.gyro_y = 1.75f;
    sent.motion.gyro_z = 0.5f;
    sent.motion.accel_x = 0.03f;
    sent.motion.accel_y = -9.79f;
    sent.motion.accel_z = 0.42f;
    sent.motion.sampled_us = 4000000000u;
    link.send_input(sent);

    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!producer.have_input() && std::chrono::steady_clock::now() < give_up_at) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(producer.have_input(), "the pad never arrived");
    CHECK_MSG(producer.input().motion == sent.motion,
              "how the pad is being held did not survive the trip");
    CHECK_MSG(producer.input().motion.gyro_x == -0.0125f,
              "a small fractional reading was lost or rounded away, which is most of what a "
              "gyroscope at rest reports");
    CHECK_EQ(producer.input().motion.sampled_us, 4000000000u);

    // A pad with everything else identical but no motion at all is a
    // different pad, not the same one -- otherwise a device whose sensors
    // stopped would leave the last reading standing for ever, since only
    // changes are sent.
    ExternalInput without = sent;
    without.motion = ExternalMotion();
    CHECK_MSG(without != sent, "a pad that stopped reporting motion looks unchanged");
    link.send_input(without);
    const auto cleared_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (producer.input().motion.present && std::chrono::steady_clock::now() < cleared_by) {
        CHECK(producer.poll(reset, &error));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(!producer.input().motion.present,
              "the game was never told the device had stopped reporting how it is held");
}

// This app's quick menu drawn over a Wii U game in Cemu's window: a producer
// that says it can draw one is handed the pixels, told when they are taken
// down, and its menu key comes back the other way. One that does not say so is
// never offered any.
void test_the_menu_crosses_to_be_drawn_over_the_game() {
    {
        ExternalFrameLink link;
        ExternalFrameProducer producer;
        if (!bring_up(link, producer)) return;
        CHECK_MSG(!link.overlay_supported(),
                  "a producer that never said it can draw an overlay was taken to");
        const std::vector<uint8_t> pixel = {1, 2, 3, 4};
        CHECK(!link.publish_overlay(pixel.data(), 1, 1));
    }

    ExternalFrameLink link;
    ExternalFrameProducer producer;
    ExternalStreamInfo info = wii_u_info();
    info.features = kExternalFeatureOverlay;
    if (!bring_up(link, producer, info)) return;
    CHECK(producer.overlay_available());
    CHECK_MSG(link.overlay_supported(), "the producer's offer of an overlay was not heard");

    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    std::vector<uint8_t> menu(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (std::size_t i = 0; i < menu.size(); ++i) menu[i] = static_cast<uint8_t>(i % 253);
    CHECK(link.publish_overlay(menu.data(), kWidth, kHeight));
    // Nothing bigger than the area can hold.
    CHECK(!link.publish_overlay(menu.data(), ExternalOverlayArea::kMaxWidth + 1, 1));

    // Waits for the producer to have something, the way a fork's render loop
    // polls once a frame.
    auto wait_for = [&](auto&& arrived) {
        const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < give_up_at) {
            bool reset = false;
            std::string error;
            CHECK(producer.poll(reset, &error));
            std::string link_error;
            CHECK(link.poll(&link_error));
            if (arrived()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };

    std::vector<uint8_t> got;
    int width = 0;
    int height = 0;
    CHECK_MSG(wait_for([&]() { return producer.take_overlay(got, width, height); }),
              "the menu never reached the producer");
    CHECK(producer.overlay_visible());
    CHECK_EQ(width, kWidth);
    CHECK_EQ(height, kHeight);
    CHECK_MSG(got == menu, "the menu arrived different from how it was sent");
    // Taken once: the same picture is not handed over again every frame.
    CHECK(!producer.take_overlay(got, width, height));

    // The cursor moves: the next picture goes into the other slot.
    menu[0] = 99;
    CHECK(link.publish_overlay(menu.data(), kWidth, kHeight));
    CHECK(wait_for([&]() { return producer.take_overlay(got, width, height); }));
    CHECK_EQ(static_cast<int>(got[0]), 99);
    CHECK(producer.overlay_takes_input());

    // A notice instead: drawn the same way, but the game keeps its input.
    menu[0] = 7;
    CHECK(link.publish_overlay(menu.data(), kWidth, kHeight, /*takes_input=*/false));
    CHECK(wait_for([&]() { return producer.take_overlay(got, width, height); }));
    CHECK_EQ(static_cast<int>(got[0]), 7);
    CHECK(producer.overlay_visible());
    CHECK_MSG(!producer.overlay_takes_input(), "a notice took the game's input");

    link.hide_overlay();
    CHECK_MSG(wait_for([&]() { return !producer.overlay_visible(); }),
              "the menu closing never reached the producer");
    CHECK(!producer.take_overlay(got, width, height));

    // The Pointer setting: shown until said otherwise, and only a change
    // crosses.
    CHECK(producer.pointer_shown());
    link.set_pointer_shown(false);
    CHECK_MSG(wait_for([&]() { return !producer.pointer_shown(); }),
              "the pointer being hidden never reached the producer");
    link.set_pointer_shown(true);
    CHECK_MSG(wait_for([&]() { return producer.pointer_shown(); }),
              "the pointer being shown again never reached the producer");

    // A click on the menu in the game's window, handed back as fractions of
    // the menu's size.
    float press_x = 0.0f, press_y = 0.0f, tap_x = 0.0f, tap_y = 0.0f;
    CHECK(!link.take_overlay_tap(press_x, press_y, tap_x, tap_y));
    producer.send_overlay_tap(0.25f, 0.5f, 0.75f, 1.0f);
    CHECK_MSG(wait_for([&]() { return link.take_overlay_tap(press_x, press_y, tap_x, tap_y); }),
              "a tap on the menu never came back");
    CHECK(std::abs(press_x - 0.25f) < 0.001f && std::abs(press_y - 0.5f) < 0.001f);
    CHECK(std::abs(tap_x - 0.75f) < 0.001f && std::abs(tap_y - 1.0f) < 0.001f);

    // Tab in the game's window, which only it can hear.
    CHECK(!link.take_menu_key());
    producer.send_menu_key();
    CHECK_MSG(wait_for([&]() { return link.take_menu_key(); }),
              "the menu key never came back");
    CHECK(!link.take_menu_key());

    // The keys that move through the menu, down and up, in order.
    ExternalMenuKey key = ExternalMenuKey::Up;
    bool down = false;
    CHECK(!link.take_menu_keyboard(key, down));
    producer.send_menu_keyboard(ExternalMenuKey::Down, true);
    producer.send_menu_keyboard(ExternalMenuKey::Down, false);
    producer.send_menu_keyboard(ExternalMenuKey::Back, true);
    CHECK_MSG(wait_for([&]() { return link.take_menu_keyboard(key, down); }),
              "a menu key pressed in the game's window never came back");
    CHECK(key == ExternalMenuKey::Down && down);
    CHECK(wait_for([&]() { return link.take_menu_keyboard(key, down); }));
    CHECK(key == ExternalMenuKey::Down && !down);
    CHECK(wait_for([&]() { return link.take_menu_keyboard(key, down); }));
    CHECK(key == ExternalMenuKey::Back && down);
    CHECK(!link.take_menu_keyboard(key, down));
}

void test_the_game_pauses_only_when_it_says_so() {
    {
        ExternalFrameLink link;
        ExternalFrameProducer producer;
        if (!bring_up(link, producer)) return;
        CHECK_MSG(!link.pause_supported(),
                  "a producer that never said it can pause was taken to");
    }

    ExternalFrameLink link;
    ExternalFrameProducer producer;
    ExternalStreamInfo info = wii_u_info();
    info.features = kExternalFeatureOverlay | kExternalFeaturePause;
    if (!bring_up(link, producer, info)) return;
    CHECK_MSG(link.pause_supported(), "the producer's offer to pause was not heard");

    auto wait_for = [&](auto&& arrived) {
        const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < give_up_at) {
            bool reset = false;
            std::string error;
            CHECK(producer.poll(reset, &error));
            std::string link_error;
            CHECK(link.poll(&link_error));
            if (arrived()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };

    bool pause = false;
    CHECK(!producer.take_pause_request(pause));
    link.request_pause(true);
    // Asking again before anything changed says nothing more.
    link.request_pause(true);
    CHECK_MSG(wait_for([&]() { return producer.take_pause_request(pause); }),
              "the request to pause never reached the producer");
    CHECK(pause);
    CHECK_MSG(!wait_for([&]() { return producer.take_pause_request(pause); }),
              "the same request crossed twice");

    // Nothing is paused until the producer says so.
    ExternalPauseState state = ExternalPauseState::Running;
    CHECK(!link.take_pause_state(state));
    producer.send_pause_state(ExternalPauseState::Paused);
    CHECK_MSG(wait_for([&]() { return link.take_pause_state(state); }),
              "the game pausing was never heard");
    CHECK(state == ExternalPauseState::Paused);

    link.request_pause(false);
    CHECK(wait_for([&]() { return producer.take_pause_request(pause); }));
    CHECK(!pause);
    producer.send_pause_state(ExternalPauseState::Running);
    CHECK(wait_for([&]() { return link.take_pause_state(state); }));
    CHECK(state == ExternalPauseState::Running);

    // A game that cannot stop says so, and in order with what came before.
    link.request_pause(true);
    CHECK(wait_for([&]() { return producer.take_pause_request(pause); }));
    producer.send_pause_state(ExternalPauseState::Refused);
    producer.send_pause_state(ExternalPauseState::Running);
    CHECK(wait_for([&]() { return link.take_pause_state(state); }));
    CHECK(state == ExternalPauseState::Refused);
    CHECK(wait_for([&]() { return link.take_pause_state(state); }));
    CHECK(state == ExternalPauseState::Running);

    // And a game that never listens for the HOME Menu, which says so at once.
    link.request_pause(false);
    CHECK(wait_for([&]() { return producer.take_pause_request(pause); }));
    link.request_pause(true);
    CHECK(wait_for([&]() { return producer.take_pause_request(pause); }));
    producer.send_pause_state(ExternalPauseState::NotListening);
    CHECK(wait_for([&]() { return link.take_pause_state(state); }));
    CHECK(state == ExternalPauseState::NotListening);
}

void test_graphics_packs_are_listed_and_changed() {
    {
        ExternalFrameLink link;
        ExternalFrameProducer producer;
        if (!bring_up(link, producer)) return;
        CHECK_MSG(!link.graphic_packs_supported(),
                  "a producer that never offered graphics packs was taken to");
    }

    ExternalFrameLink link;
    ExternalFrameProducer producer;
    ExternalStreamInfo info = wii_u_info();
    info.features = kExternalFeatureOverlay | kExternalFeatureGraphicPacks;
    if (!bring_up(link, producer, info)) return;
    CHECK_MSG(link.graphic_packs_supported(), "the producer's graphics packs were not heard of");

    auto wait_for = [&](auto&& arrived) {
        const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < give_up_at) {
            bool reset = false;
            std::string error;
            CHECK(producer.poll(reset, &error));
            std::string link_error;
            CHECK(link.poll(&link_error));
            if (arrived()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };

    CHECK(!producer.take_graphic_packs_wanted());
    link.request_graphic_packs();
    CHECK_MSG(wait_for([&]() { return producer.take_graphic_packs_wanted(); }),
              "the request for graphics packs never reached the producer");

    std::vector<ExternalGraphicPack> sent(2);
    sent[0].id = "graphicPacks/Game_Resolution/rules.txt";
    sent[0].path = "Some Game/Graphics";
    sent[0].name = "Graphics";
    sent[0].description = "Changes the resolution.";
    sent[0].enabled = true;
    sent[0].restart_needed = true;
    sent[0].choices.push_back({"TV Resolution", {"1280x720", "1920x1080", "2560x1440"}, 1});
    sent[0].choices.push_back({"", {"Low", "High"}, -1});
    sent[1].id = "graphicPacks/Game_FPS/rules.txt";
    sent[1].path = "Some Game/Mods/FPS++";
    sent[1].name = "FPS++";
    producer.send_graphic_packs(sent);

    std::vector<ExternalGraphicPack> got;
    CHECK_MSG(wait_for([&]() { return link.take_graphic_packs(got); }),
              "the graphics packs never arrived");
    CHECK_EQ(got.size(), std::size_t{2});
    if (got.size() == 2) {
        CHECK(got[0].id == sent[0].id);
        CHECK(got[0].path == sent[0].path);
        CHECK(got[0].name == "Graphics");
        CHECK(got[0].description == sent[0].description);
        CHECK(got[0].enabled && got[0].restart_needed);
        CHECK_EQ(got[0].choices.size(), std::size_t{2});
        if (got[0].choices.size() == 2) {
            CHECK(got[0].choices[0].category == "TV Resolution");
            CHECK(got[0].choices[0].presets == sent[0].choices[0].presets);
            CHECK_EQ(got[0].choices[0].active, 1);
            CHECK(got[0].choices[1].category.empty());
            CHECK_EQ(got[0].choices[1].active, -1);
        }
        CHECK(!got[1].enabled && !got[1].restart_needed && got[1].choices.empty());
    }
    // Handed over once.
    CHECK(!link.take_graphic_packs(got));

    ExternalGraphicPackChange change;
    CHECK(!producer.take_graphic_pack_change(change));
    ExternalGraphicPackChange turn_on;
    turn_on.id = sent[1].id;
    turn_on.enabled = true;
    link.change_graphic_pack(turn_on);
    ExternalGraphicPackChange choose;
    choose.id = sent[0].id;
    choose.kind = ExternalGraphicPackChange::Kind::Preset;
    choose.category = "TV Resolution";
    choose.preset = "2560x1440";
    link.change_graphic_pack(choose);
    CHECK_MSG(wait_for([&]() { return producer.take_graphic_pack_change(change); }),
              "the change never reached the producer");
    CHECK(change.id == sent[1].id && change.kind == ExternalGraphicPackChange::Kind::Enabled &&
          change.enabled);
    CHECK(wait_for([&]() { return producer.take_graphic_pack_change(change); }));
    CHECK(change.id == sent[0].id && change.kind == ExternalGraphicPackChange::Kind::Preset &&
          change.category == "TV Resolution" && change.preset == "2560x1440");

    // A game with none at all says so too.
    producer.send_graphic_packs({});
    got.assign(1, ExternalGraphicPack());
    CHECK(wait_for([&]() { return link.take_graphic_packs(got); }));
    CHECK(got.empty());
}

void test_options_and_amiibo_cross() {
    ExternalFrameLink link;
    ExternalFrameProducer producer;
    ExternalStreamInfo info = wii_u_info();
    info.features = kExternalFeatureOverlay;
    {
        ExternalFrameLink older;
        ExternalFrameProducer quiet;
        if (!bring_up(older, quiet, info)) return;
        CHECK(!older.options_supported() && !older.amiibo_supported());
    }
    info.features = kExternalFeatureOverlay | kExternalFeatureOptions | kExternalFeatureAmiibo;
    if (!bring_up(link, producer, info)) return;
    CHECK(link.options_supported() && link.amiibo_supported());

    auto wait_for = [&](auto&& arrived) {
        const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < give_up_at) {
            bool reset = false;
            std::string error;
            CHECK(producer.poll(reset, &error));
            std::string link_error;
            CHECK(link.poll(&link_error));
            if (arrived()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };

    link.request_options();
    CHECK_MSG(wait_for([&]() { return producer.take_options_wanted(); }),
              "the request for options never reached the producer");
    std::vector<ExternalOption> sent(2);
    sent[0].id = "upscale";
    sent[0].label = "Upscaling";
    sent[0].choices = {"Bilinear", "Bicubic", "Nearest neighbor"};
    sent[0].active = 1;
    sent[1].id = "language";
    sent[1].label = "Console language";
    sent[1].choices = {"English", "Japanese"};
    sent[1].restart_needed = true;
    sent[0].default_choice = 1;
    producer.send_options(sent);
    std::vector<ExternalOption> got;
    CHECK_MSG(wait_for([&]() { return link.take_options(got); }), "the options never arrived");
    CHECK_EQ(got.size(), std::size_t{2});
    if (got.size() == 2) {
        CHECK(got[0].id == "upscale" && got[0].label == "Upscaling" && got[0].choices == sent[0].choices);
        CHECK_EQ(got[0].active, 1);
        CHECK(!got[0].restart_needed && got[1].restart_needed);
        CHECK_EQ(got[1].active, -1);
        CHECK_EQ(got[0].default_choice, 1);
        CHECK_EQ(got[1].default_choice, -1);
    }
    CHECK(!link.take_options(got));

    link.change_option("upscale", 2);
    std::string id;
    int choice = -1;
    CHECK_MSG(wait_for([&]() { return producer.take_option_change(id, choice); }),
              "the change of option never reached the producer");
    CHECK(id == "upscale");
    CHECK_EQ(choice, 2);

    link.scan_amiibo("/home/deck/amiibo/Link.bin");
    std::string path;
    CHECK_MSG(wait_for([&]() { return producer.take_amiibo_scan(path); }),
              "the amiibo never reached the producer");
    CHECK(path == "/home/deck/amiibo/Link.bin");
    producer.send_amiibo_result(ExternalAmiiboResult::NotAnAmiibo);
    ExternalAmiiboResult result = ExternalAmiiboResult::Scanned;
    CHECK_MSG(wait_for([&]() { return link.take_amiibo_result(result); }),
              "how the scan went never came back");
    CHECK(result == ExternalAmiiboResult::NotAnAmiibo);
}

int main() {
    test_two_screens_of_one_moment_arrive_whole();
    test_a_frame_caught_mid_write_is_dropped_rather_than_torn();
    test_falling_behind_skips_to_the_newest();
    test_audio_and_resets_go_the_way_they_are_meant_to();
    test_the_pad_crosses_and_arrives_whole();
    test_a_second_game_is_told_the_pad_from_the_beginning();
    test_rumble_crosses_only_on_the_edges();
    test_players_cross_apart_from_the_gamepad_and_each_other();
    test_players_rumble_on_their_own_edges();
    test_the_game_closing_ends_the_session();
    test_a_screen_too_big_to_carry_is_refused();
    test_a_game_is_told_how_big_the_screen_is_worth_making();
    test_a_game_is_told_which_screen_to_show_even_when_asked_for_none();
    test_asking_for_the_same_screen_twice_says_nothing_the_second_time();
    test_how_the_pad_is_being_held_crosses_too();
    test_the_menu_crosses_to_be_drawn_over_the_game();
    test_the_game_pauses_only_when_it_says_so();
    test_graphics_packs_are_listed_and_changed();
    test_options_and_amiibo_cross();
    return test::finish("external-link");
}

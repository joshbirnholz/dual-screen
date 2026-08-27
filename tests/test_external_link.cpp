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
    CHECK(producer.publish(ScreenId::Bottom, 7, pad.data(), kPadWidth, kPadHeight, kPadWidth,
                           &error));

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
    const std::vector<int16_t> sent = {1, -1, 300, -300, 4000, -4000};
    CHECK(producer.send_audio(sent.data(), sent.size() / 2, &error));

    std::vector<int16_t> heard;
    const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (heard.empty() && std::chrono::steady_clock::now() < give_up_at) {
        CHECK(link.poll(&error));
        link.take_audio(heard);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(heard == sent, "the audio that arrived is not the audio that was sent");
    link.take_audio(heard);
    CHECK_MSG(heard.empty(), "audio was handed out twice, so it would be played twice");

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

int main() {
    test_two_screens_of_one_moment_arrive_whole();
    test_a_frame_caught_mid_write_is_dropped_rather_than_torn();
    test_falling_behind_skips_to_the_newest();
    test_audio_and_resets_go_the_way_they_are_meant_to();
    test_the_pad_crosses_and_arrives_whole();
    test_a_second_game_is_told_the_pad_from_the_beginning();
    test_the_game_closing_ends_the_session();
    test_a_screen_too_big_to_carry_is_refused();
    return test::finish("external-link");
}

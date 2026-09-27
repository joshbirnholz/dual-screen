#include "local_play.hpp"

#if DS_LOCAL_PLAY

#ifdef __SWITCH__
#include <pthread.h>
#include <switch.h>

#include <atomic>
#include <new>
#endif

#include "app.hpp"
#include "util/log.hpp"

#ifdef __SWITCH__
// Where each new thread runs.
//
// Every thread a program here starts through pthreads -- which is how
// melonDS starts its 3D renderer, how SDL starts the audio, and how std::thread
// starts this app's own network and encoder threads -- is created on the
// process's default core, the one the main thread is on, and stays there. So a
// game played here had one of the Switch's three cores doing everything: the
// emulated CPUs, the renderer that was meant to run beside them, and the rest,
// taking turns. melonDS's own Switch port (RSDuck's) starts its threads on
// cores 1 and 2 for exactly this reason. This does the same for every thread
// started while a game is played here, alternating between the cores this
// process may use other than the main thread's.
//
// Through the linker (-Wl,--wrap=pthread_create in switch/Makefile) because
// the threads are started inside melonDS, SDL and the C++ library, none of
// which can be asked. The thread itself moves to its core as the first thing
// it does, so nothing about what pthreads hands back changes.
extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                                     void* (*start)(void*), void* argument);

namespace {

std::atomic<u32> g_spare_cores{0};  // a mask; empty while not spreading threads
std::atomic<u32> g_next_spare{0};

struct ThreadStart {
    void* (*start)(void*);
    void* argument;
    s32 core;
};

void* start_on_core(void* raw) {
    const ThreadStart start = *static_cast<ThreadStart*>(raw);
    delete static_cast<ThreadStart*>(raw);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, start.core, 1u << start.core);
    return start.start(start.argument);
}

s32 next_spare_core() {
    const u32 spare = g_spare_cores.load();
    if (spare == 0) return -1;
    int count = 0;
    for (u32 bits = spare; bits != 0; bits &= bits - 1) ++count;
    int pick = static_cast<int>(g_next_spare.fetch_add(1) % static_cast<u32>(count));
    for (s32 core = 0; core < 32; ++core) {
        if ((spare & (1u << core)) == 0) continue;
        if (pick-- == 0) return core;
    }
    return -1;
}

}  // namespace

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                                     void* (*start)(void*), void* argument) {
    const s32 core = next_spare_core();
    if (core >= 0) {
        if (auto* moved = new (std::nothrow) ThreadStart{start, argument, core}) {
            const int result = __real_pthread_create(thread, attributes, start_on_core, moved);
            if (result != 0) delete moved;
            return result;
        }
    }
    return __real_pthread_create(thread, attributes, start, argument);
}
#endif

namespace ds::sw {

#ifdef __SWITCH__
namespace {

// The cores a thread started now may go to: the ones this process is allowed,
// less the main thread's -- which has the emulated CPUs to itself -- and less
// core 3, which the system keeps for its own work.
u32 spare_cores() {
    u64 allowed = 0;
    if (R_FAILED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        allowed = 0x7;  // cores 0 to 2, which every application gets
    }
    const u32 main_core = svcGetCurrentProcessorNumber();
    u32 spare = static_cast<u32>(allowed) & 0x7u & ~(1u << main_core);
    return spare;
}

// The CPU at 1785 MHz while a game plays here, rather than the 1020 MHz every
// program gets by default: the speed the Switch's own games are allowed while
// they load, and the one melonDS's Switch port offers as its fastest setting.
// A DS's two CPUs emulated on one Switch core need every bit of it. Put back to
// what it was when the game ends.
//
// The system resets the clock whenever the console is docked or undocked, and
// may when this program is left and returned to, so it is set again each time
// the applet says one of those has happened.
constexpr u32 kPlayingCpuHz = 1785000000u;

class CpuBoost {
public:
    CpuBoost() {
        use_pcv_ = hosversionBefore(8, 0, 0);
        if (use_pcv_) {
            ready_ = R_SUCCEEDED(pcvInitialize());
            if (ready_ && R_FAILED(pcvGetClockRate(PcvModule_CpuBus, &original_hz_))) {
                original_hz_ = 0;
            }
        } else {
            ready_ = R_SUCCEEDED(clkrstInitialize());
            if (ready_) {
                if (R_FAILED(clkrstOpenSession(&session_, PcvModuleId_CpuBus, 3))) {
                    clkrstExit();
                    ready_ = false;
                } else if (R_FAILED(clkrstGetClockRate(&session_, &original_hz_))) {
                    original_hz_ = 0;
                }
            }
        }
        if (!ready_) {
            log_warn("cannot reach the CPU clock; the game runs at the speed the system chose");
            return;
        }
        apply(kPlayingCpuHz);
        appletHook(&hook_, on_applet_event, this);
        log_info("CPU at " + std::to_string(kPlayingCpuHz / 1000000) + " MHz while playing (was " +
                 std::to_string(original_hz_ / 1000000) + " MHz)");
    }

    ~CpuBoost() {
        if (!ready_) return;
        appletUnhook(&hook_);
        apply(original_hz_ != 0 ? original_hz_ : 1020000000u);
        if (use_pcv_) {
            pcvExit();
        } else {
            clkrstCloseSession(&session_);
            clkrstExit();
        }
    }

    CpuBoost(const CpuBoost&) = delete;
    CpuBoost& operator=(const CpuBoost&) = delete;

private:
    static void on_applet_event(AppletHookType hook, void* self) {
        if (hook == AppletHookType_OnOperationMode || hook == AppletHookType_OnPerformanceMode ||
            hook == AppletHookType_OnFocusState || hook == AppletHookType_OnResume) {
            static_cast<CpuBoost*>(self)->apply(kPlayingCpuHz);
        }
    }

    void apply(u32 hz) {
        const Result result =
            use_pcv_ ? pcvSetClockRate(PcvModule_CpuBus, hz) : clkrstSetClockRate(&session_, hz);
        if (R_FAILED(result)) {
            log_warn("the CPU clock would not change to " + std::to_string(hz / 1000000) +
                     " MHz (error " + std::to_string(result) + ")");
        }
    }

    bool use_pcv_ = false;
    bool ready_ = false;
    u32 original_hz_ = 0;
    ClkrstSession session_{};
    AppletHookCookie hook_{};
};

}  // namespace
#endif

int play_here(const LocalPlayRequest& request) {
    AppOptions options;
    options.game_path = request.game_path;
    options.verbose = request.verbose;
    options.windowed = request.windowed;
    options.exit_after_seconds = request.exit_after_seconds;
    options.screenshot_at_seconds = request.screenshot_at_seconds;
    options.screenshot_path = request.screenshot_path;
    // Whatever would take the player back to the desktop app's own main menu
    // leaves instead: the menu they came from is this program's, and main()
    // brings it back. See AppOptions::leave_instead_of_launcher.
    options.leave_instead_of_launcher = true;
    options.keep_log_file = true;
    log_info(request.game_path.empty() ? "playing here: opening the game browser"
                                       : "playing here: " + request.game_path);
#ifdef __SWITCH__
    const u32 spare = spare_cores();
    g_spare_cores.store(spare);
    std::string cores;
    for (u32 core = 0; core < 3; ++core) {
        if (spare & (1u << core)) cores += (cores.empty() ? "" : " and ") + std::to_string(core);
    }
    log_info("the main thread is on core " + std::to_string(svcGetCurrentProcessorNumber()) +
             (cores.empty() ? std::string("; no other core is free for the rest")
                            : "; threads started while playing go to core " + cores));
    CpuBoost boost;
#endif
    App app;
    const int status = app.run(options);
#ifdef __SWITCH__
    g_spare_cores.store(0);
#endif
    log_info("finished playing here (status " + std::to_string(status) + ")");
    return status;
}

bool relaunch(const std::string& self_path, const std::string& arguments) {
#ifdef __SWITCH__
    // Only a program started by hbloader -- the homebrew menu, or a
    // forwarder -- has somewhere to ask, and only it can be started again.
    if (self_path.empty() || !envHasNextLoad()) {
        log_warn("cannot ask to be started again: this was not started by the homebrew loader");
        return false;
    }
    // The argument string is the whole command line, the program's own path
    // first, the same as argv. Quoted, because a folder on an SD card can have
    // a space in its name.
    std::string command_line = "\"" + self_path + "\"";
    if (!arguments.empty()) {
        command_line += " " + arguments;
    }
    const Result result = envSetNextLoad(self_path.c_str(), command_line.c_str());
    if (R_FAILED(result)) {
        log_warn("the loader would not start this again (error " + std::to_string(result) + ")");
        return false;
    }
    log_info("starting again as: " + command_line);
    return true;
#else
    (void)self_path;
    (void)arguments;
    return false;
#endif
}

}  // namespace ds::sw

#endif  // DS_LOCAL_PLAY

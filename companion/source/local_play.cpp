#include "local_play.hpp"

#if DS_LOCAL_PLAY

#ifdef __SWITCH__
#include <pthread.h>
#include <switch.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#endif

#include "app.hpp"
#include "util/log.hpp"
#ifdef __SWITCH__
#include "switch_forwarder.hpp"
#endif

#ifdef __SWITCH__
// libnx's: what the program does once main() returns -- 1 ends the process
// rather than going back to the loader that started it. See leave_to_home().
extern "C" u32 __nx_applet_exit_mode;

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

#ifdef __SWITCH__
// What melonDS's JIT calls when the Switch reports a memory fault the JIT
// cannot account for -- a real crash rather than one of the deliberate faults
// its fast memory relies on. melonDS declares it and leaves it to the program
// around it (its own Switch port defined one; libretro's frontends never
// linked the JIT on a Switch, so none does), and a build with the JIT does
// not link without it.
//
// Says where, in the game log, and returns: returning is what lets the
// system's own crash handling take over, as it would have anyway. The
// addresses are also given relative to the start of the program, which is
// what `aarch64-none-elf-addr2line -e Dualscreen.elf` wants to name the line.
// The types are spelled as melonDS declares them (u64 and u32 are unsigned
// long and unsigned int there), since C++ links by them.
extern "C" char __start__;

void HandleFault(unsigned long pc, unsigned long lr, unsigned long fp, unsigned long fault_address,
                 unsigned int description) {
    const unsigned long start = reinterpret_cast<unsigned long>(&__start__);
    char line[256];
    std::snprintf(line, sizeof(line),
                  "crashed: a memory fault the JIT could not handle (type %u) at pc 0x%lx "
                  "(program + 0x%lx), returning to 0x%lx (program + 0x%lx), reaching for 0x%lx, "
                  "frame 0x%lx",
                  description, pc, pc - start, lr, lr - start, fault_address, fp);
    ds::log_error(line);
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
// the applet says one of those has happened. It also resets it at times no
// applet message marks -- a race in Mario Kart ran at 1020 MHz from a
// 1.3-second stall onwards, every frame 60% slower -- so a thread looks once a
// second as well, and sets it again whenever it has changed.
constexpr u32 kPlayingCpuHz = 1785000000u;

// The clock /switch/dualscreen/melonds.ini asks for instead, with a line like
// cpu_mhz=1020 -- one of the speeds melonDS's own Switch app offers -- for
// comparing a game here with that app at the same speed. 0 for none.
u32 cpu_hz_asked() {
    std::FILE* file = std::fopen("/switch/dualscreen/melonds.ini", "r");
    if (file == nullptr) return 0;
    u32 asked = 0;
    char line[128];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        unsigned mhz = 0;
        if (std::sscanf(line, " cpu_mhz = %u", &mhz) != 1) continue;
        static const unsigned kOffered[] = {1785, 1581, 1224, 1020, 918, 816, 714};
        for (const unsigned offered : kOffered) {
            if (mhz == offered) asked = offered * 1000000u;
        }
        if (asked == 0) {
            log_warn("melonds.ini asks for the CPU at " + std::to_string(mhz) +
                     " MHz, which is not one of the speeds melonDS offers; staying at " +
                     std::to_string(kPlayingCpuHz / 1000000));
        }
    }
    std::fclose(file);
    return asked;
}

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
        const u32 asked = cpu_hz_asked();
        target_hz_ = asked != 0 ? asked : kPlayingCpuHz;
        apply(target_hz_);
        appletHook(&hook_, on_applet_event, this);
        if (!use_pcv_) {
            gpu_open_ = R_SUCCEEDED(clkrstOpenSession(&gpu_session_, PcvModuleId_GPU, 3));
            memory_open_ = R_SUCCEEDED(clkrstOpenSession(&memory_session_, PcvModuleId_EMC, 3));
        }
        log_info("CPU at " + std::to_string(target_hz_ / 1000000) + " MHz while playing (was " +
                 std::to_string(original_hz_ / 1000000) + " MHz)" +
                 (asked != 0 ? ", as melonds.ini says" : ""));
        watchdog_ = std::thread([this] { watch(); });
    }

    ~CpuBoost() {
        if (!ready_) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (watchdog_.joinable()) watchdog_.join();
        appletUnhook(&hook_);
        apply(original_hz_ != 0 ? original_hz_ : 1020000000u);
        if (gpu_open_) clkrstCloseSession(&gpu_session_);
        if (memory_open_) clkrstCloseSession(&memory_session_);
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
            auto* boost = static_cast<CpuBoost*>(self);
            boost->apply(boost->target_hz_);
        }
    }

    u32 current() {
        u32 hz = 0;
        const Result result = use_pcv_ ? pcvGetClockRate(PcvModule_CpuBus, &hz)
                                       : clkrstGetClockRate(&session_, &hz);
        return R_SUCCEEDED(result) ? hz : 0;
    }

    void watch() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; })) {
            note_gpu_clocks();
            const u32 hz = current();
            if (hz == 0 || hz == target_hz_) continue;
            log_info("the system put the CPU at " + std::to_string(hz / 1000000) +
                     " MHz; back to " + std::to_string(target_hz_ / 1000000));
            apply(target_hz_);
        }
    }

    // The GPU's and the memory's clocks, said when they change: the system
    // sets those too, and a game drawn on the GPU is as fast as they are.
    void note_gpu_clocks() {
        u32 gpu = 0, memory = 0;
        if (gpu_open_) clkrstGetClockRate(&gpu_session_, &gpu);
        if (memory_open_) clkrstGetClockRate(&memory_session_, &memory);
        if (gpu == said_gpu_hz_ && memory == said_memory_hz_) return;
        said_gpu_hz_ = gpu;
        said_memory_hz_ = memory;
        log_info("GPU at " + std::to_string(gpu / 1000000) + " MHz, memory at " +
                 std::to_string(memory / 1000000) + " MHz");
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
    u32 target_hz_ = kPlayingCpuHz;
    ClkrstSession session_{};
    ClkrstSession gpu_session_{};
    ClkrstSession memory_session_{};
    bool gpu_open_ = false;
    bool memory_open_ = false;
    u32 said_gpu_hz_ = 0;
    u32 said_memory_hz_ = 0;
    AppletHookCookie hook_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread watchdog_;
};

}  // namespace
#endif

int play_here(const LocalPlayRequest& request, bool* quit_chosen) {
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
#ifdef __SWITCH__
    // X on a ROM in the file picker makes it a forwarder on the HOME menu,
    // which starts this program -- see switch_forwarder.hpp.
    const std::unique_ptr<ForwarderPlatform> forwarders =
        make_switch_forwarder_platform(request.self_path);
    options.forwarder_platform = forwarders.get();
#endif
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
    // Closing an application from the HOME menu ends its process there and
    // then, unless it has asked to be told first: without this, closing the
    // game that way lost everything since the last resume point. With it the
    // system asks, the app leaves as it would for Quit -- the resume point and
    // the cartridge save written on the way out -- and it has 15 seconds to.
    const bool exit_locked = R_SUCCEEDED(appletLockExit());
#endif
    App app;
    const int status = app.run(options);
    if (quit_chosen != nullptr) *quit_chosen = app.quit_chosen();
    log_info("finished playing here (status " + std::to_string(status) + ")" +
             (app.quit_chosen() ? "; the player quit" : ""));
#ifdef __SWITCH__
    g_spare_cores.store(0);
    // Last: if the system asked to close the app, letting go is its cue to end
    // the process there and then, and anything after this may never run --
    // including the log's writer, so what it holds goes out first.
    flush_log_file();
    if (exit_locked) appletUnlockExit();
#endif
    return status;
}

void leave_to_home() {
#ifdef __SWITCH__
    // A program started by hbloader goes back to it when main() returns, and
    // it starts the homebrew menu again. Quit means the whole thing closes,
    // as quitting a game does, so this program's exit ends the application
    // (or the album applet) instead. Read by libnx as main() returns.
    __nx_applet_exit_mode = 1;
    log_info("closing to the HOME menu");
#endif
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

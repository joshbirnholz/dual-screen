#include "util/child_process.hpp"

#include "util/log.hpp"

#if defined(__linux__)

#include <errno.h>
#include <signal.h>
#include <cstring>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <vector>

namespace ds {
namespace {

// Long enough for a program to close its own windows and save what it has,
// short enough that quitting does not feel like a hang.
constexpr int kGracefulStopMilliseconds = 3000;

bool reap(int pid, bool& out_still_running) {
    int status = 0;
    const pid_t got = ::waitpid(pid, &status, WNOHANG);
    if (got == 0) {
        out_still_running = true;
        return true;
    }
    out_still_running = false;
    return got == pid;
}

}  // namespace

ChildProcess::~ChildProcess() { stop(); }

bool ChildProcess::start(const std::string& program, const std::vector<std::string>& arguments,
                         std::string* error) {
    stop();

    // Built before forking. Everything between fork and exec in the child has
    // to be async-signal-safe, and building argv is not.
    std::vector<std::string> owned;
    owned.reserve(arguments.size() + 1);
    owned.push_back(program);
    for (const std::string& argument : arguments) {
        owned.push_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (std::string& entry : owned) {
        argv.push_back(entry.data());
    }
    argv.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        if (error != nullptr) *error = std::string("could not start it: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        // Its own process group, so stopping it stops whatever it started too.
        ::setpgid(0, 0);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    ::setpgid(child, child);  // raced with the child on purpose; whichever wins is the same result
    pid_ = child;
    log_info("started " + program);
    return true;
}

bool ChildProcess::running() {
    if (pid_ < 0) {
        return false;
    }
    bool still = false;
    if (!reap(pid_, still)) {
        pid_ = -1;
        return false;
    }
    if (!still) {
        pid_ = -1;
    }
    return still;
}

void ChildProcess::stop() {
    if (pid_ < 0) {
        return;
    }
    // The group rather than the process: an emulator that launched a helper of
    // its own would otherwise leave it behind.
    ::kill(-pid_, SIGTERM);
    for (int waited = 0; waited < kGracefulStopMilliseconds; waited += 10) {
        bool still = false;
        if (!reap(pid_, still) || !still) {
            pid_ = -1;
            return;
        }
        const timespec pause{0, 10 * 1000 * 1000};
        ::nanosleep(&pause, nullptr);
    }
    log_warn("it would not stop when asked, so it is being ended");
    ::kill(-pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
}

}  // namespace ds

#elif defined(_WIN32)

#include "util/win_process.hpp"

namespace ds {
namespace {

constexpr DWORD kGracefulStopMilliseconds = 3000;

// Asks each of the process's top-level windows to close, as clicking its X
// would: the polite stop, before ending it outright.
BOOL CALLBACK close_windows_of(HWND window, LPARAM process_id) {
    DWORD owner = 0;
    ::GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(process_id) && ::IsWindowVisible(window)) {
        ::PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

}  // namespace

ChildProcess::~ChildProcess() { stop(); }

bool ChildProcess::start(const std::string& program, const std::vector<std::string>& arguments,
                         std::string* error) {
    stop();
    // A job that ends everything in it when its last handle closes -- this
    // program's, even if it crashes -- so an emulator is never left behind.
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    WindowsLaunch launch;
    launch.hidden = false;  // an emulator has a window of its own to show
    launch.working_directory = path_dirname(program);
    HANDLE process = nullptr;
    DWORD id = 0;
    std::string why;
    if (!start_windows_process(program, arguments, launch, process, &id, &why)) {
        if (job != nullptr) ::CloseHandle(job);
        if (error != nullptr) *error = "could not start it: " + why;
        return false;
    }
    if (job != nullptr) ::AssignProcessToJobObject(job, process);
    process_ = process;
    job_ = job;
    pid_ = static_cast<int>(id);
    log_info("started " + program);
    return true;
}

bool ChildProcess::running() {
    if (process_ == nullptr) return false;
    if (::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT) return true;
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
    // Its job stays until stop(): whatever it started may still be running.
    pid_ = -1;
    return false;
}

void ChildProcess::stop() {
    if (process_ != nullptr) {
        HANDLE process = static_cast<HANDLE>(process_);
        if (::WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            ::EnumWindows(close_windows_of, static_cast<LPARAM>(pid_));
            if (::WaitForSingleObject(process, kGracefulStopMilliseconds) == WAIT_TIMEOUT) {
                log_warn("it would not stop when asked, so it is being ended");
                if (job_ != nullptr) {
                    ::TerminateJobObject(static_cast<HANDLE>(job_), 1);
                } else {
                    ::TerminateProcess(process, 1);
                }
                ::WaitForSingleObject(process, 5000);
            }
        }
        ::CloseHandle(process);
        process_ = nullptr;
    }
    if (job_ != nullptr) {
        // Anything it left running goes with the job.
        ::CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
    pid_ = -1;
}

}  // namespace ds

#else  // __linux__

namespace ds {

ChildProcess::~ChildProcess() = default;

bool ChildProcess::start(const std::string&, const std::vector<std::string>&, std::string* error) {
    if (error != nullptr) *error = "this platform has no way to start another program";
    return false;
}

bool ChildProcess::running() { return false; }
void ChildProcess::stop() {}

}  // namespace ds

#endif

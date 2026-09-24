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
                         std::string* error, const std::vector<std::string>& environment) {
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

    // Its environment: this one's, less anything named in `environment`, then
    // those. Built here for the same reason argv is, and handed over by
    // pointing `environ` at it in the child, which is only a store.
    std::vector<std::string> owned_environment;
    for (char** entry = ::environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string existing = *entry;
        const std::string name = existing.substr(0, existing.find('='));
        bool replaced = false;
        for (const std::string& override_entry : environment) {
            if (override_entry.compare(0, name.size() + 1, name + "=") == 0) {
                replaced = true;
                break;
            }
        }
        if (!replaced) owned_environment.push_back(existing);
    }
    for (const std::string& override_entry : environment) {
        owned_environment.push_back(override_entry);
    }
    std::vector<char*> envp;
    envp.reserve(owned_environment.size() + 1);
    for (std::string& entry : owned_environment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        if (error != nullptr) *error = std::string("could not start it: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        // Its own process group, so stopping it stops whatever it started too.
        ::setpgid(0, 0);
        ::environ = envp.data();
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

#else  // __linux__

namespace ds {

ChildProcess::~ChildProcess() = default;

bool ChildProcess::start(const std::string&, const std::vector<std::string>&, std::string* error,
                         const std::vector<std::string>&) {
    if (error != nullptr) *error = "this platform has no way to start another program";
    return false;
}

bool ChildProcess::running() { return false; }
void ChildProcess::stop() {}

}  // namespace ds

#endif

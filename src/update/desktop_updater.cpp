#include "update/desktop_updater.hpp"

#include <atomic>

#include "config/paths.hpp"
#include "util/log.hpp"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#define DS_DESKTOP_UPDATES 1
#else
#define DS_DESKTOP_UPDATES 0
#endif

#if DS_DESKTOP_UPDATES
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#endif

namespace ds::update {
namespace {

std::atomic<bool> g_restart{false};

}  // namespace

void request_restart() { g_restart = true; }
bool restart_requested() { return g_restart.load(); }

#if DS_DESKTOP_UPDATES
namespace {

namespace fs = std::filesystem;

// This program's own file. Once an update has renamed a new one over it, the
// kernel calls the one still running "<path> (deleted)" -- and the path
// without that is the new one, which is the one wanted.
std::string self_path() {
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) return std::string();
    std::string path(buffer, static_cast<std::size_t>(length));
    const std::string deleted = " (deleted)";
    if (path.size() > deleted.size() && path.compare(path.size() - deleted.size(), deleted.size(), deleted) == 0) {
        path.resize(path.size() - deleted.size());
    }
    return path;
}

std::string read_text(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void sleep_briefly() {
    const timespec pause{0, 100 * 1000 * 1000};
    ::nanosleep(&pause, nullptr);
}

// Runs a program -- curl, tar -- with nothing passed through a shell, its
// output into `out_path` and its complaints into `err_path`. `tick` is called
// every tenth of a second while it runs and stops it by returning false.
// Returns its exit status, 127 when it could not be started, or -1 when it
// was stopped.
int run(const std::vector<std::string>& arguments, const std::string& out_path, const std::string& err_path,
        const std::function<bool()>& tick) {
    std::vector<std::string> owned = arguments;
    std::vector<char*> argv;
    for (std::string& argument : owned) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const pid_t child = ::fork();
    if (child < 0) return 127;
    if (child == 0) {
        const int out = ::open(out_path.empty() ? "/dev/null" : out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int err = ::open(err_path.empty() ? "/dev/null" : err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int in = ::open("/dev/null", O_RDONLY);
        if (out >= 0) ::dup2(out, 1);
        if (err >= 0) ::dup2(err, 2);
        if (in >= 0) ::dup2(in, 0);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    for (;;) {
        int status = 0;
        const pid_t done = ::waitpid(child, &status, WNOHANG);
        if (done == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (done < 0) return 1;
        if (tick && !tick()) {
            ::kill(child, SIGTERM);
            ::waitpid(child, &status, 0);
            return -1;
        }
        sleep_briefly();
    }
}

// What curl said went wrong, in words for the player.
std::string curl_error(int status, const std::string& err_path) {
    if (status == 127) return "curl is not installed";
    if (status == 6 || status == 7 || status == 28) return "GitHub could not be reached";
    if (status == 22) {
        const std::string said = read_text(err_path);
        if (said.find("404") != std::string::npos) return "the release is not there";
    }
    std::string said = read_text(err_path);
    const std::size_t end = said.find('\n');
    if (end != std::string::npos) said.resize(end);
    // "curl: (35) OpenSSL ..." -- the reason, without curl's own numbering.
    const std::size_t reason = said.find(") ");
    if (said.rfind("curl: (", 0) == 0 && reason != std::string::npos) said = said.substr(reason + 2);
    return said.empty() ? "curl stopped with status " + std::to_string(status) : said;
}

// The size of what `url` redirects to, or 0. From one byte of it rather than
// its headers alone: a release's file is behind a link signed for GET, which
// answers HEAD with 401.
int64_t remote_size(const std::string& url, const std::string& scratch, const std::atomic<bool>& cancel) {
    const std::string headers = scratch + ".headers";
    const int status = run({"curl", "-sSL", "-r", "0-0", "-D", headers, "-o", "/dev/null", "--connect-timeout", "10",
                            "--max-time", "30", url},
                           "", "", [&] { return !cancel.load(); });
    int64_t size = 0;
    if (status == 0) {
        std::istringstream lines(read_text(headers));
        std::string line;
        // "Content-Range: bytes 0-0/6012345", the last answer's, after every
        // redirect.
        while (std::getline(lines, line)) {
            std::string lower = line;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const std::size_t slash = lower.rfind('/');
            if (lower.rfind("content-range:", 0) == 0 && slash != std::string::npos) {
                size = std::strtoll(line.c_str() + slash + 1, nullptr, 10);
            }
        }
    }
    std::remove(headers.c_str());
    return size;
}

// Copies `from` beside `to` and renames it over it: whatever is running from
// `to` keeps the file it opened, and anything started after gets the new one.
bool put_in_place(const std::string& from, const std::string& to, std::string& error) {
    const std::string temporary = path_join(path_dirname(to), "." + path_basename(to) + ".update");
    std::error_code code;
    fs::copy_file(from, temporary, fs::copy_options::overwrite_existing, code);
    if (code) {
        error = "could not write " + temporary + ": " + code.message();
        return false;
    }
    struct stat source {};
    if (::stat(from.c_str(), &source) == 0) ::chmod(temporary.c_str(), source.st_mode & 07777);
    if (::rename(temporary.c_str(), to.c_str()) != 0) {
        error = "could not replace " + to + ": " + std::strerror(errno);
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

class DesktopBackend : public Backend {
public:
    explicit DesktopBackend(std::string data_dir) : data_dir_(std::move(data_dir)) {}

    bool fetch(const std::string& url, std::string& body, const std::atomic<bool>& cancel,
               std::string& error) override {
        const std::string out = path_join(staging_dir(), "fetched");
        const std::string err = out + ".err";
        if (!ensure_directory(staging_dir())) {
            error = "could not make " + staging_dir();
            return false;
        }
        const int status = run({"curl", "-fsSL", "--connect-timeout", "10", "--max-time", "30", url}, out, err,
                               [&] { return !cancel.load(); });
        if (status != 0) {
            error = curl_error(status, err);
        } else {
            body = read_text(out);
        }
        std::remove(out.c_str());
        std::remove(err.c_str());
        return status == 0;
    }

    bool download(const std::string& url, const std::string& path, const std::atomic<bool>& cancel,
                  const std::function<void(int64_t, int64_t)>& progress, std::string& error) override {
        const int64_t total = remote_size(url, path, cancel);
        const std::string err = path + ".err";
        const int status = run({"curl", "-fsSL", "--connect-timeout", "15", "--retry", "2", "-o", path, url}, "", err,
                               [&] {
                                   if (cancel) return false;
                                   struct stat file {};
                                   progress(::stat(path.c_str(), &file) == 0 ? file.st_size : 0, total);
                                   return true;
                               });
        if (status != 0) error = status < 0 ? "cancelled" : curl_error(status, err);
        std::remove(err.c_str());
        return status == 0;
    }

    std::string staging_dir() override { return path_join(data_dir_, "update"); }

    bool install(const ComponentSpec& component, const ReleaseInfo& release, const std::string& file,
                 std::string& error) override {
        const bool done = component.id == Component::Cemu ? install_cemu(release, file, error)
                                                           : install_dualscreen(file, error);
        std::remove(file.c_str());
        return done;
    }

private:
    bool install_dualscreen(const std::string& file, std::string& error) {
        const std::string target = install_dir();
        if (target.empty() || ::access(target.c_str(), W_OK) != 0) {
            error = "the folder Dualscreen is in (" + target + ") cannot be written to";
            return false;
        }
        const std::string unpacked = path_join(staging_dir(), "unpacked");
        std::error_code code;
        fs::remove_all(unpacked, code);
        if (!ensure_directory(unpacked)) {
            error = "could not make " + unpacked;
            return false;
        }
        const std::string err = path_join(staging_dir(), "tar.err");
        const int status = run({"tar", "-xzf", file, "-C", unpacked}, "", err, {});
        if (status != 0) {
            error = status == 127 ? "tar is not installed" : "the download would not unpack";
            std::remove(err.c_str());
            fs::remove_all(unpacked, code);
            return false;
        }
        std::remove(err.c_str());
        const std::string from = path_join(unpacked, "dualscreen");
        if (!file_exists(path_join(from, "dualscreen"))) {
            error = "the download has no Dualscreen in it";
            fs::remove_all(unpacked, code);
            return false;
        }
        // The program last: a power cut part way through then leaves the old
        // program with whichever libraries made it, rather than a new program
        // with old ones it may not run with.
        std::vector<std::string> names;
        for (const fs::directory_entry& entry : fs::directory_iterator(from, code)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (name != "dualscreen") names.push_back(name);
        }
        names.push_back("dualscreen");
        for (const std::string& name : names) {
            if (!put_in_place(path_join(from, name), path_join(target, name), error)) {
                fs::remove_all(unpacked, code);
                return false;
            }
        }
        log_info("updates: replaced " + std::to_string(names.size()) + " files in " + target);
        fs::remove_all(unpacked, code);
        return true;
    }

    bool install_cemu(const ReleaseInfo& release, const std::string& file, std::string& error) {
        const std::string target = managed_cemu_path(data_dir_);
        if (!ensure_directory(path_dirname(target))) {
            error = "could not make " + path_dirname(target);
            return false;
        }
        ::chmod(file.c_str(), 0755);
        if (!put_in_place(file, target, error)) return false;
        const std::string number = std::to_string(release.build) + "\n";
        std::string ignored;
        write_file_atomically(managed_cemu_build_file(data_dir_), number.data(), number.size(), &ignored);
        return true;
    }

    std::string data_dir_;
};

}  // namespace

bool desktop_updates_supported() { return true; }

std::unique_ptr<Backend> make_desktop_backend(const std::string& data_dir) {
    return std::make_unique<DesktopBackend>(data_dir);
}

std::string install_dir() {
    const std::string self = self_path();
    return self.empty() ? std::string() : path_dirname(self);
}

void restart_if_requested(char** argv) {
    if (!restart_requested()) return;
    const std::string self = self_path();
    if (self.empty()) {
        log_warn("updates: could not find this program to start it again");
        return;
    }
    log_info("updates: starting " + self + " again");
    close_log_file();
    // Nothing this run had open goes on into the next: a socket left open
    // would hold the port the new one wants.
    if (DIR* fds = ::opendir("/proc/self/fd")) {
        const int own = ::dirfd(fds);
        while (const dirent* entry = ::readdir(fds)) {
            const int fd = std::atoi(entry->d_name);
            if (fd > 2 && fd != own) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        ::closedir(fds);
    }
    ::execv(self.c_str(), argv);
    // Still here: say so somewhere, now that the log is closed.
    std::fprintf(stderr, "dualscreen: could not start %s again: %s\n", self.c_str(), std::strerror(errno));
}

#else

bool desktop_updates_supported() { return false; }
std::unique_ptr<Backend> make_desktop_backend(const std::string&) { return nullptr; }
std::string install_dir() { return std::string(); }
void restart_if_requested(char**) {}

#endif

}  // namespace ds::update

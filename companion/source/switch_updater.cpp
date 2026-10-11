#include "switch_updater.hpp"

#include "local_play.hpp"

#if DS_LOCAL_PLAY && defined(__SWITCH__) && DS_HAVE_CURL

#include <curl/curl.h>

#include <cstdio>
#include <mutex>
#include <vector>

#include "config/paths.hpp"
#include "switch_forwarder.hpp"
#include "util/log.hpp"

namespace ds::sw {
namespace {

// version.json is a few dozen bytes; anything near this is not one.
constexpr std::size_t kMaxFetch = 64 * 1024;

std::size_t collect(char* data, std::size_t size, std::size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    const std::size_t bytes = size * count;
    if (body->size() + bytes > kMaxFetch) return 0;
    body->append(data, bytes);
    return bytes;
}

std::size_t write_out(char* data, std::size_t size, std::size_t count, void* user) {
    return std::fwrite(data, size, count, static_cast<FILE*>(user)) * size;
}

struct Progress {
    const std::atomic<bool>* cancel;
    const std::function<void(int64_t, int64_t)>* report;
};

int on_progress(void* user, curl_off_t total, curl_off_t done, curl_off_t, curl_off_t) {
    const auto* progress = static_cast<Progress*>(user);
    if (*progress->cancel) return 1;  // ends the transfer
    (*progress->report)(static_cast<int64_t>(done), static_cast<int64_t>(total));
    return 0;
}

// A handle set up the way every request here wants: redirects followed (a
// release's files are all one), the console's certificates trusted, and an
// error said in words.
CURL* make_handle(const std::string& url, char* error_buffer) {
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return nullptr;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Dualscreen");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    if (curl_needs_ca_file()) {
        const std::string ca = console_ca_file();
        if (!ca.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
    }
    return curl;
}

std::string describe(CURLcode code, const char* buffer) {
    if (code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT || code == CURLE_OPERATION_TIMEDOUT) {
        return "GitHub could not be reached";
    }
    if (code == CURLE_HTTP_RETURNED_ERROR) return "the release is not there";
    return buffer != nullptr && buffer[0] != '\0' ? std::string(buffer) : std::string(curl_easy_strerror(code));
}

class SwitchUpdateBackend : public update::Backend {
public:
    SwitchUpdateBackend(std::string nro_path, std::string staging)
        : nro_path_(std::move(nro_path)), staging_(std::move(staging)) {}

    bool fetch(const std::string& url, std::string& body, const std::atomic<bool>& cancel,
               std::string& error) override {
        char buffer[CURL_ERROR_SIZE] = {};
        CURL* curl = make_handle(url, buffer);
        if (curl == nullptr) {
            error = "curl would not start";
            return false;
        }
        body.clear();
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        const std::function<void(int64_t, int64_t)> ignore = [](int64_t, int64_t) {};
        Progress state{&cancel, &ignore};
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
        const CURLcode code = curl_easy_perform(curl);
        if (code != CURLE_OK) error = describe(code, buffer);
        curl_easy_cleanup(curl);
        return code == CURLE_OK;
    }

    bool download(const std::string& url, const std::string& path, const std::atomic<bool>& cancel,
                  const std::function<void(int64_t, int64_t)>& progress, std::string& error) override {
        FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            error = "could not write " + path;
            return false;
        }
        char buffer[CURL_ERROR_SIZE] = {};
        CURL* curl = make_handle(url, buffer);
        if (curl == nullptr) {
            std::fclose(file);
            error = "curl would not start";
            return false;
        }
        Progress state{&cancel, &progress};
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_out);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
        // A stalled transfer gives up rather than waiting for ever.
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        const CURLcode code = curl_easy_perform(curl);
        const bool written = std::fclose(file) == 0;
        curl_easy_cleanup(curl);
        if (code != CURLE_OK) {
            error = cancel ? "cancelled" : describe(code, buffer);
            return false;
        }
        if (!written) {
            error = "the SD card would not take it";
            return false;
        }
        return true;
    }

    std::string staging_dir() override { return staging_; }

    bool install(const update::ComponentSpec& component, const update::ReleaseInfo&, const std::string& file,
                 std::string& error) override {
        if (component.id != update::Component::Dualscreen) {
            error = "a Switch keeps no " + component.name;
            return false;
        }
        if (nro_path_.empty()) {
            error = "this program does not know where it was started from";
            return false;
        }
        // Anything that is not an NRO -- an error page, a cut-off download --
        // stays where it is rather than over the one that works.
        FILE* check = std::fopen(file.c_str(), "rb");
        char magic[4] = {};
        bool looks_right = false;
        if (check != nullptr) {
            looks_right = std::fseek(check, 0x10, SEEK_SET) == 0 && std::fread(magic, 1, 4, check) == 4 &&
                          magic[0] == 'N' && magic[1] == 'R' && magic[2] == 'O' && magic[3] == '0';
            std::fclose(check);
        }
        if (!looks_right) {
            std::remove(file.c_str());
            error = "the download is not a Switch program";
            return false;
        }
        if (!replace_file(file, nro_path_, &error)) {
            std::remove(file.c_str());
            return false;
        }
        log_info("updates: wrote the new build over " + nro_path_);
        return true;
    }

private:
    std::string nro_path_;
    std::string staging_;
};

}  // namespace

std::unique_ptr<update::Backend> make_switch_update_backend(const std::string& nro_path,
                                                            const std::string& staging_dir) {
    std::string path = nro_path;
    if (!path.empty() && path[0] == '/') path = "sdmc:" + path;
    return std::make_unique<SwitchUpdateBackend>(path, staging_dir);
}

}  // namespace ds::sw

#else

namespace ds::sw {

std::unique_ptr<update::Backend> make_switch_update_backend(const std::string&, const std::string&) {
    return nullptr;
}

}  // namespace ds::sw

#endif

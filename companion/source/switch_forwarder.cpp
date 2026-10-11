#include "switch_forwarder.hpp"

#include "local_play.hpp"

#if DS_LOCAL_PLAY && defined(__SWITCH__)

#include <switch.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <vector>

#if DS_HAVE_CURL
#include <curl/curl.h>
#endif

#include "config/paths.hpp"
#include "forwarder_install.hpp"
#include "util/log.hpp"

namespace ds::sw {
namespace {

// Cut back to at most `bytes` bytes without splitting a character.
std::string clip_utf8(std::string text, std::size_t bytes) {
    if (text.size() <= bytes) return text;
    std::size_t end = bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    text.resize(end);
    return text;
}

#if DS_HAVE_CURL
// The body, up to a limit no picture or answer here comes near.
constexpr std::size_t kMaxBody = 16 * 1024 * 1024;

std::size_t collect(char* data, std::size_t size, std::size_t count, void* user) {
    auto* body = static_cast<std::vector<uint8_t>*>(user);
    const std::size_t bytes = size * count;
    if (body->size() + bytes > kMaxBody) return 0;  // ends the transfer
    body->insert(body->end(), data, data + bytes);
    return bytes;
}

std::string base64(const uint8_t* data, std::size_t size) {
    static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (std::size_t i = 0; i < size; i += 3) {
        const uint32_t chunk = (uint32_t(data[i]) << 16) | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
                               (i + 2 < size ? uint32_t(data[i + 2]) : 0);
        text += digits[(chunk >> 18) & 63];
        text += digits[(chunk >> 12) & 63];
        text += i + 1 < size ? digits[(chunk >> 6) & 63] : '=';
        text += i + 2 < size ? digits[chunk & 63] : '=';
    }
    return text;
}

// The certificate authorities the console itself trusts, as a PEM file curl
// can be pointed at. A libcurl built on mbedTLS knows none of its own, and
// without them every HTTPS request fails -- the answer to that is these, never
// switching verification off. Written once a run; empty when the console
// would not give them.
std::string system_ca_file() {
    static std::once_flag once;
    static std::string path;
    std::call_once(once, [] {
        if (R_FAILED(sslInitialize(1))) {
            log_warn("forwarder: the ssl service would not open, so there are no certificates to trust");
            return;
        }
        u32 ids[1] = {static_cast<u32>(SslCaCertificateId_All)};
        u32 size = 0;
        std::vector<uint8_t> buffer;
        u32 count = 0;
        if (R_SUCCEEDED(sslGetCertificateBufSize(ids, 1, &size)) && size > 0) {
            buffer.resize(size);
            if (R_FAILED(sslGetCertificates(buffer.data(), size, ids, 1, &count))) count = 0;
        }
        sslExit();
        std::string pem;
        const auto* infos = reinterpret_cast<const SslBuiltInCertificateInfo*>(buffer.data());
        for (u32 i = 0; i < count; ++i) {
            if (infos[i].status != SslTrustedCertStatus_EnabledTrusted || infos[i].cert_data == nullptr) continue;
            const std::string body = base64(infos[i].cert_data, infos[i].cert_size);
            pem += "-----BEGIN CERTIFICATE-----\n";
            for (std::size_t at = 0; at < body.size(); at += 64) pem += body.substr(at, 64) + "\n";
            pem += "-----END CERTIFICATE-----\n";
        }
        if (pem.empty()) {
            log_warn("forwarder: the console gave no certificates to trust");
            return;
        }
        const std::string file_path = AppPaths::resolve().config_dir + "/ca-certificates.pem";
        FILE* file = std::fopen(file_path.c_str(), "wb");
        if (file == nullptr) return;
        const bool written = std::fwrite(pem.data(), 1, pem.size(), file) == pem.size();
        std::fclose(file);
        if (written) path = file_path;
    });
    return path;
}

bool needs_ca_file() {
    const curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    const std::string ssl = version != nullptr && version->ssl_version != nullptr ? version->ssl_version : "";
    std::string lower(ssl);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return lower.find("mbedtls") != std::string::npos;
}
#endif

class SwitchForwarderPlatform : public ForwarderPlatform {
public:
    explicit SwitchForwarderPlatform(std::string nro_path) : nro_path_(std::move(nro_path)) {
        // As the homebrew menu names it: hbloader wants the SD card's device.
        if (!nro_path_.empty() && nro_path_[0] == '/') nro_path_ = "sdmc:" + nro_path_;
    }

    bool edit_text(const std::string& header, const std::string& guide, const std::string& initial,
                   std::size_t max_length, std::string& out) override {
        SwkbdConfig keyboard;
        if (R_FAILED(swkbdCreate(&keyboard, 0))) {
            log_warn("forwarder: the keyboard would not open");
            return false;
        }
        swkbdConfigMakePresetDefault(&keyboard);
        swkbdConfigSetHeaderText(&keyboard, header.c_str());
        swkbdConfigSetGuideText(&keyboard, guide.c_str());
        swkbdConfigSetInitialText(&keyboard, initial.c_str());
        swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(std::min<std::size_t>(max_length, 500)));
        std::vector<char> text(max_length * 4 + 1, 0);
        const Result result = swkbdShow(&keyboard, text.data(), text.size());
        swkbdClose(&keyboard);
        if (R_FAILED(result)) return false;  // backed out
        out = clip_utf8(text.data(), max_length);
        return true;
    }

    HttpResponse http_get(const std::string& url, const std::string& bearer) override {
        HttpResponse response;
#if DS_HAVE_CURL
        static std::once_flag init;
        std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            response.error = "curl would not start";
            return response;
        }
        curl_slist* headers = nullptr;
        if (!bearer.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
        headers = curl_slist_append(headers, "Accept: application/json, image/*");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Dualscreen");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        if (needs_ca_file()) {
            const std::string ca = system_ca_file();
            if (!ca.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
        }
        const CURLcode code = curl_easy_perform(curl);
        if (code == CURLE_OK) {
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            response.reached = true;
            response.status = static_cast<int>(status);
        } else {
            response.error = curl_easy_strerror(code);
            response.body.clear();
            log_warn("forwarder: " + url.substr(0, url.find('?')) + ": " + response.error);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
#else
        (void)url;
        (void)bearer;
        response.error = "this build has no libcurl (pacman -S switch-curl)";
#endif
        return response;
    }

    bool install(const ForwarderRequest& request, std::string* error) override {
        forwarder::ForwarderContents contents;
        contents.nro_path = nro_path_;
        contents.arguments = "--game \"" + request.rom_path + "\"";
        contents.name = request.title;
        contents.author = request.publisher;
        contents.icon = request.icon;
        contents.screenshots = request.screenshots;
        contents.video_capture = request.video_capture;
        return forwarder::install_forwarder(std::move(contents), error);
    }

private:
    std::string nro_path_;
};

}  // namespace

std::unique_ptr<ForwarderPlatform> make_switch_forwarder_platform(const std::string& nro_path) {
    return std::make_unique<SwitchForwarderPlatform>(nro_path);
}

bool curl_needs_ca_file() {
#if DS_HAVE_CURL
    return needs_ca_file();
#else
    return false;
#endif
}

std::string console_ca_file() {
#if DS_HAVE_CURL
    return system_ca_file();
#else
    return std::string();
#endif
}

}  // namespace ds::sw

#endif

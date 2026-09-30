#include "forwarder/steamgriddb.hpp"

#include <algorithm>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace ds {
namespace {

constexpr const char* kApi = "https://www.steamgriddb.com/api/v2";

SteamGridError error_for(const HttpResponse& response) {
    if (!response.reached) return SteamGridError::Network;
    if (response.status == 401 || response.status == 403) return SteamGridError::BadKey;
    if (response.status < 200 || response.status >= 300) return SteamGridError::NotFound;
    return SteamGridError::None;
}

nlohmann::json parse(const std::vector<uint8_t>& body) {
    return nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
}

}  // namespace

std::string url_escape(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char escaped[4];
            std::snprintf(escaped, sizeof(escaped), "%%%02X", c);
            out += escaped;
        }
    }
    return out;
}

bool steamgrid_first_game(const std::vector<uint8_t>& json, int64_t& id, std::string& name) {
    const nlohmann::json document = parse(json);
    if (!document.is_object()) return false;
    const auto data = document.find("data");
    if (data == document.end() || !data->is_array() || data->empty()) return false;
    const nlohmann::json& game = data->front();
    if (!game.is_object() || !game.contains("id") || !game["id"].is_number_integer() ||
        !game.contains("name") || !game["name"].is_string()) {
        return false;
    }
    id = game["id"].get<int64_t>();
    name = game["name"].get<std::string>();
    return id > 0 && !name.empty();
}

void steamgrid_append_urls(const std::vector<uint8_t>& json, std::vector<std::string>& urls) {
    const nlohmann::json document = parse(json);
    if (!document.is_object()) return;
    const auto data = document.find("data");
    if (data == document.end() || !data->is_array()) return;
    for (const nlohmann::json& entry : *data) {
        if (!entry.is_object() || !entry.contains("url") || !entry["url"].is_string()) continue;
        const std::string url = entry["url"].get<std::string>();
        if (!url.empty() && std::find(urls.begin(), urls.end(), url) == urls.end()) {
            urls.push_back(url);
        }
    }
}

SteamGridSearch steamgrid_search(const HttpGet& get, const std::string& api_key,
                                 const std::string& title) {
    SteamGridSearch search;
    const HttpResponse found =
        get(std::string(kApi) + "/search/autocomplete/" + url_escape(title), api_key);
    search.error = error_for(found);
    if (search.error != SteamGridError::None) return search;

    int64_t game_id = 0;
    if (!steamgrid_first_game(found.body, game_id, search.match_name)) {
        search.error = SteamGridError::NotFound;
        return search;
    }

    // Square grids first -- a whole picture of the game, made to be seen at
    // this shape -- then its icons.
    const std::string id = std::to_string(game_id);
    const std::string lists[] = {
        std::string(kApi) + "/grids/game/" + id +
            "?dimensions=1024x1024,512x512&types=static&mimes=image/png,image/jpeg",
        std::string(kApi) + "/icons/game/" + id + "?types=static&mimes=image/png,image/jpeg",
    };
    for (const std::string& url : lists) {
        const HttpResponse listed = get(url, api_key);
        const SteamGridError error = error_for(listed);
        if (error == SteamGridError::BadKey || error == SteamGridError::Network) {
            search.error = error;
            return search;
        }
        if (error == SteamGridError::None) steamgrid_append_urls(listed.body, search.urls);
    }
    if (search.urls.empty()) search.error = SteamGridError::NotFound;
    return search;
}

std::string describe(SteamGridError error) {
    switch (error) {
        case SteamGridError::BadKey: return "SteamGridDB did not accept the API key.";
        case SteamGridError::Network: return "SteamGridDB could not be reached.";
        case SteamGridError::NotFound: return "SteamGridDB has no pictures of this game.";
        case SteamGridError::None: break;
    }
    return std::string();
}

}  // namespace ds

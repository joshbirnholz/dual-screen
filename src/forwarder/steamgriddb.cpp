#include "forwarder/steamgriddb.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "util/log.hpp"

namespace ds {
namespace {

constexpr const char* kApi = "https://www.steamgriddb.com/api/v2";
// How many of the games a name matches are looked through for pictures
// before the player is asked which one it is.
constexpr std::size_t kGamesTried = 3;
constexpr std::size_t kGamesKept = 8;

SteamGridError error_for(const HttpResponse& response) {
    if (!response.reached) return SteamGridError::Network;
    if (response.status == 401 || response.status == 403) return SteamGridError::BadKey;
    if (response.status < 200 || response.status >= 300) return SteamGridError::NotFound;
    return SteamGridError::None;
}

nlohmann::json parse(const std::vector<uint8_t>& body) {
    return nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
}

std::string trim_spaces(const std::string& text) {
    std::string out;
    for (const char c : text) {
        const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (space) {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += c;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void erase_all(std::string& text, const std::string& what) {
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at)) {
        text.erase(at, what.size());
    }
}

// The year a Unix time falls in (Howard Hinnant's days-to-civil).
int year_of(int64_t seconds) {
    int64_t days = seconds / 86400;
    if (seconds % 86400 < 0) --days;
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t day_of_era = days - era * 146097;
    const int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t month_index = (5 * day_of_year + 2) / 153;
    const int64_t month = month_index < 10 ? month_index + 3 : month_index - 9;
    return static_cast<int>(year_of_era + era * 400 + (month <= 2 ? 1 : 0));
}

void add_term(std::vector<std::string>& terms, const std::string& term) {
    if (!term.empty() && std::find(terms.begin(), terms.end(), term) == terms.end()) terms.push_back(term);
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

std::string steamgrid_clean_title(const std::string& title) {
    std::string text = title;
    for (const char* mark : {"\xE2\x84\xA2", "\xC2\xAE", "\xC2\xA9"}) erase_all(text, mark);  // ™ ® ©
    std::string out;
    int depth = 0;
    for (const char c : text) {
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
            out += ' ';
        } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
            --depth;
        } else if (depth == 0) {
            out += c == '_' ? ' ' : c;
        }
    }
    out = trim_spaces(out);
    return out.empty() ? trim_spaces(title) : out;
}

std::vector<std::string> steamgrid_search_terms(const std::string& title) {
    std::vector<std::string> terms;
    const std::string clean = steamgrid_clean_title(title);
    add_term(terms, clean);

    // Without its punctuation: "Mario & Luigi: Bowser's Inside Story" as
    // "Mario Luigi Bowsers Inside Story".
    std::string plain;
    for (const char c : clean) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || byte >= 0x80) {
            plain += c;
        } else if (c == ' ' || c == '-' || c == ':' || c == '&' || c == '/' || c == '.' || c == ',') {
            plain += ' ';
        }
    }
    add_term(terms, trim_spaces(plain));

    // Without the "DS" so many DS games end their names with.
    for (const std::string& base : std::vector<std::string>(terms)) {
        if (base.size() > 3) {
            std::string tail = base.substr(base.size() - 3);
            std::transform(tail.begin(), tail.end(), tail.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (tail == " DS") add_term(terms, trim_spaces(base.substr(0, base.size() - 3)));
        }
    }

    // What comes before its subtitle, last: the loosest.
    std::size_t cut = std::string::npos;
    for (const char* separator : {":", " - ", " \xE2\x80\x93 ", " \xE2\x80\x94 "}) {
        cut = std::min(cut, clean.find(separator));
    }
    if (cut != std::string::npos && cut > 0) add_term(terms, trim_spaces(clean.substr(0, cut)));
    return terms;
}

std::vector<SteamGridGame> steamgrid_games(const std::vector<uint8_t>& json) {
    std::vector<SteamGridGame> games;
    const nlohmann::json document = parse(json);
    if (!document.is_object()) return games;
    const auto data = document.find("data");
    if (data == document.end() || !data->is_array()) return games;
    for (const nlohmann::json& entry : *data) {
        if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_number_integer() ||
            !entry.contains("name") || !entry["name"].is_string()) {
            continue;
        }
        SteamGridGame game;
        game.id = entry["id"].get<int64_t>();
        game.name = entry["name"].get<std::string>();
        if (entry.contains("release_date") && entry["release_date"].is_number_integer()) {
            game.year = year_of(entry["release_date"].get<int64_t>());
        }
        if (game.id > 0 && !game.name.empty()) games.push_back(std::move(game));
    }
    return games;
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

SteamGridSearch steamgrid_pictures(const HttpGet& get, const std::string& api_key,
                                   const SteamGridGame& game) {
    SteamGridSearch search;
    search.match = game;
    // Square grids first -- a whole picture of the game, made to be seen at
    // this shape -- then its icons, then the rest of its grids. Only in what
    // a picture here can be read from: PNG and JPEG, and ICO for icons.
    const std::string id = std::to_string(game.id);
    const std::string lists[] = {
        std::string(kApi) + "/grids/game/" + id +
            "?dimensions=512x512,1024x1024&types=static&mimes=image/png,image/jpeg",
        std::string(kApi) + "/icons/game/" + id + "?types=static&mimes=image/png,image/vnd.microsoft.icon",
        std::string(kApi) + "/grids/game/" + id +
            "?dimensions=600x900,342x482,660x930,460x215,920x430&types=static&mimes=image/png,image/jpeg",
    };
    for (const std::string& url : lists) {
        const HttpResponse listed = get(url, api_key);
        const SteamGridError error = error_for(listed);
        if (error == SteamGridError::BadKey || error == SteamGridError::Network) {
            search.error = error;
            return search;
        }
        if (error == SteamGridError::None) {
            steamgrid_append_urls(listed.body, search.urls);
        } else {
            log_info("forwarder: SteamGridDB answered " + std::to_string(listed.status) + " for " +
                     url.substr(std::string(kApi).size()));
        }
    }
    if (search.urls.empty()) search.error = SteamGridError::NotFound;
    return search;
}

SteamGridSearch steamgrid_search(const HttpGet& get, const std::string& api_key,
                                 const std::string& title) {
    SteamGridSearch search;
    for (const std::string& term : steamgrid_search_terms(title)) {
        search.term = term;
        // Escaped twice, as SteamGridDB's plugin does: the site decodes the
        // name once more itself, so a '/' in it survives the path.
        const HttpResponse found =
            get(std::string(kApi) + "/search/autocomplete/" + url_escape(url_escape(term)), api_key);
        search.error = error_for(found);
        if (search.error == SteamGridError::BadKey || search.error == SteamGridError::Network) return search;
        if (search.error == SteamGridError::None) search.games = steamgrid_games(found.body);
        if (!search.games.empty()) break;
        log_info("forwarder: SteamGridDB knows no game called \"" + term + "\"");
    }
    if (search.games.empty()) {
        search.error = SteamGridError::NotFound;
        return search;
    }
    if (search.games.size() > kGamesKept) search.games.resize(kGamesKept);

    for (std::size_t index = 0; index < std::min(kGamesTried, search.games.size()); ++index) {
        SteamGridSearch pictures = steamgrid_pictures(get, api_key, search.games[index]);
        search.error = pictures.error;
        search.match = pictures.match;
        search.urls = std::move(pictures.urls);
        if (search.error != SteamGridError::NotFound) break;
        log_info("forwarder: SteamGridDB has no pictures of " + search.games[index].name);
    }
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

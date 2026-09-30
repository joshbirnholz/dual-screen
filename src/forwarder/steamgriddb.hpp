#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ds {

// An HTTP GET's answer: whether anything came back at all, its status, and
// its body.
struct HttpResponse {
    bool reached = false;
    int status = 0;
    std::vector<uint8_t> body;
    std::string error;
};

// Fetches `url`, with `bearer` as an Authorization: Bearer token when it is
// not empty. Given by the platform -- curl on a Switch -- so the searching
// can be tested anywhere.
using HttpGet = std::function<HttpResponse(const std::string& url, const std::string& bearer)>;

// SteamGridDB (steamgriddb.com): pictures of games people have made, found
// by the game's name. A search takes an API key, which anyone can have for
// free from the site's preferences page, and asks the site which game the
// name is, then for that game's square grids and its icons, the two kinds of
// picture a HOME menu icon can be made from.
enum class SteamGridError { None, BadKey, Network, NotFound };

struct SteamGridSearch {
    SteamGridError error = SteamGridError::None;
    // What SteamGridDB calls the game it matched.
    std::string match_name;
    // Every picture it has for the game, square grids first, each once.
    std::vector<std::string> urls;
};

SteamGridSearch steamgrid_search(const HttpGet& get, const std::string& api_key,
                                 const std::string& title);

// Why a search came to nothing, in words for the player.
std::string describe(SteamGridError error);

// For tests: the pieces of a search.
std::string url_escape(const std::string& text);
bool steamgrid_first_game(const std::vector<uint8_t>& json, int64_t& id, std::string& name);
void steamgrid_append_urls(const std::vector<uint8_t>& json, std::vector<std::string>& urls);

}  // namespace ds

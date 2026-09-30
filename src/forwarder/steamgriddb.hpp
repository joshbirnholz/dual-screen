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
// free from the site's preferences page, and asks the site which games the
// name could be, then for a game's square grids, its icons, and then its
// other grids, which make a square icon laid over a blur of themselves.
//
// Searched the way SteamGridDB's own Steam Deck plugin searches: the site's
// autocomplete, which is forgiving about spelling and punctuation, with the
// name escaped twice so a '/' or '&' in it arrives as itself; and only for
// the kinds of picture the site has -- icons are PNG or ICO, never JPEG.
enum class SteamGridError { None, BadKey, Network, NotFound };

struct SteamGridGame {
    int64_t id = 0;
    std::string name;
    int year = 0;  // 0 when the site does not know
};

struct SteamGridSearch {
    SteamGridError error = SteamGridError::None;
    // What was asked for last: the name as it was searched for.
    std::string term;
    // The games it could be, best first. Empty when the site knew none.
    std::vector<SteamGridGame> games;
    // The one whose pictures these are.
    SteamGridGame match;
    // Every picture it has for that game, square grids first, each once.
    std::vector<std::string> urls;
};

// Searches for `title`: tidied (see steamgrid_search_terms), trying simpler
// forms of it until the site knows one, and then the first few games it
// matches until one has pictures. NotFound with `games` empty means no game
// was found; with `games` filled, that none of them had pictures.
SteamGridSearch steamgrid_search(const HttpGet& get, const std::string& api_key,
                                 const std::string& title);
// The pictures of one game, chosen from a search's `games`.
SteamGridSearch steamgrid_pictures(const HttpGet& get, const std::string& api_key,
                                   const SteamGridGame& game);

// Why a search came to nothing, in words for the player.
std::string describe(SteamGridError error);

// For tests: the pieces of a search.
std::string url_escape(const std::string& text);
// A name as it is worth searching for: no trademark signs, nothing in
// brackets -- (USA), [!], (Rev 1) -- no underscores for spaces.
std::string steamgrid_clean_title(const std::string& title);
// What to search for, in order: the tidied name, then simpler forms of it --
// what comes before a subtitle, without a trailing "DS", without punctuation.
std::vector<std::string> steamgrid_search_terms(const std::string& title);
std::vector<SteamGridGame> steamgrid_games(const std::vector<uint8_t>& json);
void steamgrid_append_urls(const std::vector<uint8_t>& json, std::vector<std::string>& urls);

}  // namespace ds

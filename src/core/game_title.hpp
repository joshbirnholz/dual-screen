#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ds {

// The name a console would show for a game, read out of the file the player
// actually loaded.
//
// A file name is not a title. "Lunch", "cking" and "The" are what three real
// sessions reported, because a shortened or oddly-punctuated file name is what
// people actually have on disk, and the shell was showing path_stem(). Every
// one of these formats carries the real name somewhere inside it -- it is how
// Cemu and Azahar fill in their own game lists -- so it is a question of
// reading it rather than of not having it.
//
// Each of these takes bytes and returns a title or an empty string, with no
// file handling in it, because the parsing is the part that can be wrong and
// the part worth testing: an offset a field out reads as plausible nonsense
// rather than as a failure.

// A Nintendo DS ROM, from its banner.
//
// The banner carries the localised name the DS menu shows, in UTF-16, as up to
// three lines: the title, sometimes a second line of it, and the publisher on
// the last. The publisher is dropped -- "Mario Kart DS" rather than "Mario
// Kart DS Nintendo" -- and any remaining lines are joined with a space, which
// turns "The Legend of Zelda\nPhantom Hourglass\nNintendo" into the name
// somebody would recognise.
//
// Not the twelve-character title in the header: that is ASCII, upper case and
// truncated to fit ("ZELDA PHANTOM"), and is worse than the file name it would
// be replacing.
std::string nds_title_from_rom(const uint8_t* data, std::size_t size);

// A 3DS SMDH blob -- the icon-and-metadata structure every 3DS title carries,
// and where Azahar's own game list gets its names. Takes the English short
// description, which is the name on the HOME menu.
std::string smdh_title(const uint8_t* data, std::size_t size);

// A 3DS file of any of the shapes one comes in -- a .3ds/.cci card image, a
// .cia install package, a bare .cxi/.app executable, or an .smdh on its own --
// found by walking the format rather than by searching for the magic, so that
// a byte sequence appearing inside the game's own data cannot be mistaken for
// it.
//
// Empty for an encrypted card image. The name lives inside the encrypted
// ExeFS, so without the console's keys there is nothing here to read, and
// saying so by returning nothing is the honest answer.
std::string three_ds_title_from_rom(const uint8_t* data, std::size_t size);

// A Wii U meta.xml, which is the file Cemu's own game list reads. Takes
// <longname_en>, whose line breaks are the ones on the Wii U menu's own two
// lines and become spaces here.
std::string wii_u_title_from_meta_xml(const std::string& xml);

// What the app calls: works out which of the above applies from the file
// itself and reads it. Returns empty when the file carries no title, when it
// cannot be read, or when it is encrypted -- the caller falls back to the file
// name, which is what this replaces and remains the answer when there is
// nothing better.
//
// For a Wii U the path is a .rpx, a .wud, or the game's folder, and the title
// is beside it in meta/meta.xml rather than inside it.
std::string game_title_for_file(const std::string& path);

}  // namespace ds

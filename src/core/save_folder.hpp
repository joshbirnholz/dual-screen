#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// A save that is a folder rather than a file -- a 3DS game's save data as
// Azahar keeps it, a Wii U game's as Cemu does -- as one run of bytes, so that
// everything Data Management does with a DS game's .srm (hashing it to tell
// versions apart, keeping it in Earlier Saves, sending it to another device)
// it does the same way with these.
//
// The bytes are the folder's directories and files in path order, each file's
// contents after its name and size, and nothing else -- no times, no owners --
// so the same folder always packs to the same bytes, on any device.

// False, with why, when the folder cannot be read. An empty or missing folder
// packs to an archive with nothing in it.
bool pack_save_folder(const std::string& directory, std::vector<uint8_t>& out, std::string* error);

// Makes `directory` hold exactly what `archive` does: written beside it first,
// then put in its place, so a failure part way leaves the old folder as it
// was. Refuses an archive naming anything outside the folder.
bool unpack_save_folder(const std::vector<uint8_t>& archive, const std::string& directory,
                        std::string* error);

// Whether the folder holds any file at all, however deep: an emulator makes
// the folders for a game's save before the game ever writes one.
bool save_folder_has_files(const std::string& directory);
// The files' total size, and the newest of their modification times.
uint64_t save_folder_size(const std::string& directory);
int64_t save_folder_modified_at(const std::string& directory);

// The folder and everything in it. True when there is nothing left of it.
bool remove_save_folder(const std::string& directory);

}  // namespace ds

#pragma once

#include <string>
#include <vector>

namespace ds::update {

// Reading one file out of a zip -- a libretro core from the buildbot, which
// comes one to a zip -- with zlib, which every build links already, rather
// than an unzip program a Steam Deck may not have.
//
// Stored and deflated entries only, which is what a zip tool makes; a zip64
// archive, or one encrypted, is refused with a reason.

// The names of the files in `zip`, as its central directory lists them.
bool list_zip(const std::string& zip, std::vector<std::string>& names, std::string* error);

// Writes the entry called `name` to `out_path`, checking its CRC.
bool extract_zip_entry(const std::string& zip, const std::string& name, const std::string& out_path,
                       std::string* error);

// Every entry whose name starts with `under` ("" for all of them), unpacked
// into `folder` with that much taken off the front of its name: a release's
// "Dualscreen/" folder, say. Reads the archive once, however many files it
// holds. Refuses an archive that names a file outside `folder`. `written`,
// when given, gets each file's name as unpacked, relative to `folder`.
bool extract_zip_all(const std::string& zip, const std::string& folder, const std::string& under,
                     std::vector<std::string>* written, std::string* error);

}  // namespace ds::update

#pragma once

#include <memory>
#include <string>

#include "update/updater.hpp"

namespace ds::update {

// A PC's or a Steam Deck's half of the updater: Linux on x86-64, which is what
// the SteamOS build CI publishes is for. Nowhere else has one, and there
// make_desktop_backend() gives nothing, so nothing is offered.
//
// It downloads with the system's curl and unpacks with its tar, both of which
// every SteamOS has, rather than linking a TLS library into the app for one
// download a week. Dualscreen is the folder this program is running from, and
// is replaced file by file -- each one renamed over the old, which the program
// already running from it does not notice -- before the app starts itself
// again. Cemu, when Dualscreen keeps it, is one AppImage in the app's data
// folder (managed_cemu_path()).

// Whether this build is one the SteamOS release can replace.
bool desktop_updates_supported();

// The file in Dualscreen's release this platform installs.
inline constexpr const char* kSteamOsAsset = "dualscreen-steamos.tar.gz";

// `data_dir` is AppPaths::data_dir: downloads are staged under it, and Cemu
// kept in it. `cores_dir` is where the DS and 3DS cores go -- see
// core_component(). Null where desktop_updates_supported() is false.
std::unique_ptr<Backend> make_desktop_backend(const std::string& data_dir, const std::string& cores_dir);

// The folder this program runs from, which an update replaces.
std::string install_dir();

// Asks for this program to be started again once it has shut down, and does
// it: called at the very end of main(), it replaces this process with the
// newly installed program, with the same arguments. Returns only when there
// was no restart asked for, or it could not be done.
void request_restart();
bool restart_requested();
void restart_if_requested(char** argv);

}  // namespace ds::update

#pragma once

#include <string>

namespace ds {

// Which build this binary actually is: a short git hash and when it was
// configured, as the build system stamped it in, or an empty string when
// nothing did.
//
// This exists because of a question that kept being asked and could not be
// answered: a rebuilt binary looks exactly like one that was not rebuilt, so
// "I updated it and the behaviour is the same" and "the update never got onto
// the device" produce identical logs. Two devices in a session make it worse,
// since only one of them may have been updated, and the one that was not is
// usually the one being diagnosed. A macOS bundle carried this first, for the
// same reason; it belongs on every platform, and most of all on the Steam Deck,
// which is the device furthest from whoever is reading the log.
std::string build_stamp();

// Logs it as an info line, or says outright that this build does not carry one
// -- which is itself the answer to the same question, since a binary old
// enough to predate this cannot say anything at all.
void log_build_stamp();

}  // namespace ds

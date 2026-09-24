#pragma once

#include "net/companion_session.hpp"
#include "ui/layout_picker.hpp"

namespace ds {

// The quick menu a companion draws, read entirely off what its host has told
// it -- down to which entries it has, so the same cursor position means the
// same thing on both screens.
//
// One function for every companion. There used to be two copies, the desktop
// app's and the Switch client's, and a new entry added to one and not the
// other put the Options list a row out of step on the Switch: the cursor on
// one device was over a different setting from the other's.
LayoutPickerState menu_state_from_host(const net::CompanionSession& session);

}  // namespace ds

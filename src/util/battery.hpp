#pragma once

namespace ds {

// This device's battery, as the quick menu shows it in its corner the way a
// Switch's HOME menu does.
struct BatteryReading {
    // False on a device with no battery at all -- a desktop PC -- and where
    // the system will not say, either of which shows nothing.
    bool present = false;
    int percent = 0;  // 0 to 100
    bool charging = false;
};

// Read at most every few seconds, whatever the caller's rate: SDL reads a
// Linux machine's from files under /sys, and the number moves slowly anyway.
BatteryReading read_battery();

}  // namespace ds

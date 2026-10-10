#include "util/battery.hpp"

#include <SDL.h>

#include <algorithm>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace ds {
namespace {

constexpr Uint32 kReadEveryMs = 5000;

BatteryReading read_now() {
    BatteryReading reading;
    // DUALSCREEN_FAKE_BATTERY=73, or 73c for charging: a reading for a
    // machine with no battery, so the indicator can be looked at -- and
    // screenshotted by a test -- anywhere.
    if (const char* fake = SDL_getenv("DUALSCREEN_FAKE_BATTERY")) {
        reading.present = true;
        reading.percent = std::clamp(SDL_atoi(fake), 0, 100);
        reading.charging = SDL_strchr(fake, 'c') != nullptr;
        return reading;
    }
#ifdef __SWITCH__
    // SDL has no power reading for a Switch; the system's psm service has.
    static bool started = R_SUCCEEDED(psmInitialize());
    if (!started) return reading;
    u32 percent = 0;
    if (R_FAILED(psmGetBatteryChargePercentage(&percent))) return reading;
    PsmChargerType charger = PsmChargerType_Unconnected;
    psmGetChargerType(&charger);
    reading.present = true;
    reading.percent = std::clamp(static_cast<int>(percent), 0, 100);
    reading.charging = charger != PsmChargerType_Unconnected;
#else
    int seconds = -1;
    int percent = -1;
    const SDL_PowerState state = SDL_GetPowerInfo(&seconds, &percent);
    // No battery, or one the system will not give a number for: nothing to
    // show, rather than a guess.
    if (state == SDL_POWERSTATE_NO_BATTERY || state == SDL_POWERSTATE_UNKNOWN || percent < 0) {
        return reading;
    }
    reading.present = true;
    reading.percent = std::clamp(percent, 0, 100);
    reading.charging = state == SDL_POWERSTATE_CHARGING || state == SDL_POWERSTATE_CHARGED;
#endif
    return reading;
}

}  // namespace

BatteryReading read_battery() {
    static BatteryReading last;
    static Uint32 read_at = 0;
    static bool ever = false;
    const Uint32 now = SDL_GetTicks();
    if (!ever || now - read_at >= kReadEveryMs) {
        last = read_now();
        read_at = now;
        ever = true;
    }
    return last;
}

}  // namespace ds

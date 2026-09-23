#pragma once

#ifdef __SWITCH__
#include <switch.h>

namespace ds {

// Which Switch this is, asked of the console once and remembered -- it is
// not going to change while the app is running.
inline SetSysProductModel switch_product_model() {
    static const SetSysProductModel model = [] {
        SetSysProductModel asked = SetSysProductModel_Invalid;
        if (R_SUCCEEDED(setsysInitialize())) {
            if (R_FAILED(setsysGetProductModel(&asked))) {
                asked = SetSysProductModel_Invalid;
            }
            setsysExit();
        }
        return asked;
    }();
    return model;
}

// A Switch Lite: controls built in rather than two Joy-Con, and no rumble
// motor at all.
inline bool switch_is_lite() { return switch_product_model() == SetSysProductModel_Hoag; }

// In the words on its box: "Switch Lite", "Switch OLED" or "Switch".
inline const char* switch_console_name() {
    switch (switch_product_model()) {
        case SetSysProductModel_Hoag: return "Switch Lite";
        case SetSysProductModel_Aula: return "Switch OLED";
        default: return "Switch";
    }
}

}  // namespace ds

#endif

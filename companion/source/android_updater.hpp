#pragma once

// Android's half of the updater (see src/update/updater.hpp). The downloads
// are the activity's -- Java has HTTPS and the native side has nothing to do
// it with -- and installing is the system's: the APK is handed to Android's
// package installer, which asks the player, replaces the app and offers to
// open the new one. See DualscreenActivity's update methods.
//
// It can only replace an app signed with the same key, which is what CI's
// builds are: a build made and signed anywhere else is not offered one.

#include <memory>
#include <string>

#include "update/updater.hpp"

namespace ds::android {

inline constexpr const char* kAndroidAsset = "dualscreen-android.apk";

// Null anywhere but Android.
std::unique_ptr<update::Backend> make_android_update_backend();

}  // namespace ds::android

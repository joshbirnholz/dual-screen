#include "android_updater.hpp"

#ifdef __ANDROID__

#include <jni.h>

#include <SDL.h>

#include <atomic>
#include <mutex>

#include "config/paths.hpp"
#include "util/log.hpp"

namespace ds::android {
namespace {

// The download under way, for DualscreenActivity.updateProgress() to report
// to. One at a time: the updater never starts a second.
struct Progress {
    const std::atomic<bool>* cancel = nullptr;
    const std::function<void(int64_t, int64_t)>* report = nullptr;
};
std::mutex g_progress_mutex;
Progress g_progress;

// Calls one of the activity's methods from whichever thread this is -- SDL
// attaches it to the VM if it is not already. False when the method threw, or
// is not there.
template <typename Call>
bool with_activity(const char* method, const char* signature, Call call) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) return false;
    jclass type = env->GetObjectClass(activity);
    jmethodID id = env->GetMethodID(type, method, signature);
    bool done = false;
    if (id != nullptr) {
        call(env, activity, id);
        done = true;
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        done = false;
    }
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return done;
}

std::string to_string(JNIEnv* env, jstring text) {
    if (text == nullptr) return std::string();
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(text, chars);
    return out;
}

std::string activity_error() {
    std::string error;
    with_activity("updateError", "()Ljava/lang/String;", [&](JNIEnv* env, jobject activity, jmethodID id) {
        auto text = static_cast<jstring>(env->CallObjectMethod(activity, id));
        error = to_string(env, text);
        if (text != nullptr) env->DeleteLocalRef(text);
    });
    return error.empty() ? "it did not say why" : error;
}

class AndroidUpdateBackend : public update::Backend {
public:
    bool fetch(const std::string& url, std::string& body, const std::atomic<bool>&, std::string& error) override {
        bool got = false;
        with_activity("httpGet", "(Ljava/lang/String;)Ljava/lang/String;",
                      [&](JNIEnv* env, jobject activity, jmethodID id) {
                          jstring address = env->NewStringUTF(url.c_str());
                          auto text = static_cast<jstring>(env->CallObjectMethod(activity, id, address));
                          env->DeleteLocalRef(address);
                          if (text != nullptr) {
                              body = to_string(env, text);
                              env->DeleteLocalRef(text);
                              got = true;
                          }
                      });
        if (!got) error = activity_error();
        return got;
    }

    bool download(const std::string& url, const std::string& path, const std::atomic<bool>& cancel,
                  const std::function<void(int64_t, int64_t)>& progress, std::string& error) override {
        {
            std::lock_guard<std::mutex> lock(g_progress_mutex);
            g_progress = Progress{&cancel, &progress};
        }
        bool got = false;
        with_activity("downloadFile", "(Ljava/lang/String;Ljava/lang/String;)Z",
                      [&](JNIEnv* env, jobject activity, jmethodID id) {
                          jstring address = env->NewStringUTF(url.c_str());
                          jstring file = env->NewStringUTF(path.c_str());
                          got = env->CallBooleanMethod(activity, id, address, file) == JNI_TRUE;
                          env->DeleteLocalRef(address);
                          env->DeleteLocalRef(file);
                      });
        {
            std::lock_guard<std::mutex> lock(g_progress_mutex);
            g_progress = Progress{};
        }
        if (!got) error = cancel ? "cancelled" : activity_error();
        return got;
    }

    std::string staging_dir() override {
        const char* internal = SDL_AndroidGetInternalStoragePath();
        return internal != nullptr ? path_join(internal, "update") : std::string();
    }

    bool install(const update::ComponentSpec& component, const update::ReleaseInfo&, const std::string& file,
                 std::string& error) override {
        if (component.id != update::Component::Dualscreen) {
            error = "Android keeps no " + component.name;
            return false;
        }
        bool handed = false;
        with_activity("installPackage", "(Ljava/lang/String;)Z", [&](JNIEnv* env, jobject activity, jmethodID id) {
            jstring path = env->NewStringUTF(file.c_str());
            handed = env->CallBooleanMethod(activity, id, path) == JNI_TRUE;
            env->DeleteLocalRef(path);
        });
        if (!handed) error = activity_error();
        return handed;
    }

    bool restarts_after_install() const override { return false; }
};

}  // namespace

std::unique_ptr<update::Backend> make_android_update_backend() {
    return std::make_unique<AndroidUpdateBackend>();
}

}  // namespace ds::android

// DualscreenActivity.updateProgress(): how far a download has got, and the
// answer to whether it should go on.
extern "C" JNIEXPORT jboolean JNICALL Java_com_dualscreen_companion_DualscreenActivity_updateProgress(
    JNIEnv*, jclass, jlong done, jlong total) {
    std::lock_guard<std::mutex> lock(ds::android::g_progress_mutex);
    const ds::android::Progress& progress = ds::android::g_progress;
    if (progress.cancel == nullptr) return JNI_TRUE;
    if (*progress.cancel) return JNI_FALSE;
    (*progress.report)(static_cast<int64_t>(done), static_cast<int64_t>(total));
    return JNI_TRUE;
}

#else

namespace ds::android {

std::unique_ptr<update::Backend> make_android_update_backend() { return nullptr; }

}  // namespace ds::android

#endif

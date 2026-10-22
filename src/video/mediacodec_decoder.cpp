#include "video/mediacodec_decoder.hpp"

#if defined(__ANDROID__)

#include <SDL_system.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "render/gl_loader.hpp"
#include "util/log.hpp"
#include "video/h264_bitstream.hpp"

namespace ds::video {
namespace {

// MediaCodecInfo.CodecCapabilities' colour formats, by number: the NDK has no
// names for them. These are the ones a decoder hands back as bytes.
constexpr int32_t kColorFormatYuv420Planar = 19;          // I420
constexpr int32_t kColorFormatYuv420PackedPlanar = 20;    // I420 again
constexpr int32_t kColorFormatYuv420SemiPlanar = 21;      // NV12
constexpr int32_t kColorFormatYuv420PackedSemiPlanar = 39;
constexpr int32_t kColorFormatYuv420Flexible = 0x7f420888;
constexpr int32_t kColorFormatTiYuv420PackedSemiPlanar = 0x7f000100;
constexpr int32_t kColorFormatQcomYuv420SemiPlanar = 0x7fa30c00;
// Qualcomm's own NV12, which is most Android handhelds' -- a G Cloud's
// Snapdragon among them. The same bytes as plain NV12, with rows padded to
// 128 and the luma plane padded to a multiple of 32 rows.
constexpr int32_t kColorFormatQcomYuv420SemiPlanar32m = 0x7fa30c04;
// Qualcomm's tiled layout, which is not rows at all. Older chips only; a
// decoder that insists on it is one this app cannot read.
constexpr int32_t kColorFormatQcomTiled = 0x7fa30c03;

// MediaCodec.BUFFER_FLAG_CODEC_CONFIG.
constexpr uint32_t kBufferFlagCodecConfig = 2;

// MediaCodec runs on a thread of its own, and decode() never calls it.
//
// Called from the loop that draws, a decoder costs that loop whatever it
// waits on -- and a hardware decoder is waited on all the time: for a free
// input slot, for the picture it is still working on, for the one it holds
// until the next frame is fed in. On a G Cloud that was 100 to 200ms of every
// pass, which the session then took for this device being too slow to decode
// and answered by asking the host for smaller and smaller pictures. The
// worker waits on all of that instead, and decode() only hands it a frame and
// takes whatever picture is newest.
//
// How long decode() may still wait for the picture of the frame it has just
// handed over. A hardware decoder takes a few milliseconds over a frame, and
// showing it on this pass rather than the next is a frame period of latency
// saved. But some decoders keep each picture until the next frame is fed in,
// however long they are waited for, and waiting on those costs this every
// frame for nothing -- so the wait stops once it has been seen to be useless.
constexpr int64_t kPictureWaitUs = 12000;
// Once it is not: how long decode() may wait for the picture of the frame
// before this one instead. A decoder that takes a little over a frame period
// over each frame -- a G Cloud's, at 1080p -- hands one picture back on some
// calls and none or two on others, and every two is one never seen: an uneven
// picture, at whatever rate it averages out to. Asking each call for the one
// before makes it one picture a call, in order, a frame behind. That frame was
// being paid already, unevenly; this pays it evenly.
constexpr int64_t kPreviousPictureWaitUs = 20000;
// Ten in a row, not a few: a rebuilt codec takes several frames over its first
// picture, and that is not this.
constexpr int kMissedWaitsBeforeGivingUp = 10;
// How long the worker blocks on one call for a free input slot or a finished
// picture, before looking for new frames again.
constexpr int64_t kInputWaitUs = 5000;
constexpr int64_t kOutputWaitUs = 2000;
// How long a frame may go without the decoder taking it in, and how many may
// queue up for the worker, before the decoder is called wedged. Either is
// answered the way a frame that will not decode is: the session reopens the
// decoder and asks the host for a keyframe.
constexpr int64_t kInputGiveUpUs = 250000;
constexpr std::size_t kMostFramesQueued = 30;

int64_t micros_since(std::chrono::steady_clock::time_point began) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - began)
        .count();
}

int32_t format_int(AMediaFormat* format, const char* key, int32_t fallback) {
    int32_t value = 0;
    return AMediaFormat_getInt32(format, key, &value) ? value : fallback;
}

int32_t align_up(int32_t value, int32_t to) { return (value + to - 1) / to * to; }

std::string hex(int32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%x", static_cast<unsigned>(value));
    return buffer;
}

// AMediaCodec_getName arrived in Android 9 (API 28), above this app's floor,
// so it is looked up rather than linked: a device that has it says which
// decoder it chose, and one that does not just says less.
std::string codec_name_of(AMediaCodec* codec) {
    using GetName = media_status_t (*)(AMediaCodec*, char**);
    using ReleaseName = void (*)(AMediaCodec*, char*);
    static void* library = dlopen("libmediandk.so", RTLD_NOW);
    if (library == nullptr) return "";
    static auto get_name = reinterpret_cast<GetName>(dlsym(library, "AMediaCodec_getName"));
    static auto release_name =
        reinterpret_cast<ReleaseName>(dlsym(library, "AMediaCodec_releaseName"));
    if (get_name == nullptr) return "";
    char* name = nullptr;
    if (get_name(codec, &name) != AMEDIA_OK || name == nullptr) return "";
    std::string copy = name;
    if (release_name != nullptr) release_name(codec, name);
    return copy;
}

// How a decoded picture is laid out in an output buffer, read off the
// decoder's output format whenever it changes.
struct OutputLayout {
    bool known = false;
    int32_t color_format = 0;
    int32_t stride = 0;
    int32_t slice_height = 0;
    int32_t crop_left = 0;
    int32_t crop_top = 0;
    int32_t width = 0;   // what is shown, after cropping
    int32_t height = 0;
    bool planar = false;  // I420 rather than NV12
};

OutputLayout read_layout(AMediaFormat* format, int fallback_width, int fallback_height) {
    OutputLayout layout;
    const int32_t width = format_int(format, "width", fallback_width);
    const int32_t height = format_int(format, "height", fallback_height);
    layout.color_format = format_int(format, "color-format", kColorFormatYuv420SemiPlanar);

    // The crop rectangle is what is actually picture. A 1280x720 stream is
    // coded 1280x736 and many decoders report that as their width and height,
    // with the real size only here.
    const int32_t crop_left = format_int(format, "crop-left", 0);
    const int32_t crop_top = format_int(format, "crop-top", 0);
    const int32_t crop_right = format_int(format, "crop-right", width - 1);
    const int32_t crop_bottom = format_int(format, "crop-bottom", height - 1);
    layout.crop_left = std::max(0, crop_left) & ~1;
    layout.crop_top = std::max(0, crop_top) & ~1;
    layout.width = crop_right - crop_left + 1;
    layout.height = crop_bottom - crop_top + 1;
    if (layout.width <= 0 || layout.height <= 0) {
        layout.crop_left = layout.crop_top = 0;
        layout.width = width;
        layout.height = height;
    }

    layout.stride = format_int(format, "stride", 0);
    layout.slice_height = format_int(format, "slice-height", 0);
    if (layout.color_format == kColorFormatQcomYuv420SemiPlanar32m) {
        // The padding its name promises, for a decoder that does not say.
        if (layout.stride <= 0) layout.stride = align_up(width, 128);
        if (layout.slice_height <= 0) layout.slice_height = align_up(height, 32);
    }
    if (layout.stride < width) layout.stride = width;
    if (layout.slice_height < height) layout.slice_height = height;

    switch (layout.color_format) {
        case kColorFormatYuv420Planar:
        case kColorFormatYuv420PackedPlanar:
            layout.planar = true;
            layout.known = true;
            break;
        case kColorFormatYuv420SemiPlanar:
        case kColorFormatYuv420PackedSemiPlanar:
        case kColorFormatTiYuv420PackedSemiPlanar:
        case kColorFormatQcomYuv420SemiPlanar:
        case kColorFormatQcomYuv420SemiPlanar32m:
            layout.known = true;
            break;
        case kColorFormatQcomTiled:
            layout.known = false;
            break;
        case kColorFormatYuv420Flexible:
        default:
            // Flexible, or a vendor's own number. In a byte buffer, rather
            // than an Image, those are NV12 on every device that has been
            // looked at; read as that, and say so once (see decode()).
            layout.known = true;
            break;
    }
    return layout;
}

// --- Decoding into a Surface ------------------------------------------------
//
// Why: a decoder writing into a byte buffer has to turn its picture from the
// layout its hardware works in into plain NV12 rows in ordinary memory, and on
// a Snapdragon that is most of what a frame costs. On a G Cloud a 1920x1072
// frame spent 27ms inside MediaCodec that way, and 16ms at 1280x704 -- the
// time followed the pixel count, not the decoding. Into a Surface the picture
// stays where the hardware put it, and the GPU reads it from there: what
// Moonlight does, and why it decodes the same stream on the same device in a
// few milliseconds.
//
// The Surface is a SurfaceTexture's, made on the renderer's thread with an
// external texture of its own. That texture can only be sampled through
// samplerExternalOES, which no other shader in this app knows about -- so
// each picture is drawn once from it into the screen's ordinary RGBA texture
// (SurfaceOutput::draw_into), and everything that draws screens carries on
// as it was.

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif

// The picture as the decoder left it, upright: SurfaceTexture's matrix takes
// care of the crop and of which way up the buffer is. Rows are flipped here
// so that the texture drawn into has the picture's top row first, as every
// texture uploaded from memory does.
constexpr const char* kSurfaceVertexShader =
    "attribute vec2 a_position;\n"
    "uniform vec4 u_row0;\n"
    "uniform vec4 u_row1;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec3 uv = vec3(a_position.x * 0.5 + 0.5, 0.5 - a_position.y * 0.5, 1.0);\n"
    "    v_uv = vec2(dot(u_row0.xyz, uv), dot(u_row1.xyz, uv));\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

constexpr const char* kSurfaceFragmentShader =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES u_picture;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_picture, v_uv);\n"
    "}\n";

GLuint compile_shader(GLenum kind, const char* source) {
    const GLuint shader = gl::CreateShader(kind);
    if (shader == 0) return 0;
    gl::ShaderSource(shader, 1, &source, nullptr);
    gl::CompileShader(shader);
    GLint ok = 0;
    gl::GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char text[512] = {};
        gl::GetShaderInfoLog(shader, sizeof(text), nullptr, text);
        log_warn(std::string("video decoder: the Surface's shader would not compile: ") + text);
        gl::DeleteShader(shader);
        return 0;
    }
    return shader;
}

bool jni_failed(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

class SurfaceOutput {
public:
    // Made on the renderer's thread, with its context current. Null, and the
    // decoder writes bytes instead, wherever any part of this is missing.
    static std::shared_ptr<SurfaceOutput> create() {
        if (!native_frames_usable() || gl::GenTextures == nullptr ||
            gl::GenFramebuffers == nullptr || gl::GetString == nullptr) {
            return nullptr;
        }
        const char* extensions = reinterpret_cast<const char*>(gl::GetString(GL_EXTENSIONS));
        if (extensions == nullptr ||
            std::strstr(extensions, "GL_OES_EGL_image_external") == nullptr) {
            static bool said = false;
            if (!said) {
                said = true;
                log_info("video decoder: this GPU cannot sample a Surface, so pictures are "
                         "copied out of the decoder instead");
            }
            return nullptr;
        }
        auto output = std::shared_ptr<SurfaceOutput>(new SurfaceOutput());
        if (!output->build()) {
            return nullptr;
        }
        return output;
    }

    ~SurfaceOutput() {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (window_ != nullptr) ANativeWindow_release(window_);
        if (env != nullptr) {
            if (surface_ != nullptr) {
                jclass type = env->GetObjectClass(surface_);
                jmethodID release = env->GetMethodID(type, "release", "()V");
                if (release != nullptr) env->CallVoidMethod(surface_, release);
                jni_failed(env);
                env->DeleteLocalRef(type);
                env->DeleteGlobalRef(surface_);
            }
            if (texture_ != nullptr) {
                if (release_ != nullptr) env->CallVoidMethod(texture_, release_);
                jni_failed(env);
                env->DeleteGlobalRef(texture_);
            }
        }
        if (gl::DeleteTextures != nullptr && external_ != 0) gl::DeleteTextures(1, &external_);
        if (gl::DeleteFramebuffers != nullptr && framebuffer_ != 0) {
            gl::DeleteFramebuffers(1, &framebuffer_);
        }
        if (gl::DeleteBuffers != nullptr && vertices_ != 0) gl::DeleteBuffers(1, &vertices_);
        if (gl::DeleteProgram != nullptr && program_ != 0) gl::DeleteProgram(program_);
    }

    ANativeWindow* window() const { return window_; }

    // Takes the newest picture the decoder has put on the Surface into the
    // external texture, waiting up to `budget_us` for the one stamped `pts`
    // (or later) if it is not there yet: a picture released to a Surface
    // reaches it a moment later, not at once. Returns the newest one latched
    // so far, or -1 when nothing has been.
    int64_t latch(int64_t pts, int64_t budget_us) {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (env == nullptr || broken_) return latched_pts_;
        const auto began = std::chrono::steady_clock::now();
        int64_t previous = latched_pts_;
        // Each call takes the next picture queued, so this goes on until it
        // stops changing: a backlog is taken in one go, newest last.
        for (int round = 0; round < 64; ++round) {
            env->CallVoidMethod(texture_, update_);
            if (jni_failed(env)) {
                broken_ = true;
                log_warn("video decoder: the Surface could not be read from");
                return latched_pts_;
            }
            const jlong stamp = env->CallLongMethod(texture_, timestamp_);
            // A Surface that has never had a picture says 0.
            if (stamp > 0) latched_pts_ = static_cast<int64_t>(stamp / 1000);
            if (latched_pts_ != previous) {
                previous = latched_pts_;
                continue;
            }
            if (latched_pts_ >= pts || micros_since(began) >= budget_us) break;
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        return latched_pts_;
    }

    bool broken() const { return broken_; }

    // Whether a picture has reached the Surface yet. A broken one says yes,
    // so the draw that follows fails and is counted as failing.
    bool has_picture() {
        if (broken_) return true;
        latch(latched_pts_, 0);
        return broken_ || latched_pts_ >= 0;
    }

    // Draws what is latched into an ordinary RGBA texture of `width` x
    // `height`, leaving the GL state as it found it.
    bool draw_into(GLuint target, int width, int height) {
        if (broken_ || target == 0) return false;
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (env == nullptr) return false;
        // The newest picture there is, which may be newer than the one the
        // session was told about: nothing is lost by showing it now.
        latch(latched_pts_, 0);
        if (broken_ || latched_pts_ < 0) return false;
        jfloatArray matrix = env->NewFloatArray(16);
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (matrix != nullptr) {
            env->CallVoidMethod(texture_, transform_, matrix);
            if (!jni_failed(env)) env->GetFloatArrayRegion(matrix, 0, 16, m);
            env->DeleteLocalRef(matrix);
        }

        GLint framebuffer = 0, program = 0, array_buffer = 0, active = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        gl::GetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        gl::GetIntegerv(GL_CURRENT_PROGRAM, &program);
        gl::GetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        gl::GetIntegerv(GL_ACTIVE_TEXTURE, &active);
        gl::GetIntegerv(GL_VIEWPORT, viewport);
        const bool blend = gl::IsEnabled(GL_BLEND);
        const bool scissor = gl::IsEnabled(GL_SCISSOR_TEST);

        gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        const bool complete =
            gl::CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (complete) {
            gl::Viewport(0, 0, width, height);
            gl::Disable(GL_BLEND);
            gl::Disable(GL_SCISSOR_TEST);
            gl::UseProgram(program_);
            gl::ActiveTexture(GL_TEXTURE0);
            gl::BindTexture(GL_TEXTURE_EXTERNAL_OES, external_);
            gl::Uniform1i(picture_at_, 0);
            // Column-major, as Android hands it over; only the parts that act
            // on (s, t, 1) matter.
            gl::Uniform4f(row0_at_, m[0], m[4], m[12], 0.0f);
            gl::Uniform4f(row1_at_, m[1], m[5], m[13], 0.0f);
            gl::BindBuffer(GL_ARRAY_BUFFER, vertices_);
            gl::EnableVertexAttribArray(static_cast<GLuint>(position_at_));
            gl::VertexAttribPointer(static_cast<GLuint>(position_at_), 2, GL_FLOAT, GL_FALSE, 0,
                                    nullptr);
            gl::DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            gl::DisableVertexAttribArray(static_cast<GLuint>(position_at_));
            gl::BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        }
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        gl::BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        gl::BindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
        gl::UseProgram(static_cast<GLuint>(program));
        gl::ActiveTexture(static_cast<GLenum>(active));
        gl::Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (blend) gl::Enable(GL_BLEND);
        if (scissor) gl::Enable(GL_SCISSOR_TEST);
        if (!complete) {
            static bool said = false;
            if (!said) {
                said = true;
                log_warn("video decoder: a screen's texture cannot be drawn into");
            }
        }
        return complete;
    }

private:
    SurfaceOutput() = default;

    bool build() {
        const GLuint vertex = compile_shader(GL_VERTEX_SHADER, kSurfaceVertexShader);
        const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, kSurfaceFragmentShader);
        if (vertex == 0 || fragment == 0) {
            if (vertex != 0) gl::DeleteShader(vertex);
            if (fragment != 0) gl::DeleteShader(fragment);
            return false;
        }
        program_ = gl::CreateProgram();
        gl::AttachShader(program_, vertex);
        gl::AttachShader(program_, fragment);
        gl::LinkProgram(program_);
        gl::DeleteShader(vertex);
        gl::DeleteShader(fragment);
        GLint linked = 0;
        gl::GetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (!linked) {
            log_warn("video decoder: the Surface's shader would not link");
            return false;
        }
        position_at_ = gl::GetAttribLocation(program_, "a_position");
        picture_at_ = gl::GetUniformLocation(program_, "u_picture");
        row0_at_ = gl::GetUniformLocation(program_, "u_row0");
        row1_at_ = gl::GetUniformLocation(program_, "u_row1");
        if (position_at_ < 0) return false;

        const GLfloat corners[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        GLint array_buffer = 0;
        gl::GetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        gl::GenBuffers(1, &vertices_);
        gl::BindBuffer(GL_ARRAY_BUFFER, vertices_);
        gl::BufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
        gl::BindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
        gl::GenFramebuffers(1, &framebuffer_);

        gl::GenTextures(1, &external_);
        gl::BindTexture(GL_TEXTURE_EXTERNAL_OES, external_);
        gl::TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl::BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (env == nullptr) return false;
        jclass texture_type = env->FindClass("android/graphics/SurfaceTexture");
        if (jni_failed(env) || texture_type == nullptr) return false;
        jclass surface_type = env->FindClass("android/view/Surface");
        if (jni_failed(env) || surface_type == nullptr) return false;
        jmethodID make_texture = env->GetMethodID(texture_type, "<init>", "(I)V");
        update_ = env->GetMethodID(texture_type, "updateTexImage", "()V");
        timestamp_ = env->GetMethodID(texture_type, "getTimestamp", "()J");
        transform_ = env->GetMethodID(texture_type, "getTransformMatrix", "([F)V");
        release_ = env->GetMethodID(texture_type, "release", "()V");
        jmethodID make_surface =
            env->GetMethodID(surface_type, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
        if (jni_failed(env) || make_texture == nullptr || update_ == nullptr ||
            timestamp_ == nullptr || transform_ == nullptr || make_surface == nullptr) {
            return false;
        }
        jobject texture = env->NewObject(texture_type, make_texture, static_cast<jint>(external_));
        if (jni_failed(env) || texture == nullptr) return false;
        texture_ = env->NewGlobalRef(texture);
        env->DeleteLocalRef(texture);
        jobject surface = env->NewObject(surface_type, make_surface, texture_);
        if (jni_failed(env) || surface == nullptr) return false;
        surface_ = env->NewGlobalRef(surface);
        window_ = ANativeWindow_fromSurface(env, surface);
        env->DeleteLocalRef(surface);
        env->DeleteLocalRef(texture_type);
        env->DeleteLocalRef(surface_type);
        return window_ != nullptr;
    }

    GLuint external_ = 0;
    GLuint framebuffer_ = 0;
    GLuint vertices_ = 0;
    GLuint program_ = 0;
    GLint position_at_ = -1;
    GLint picture_at_ = -1;
    GLint row0_at_ = -1;
    GLint row1_at_ = -1;
    jobject texture_ = nullptr;
    jobject surface_ = nullptr;
    jmethodID update_ = nullptr;
    jmethodID timestamp_ = nullptr;
    jmethodID transform_ = nullptr;
    jmethodID release_ = nullptr;
    ANativeWindow* window_ = nullptr;
    int64_t latched_pts_ = -1;
    bool broken_ = false;
};

// One decoded picture, as the session holds it: whatever the Surface has
// latched when it is drawn, which is this picture or a newer one.
class SurfacePicture : public GpuPicture {
public:
    explicit SurfacePicture(std::shared_ptr<SurfaceOutput> output) : output_(std::move(output)) {}
    bool draw_into(unsigned texture, int width, int height) override {
        return output_->draw_into(texture, width, height);
    }
    bool ready() override { return output_->has_picture(); }

private:
    std::shared_ptr<SurfaceOutput> output_;
};

}  // namespace

struct MediaCodecDecoder::Impl {
    // One access unit waiting for the worker, with the timestamp it goes in
    // under -- which is also how the picture it becomes is recognised.
    struct Unit {
        std::vector<uint8_t> data;
        int64_t pts = 0;
    };

    // Between decode() and the worker, under `mutex`.
    std::mutex mutex;
    std::condition_variable work;      // the worker has something to do
    std::condition_variable pictures;  // a picture has come out
    std::deque<Unit> queued;
    bool stopping = false;
    DecodedFrame ready;      // the newest picture not yet taken
    int64_t ready_pts = -1;  // the frame the newest picture so far was
    std::string failure;     // said by the next decode()

    // decode()'s own.
    int64_t next_pts = 0;
    int64_t wait_us = kPictureWaitUs;
    int missed_waits = 0;
    // Whether decode() waits for the picture of the frame it was handed (0)
    // or of the one before (1) -- see kPreviousPictureWaitUs.
    int behind = 0;

    // The worker's own: nothing else touches the codec.
    ParameterSets sets;
    AMediaCodec* codec = nullptr;
    std::string name;
    OutputLayout layout;
    int configured_width = 0;
    int configured_height = 0;
    // Frames fed in and not yet come out.
    int64_t in_flight = 0;
    bool said_format = false;
    bool said_first_picture = false;
    // How long a frame spends inside the decoder, fed to finished, said every
    // few seconds: whether it hands each picture straight back or holds it
    // until the next frame pushes it out is most of the latency it adds.
    std::deque<std::pair<int64_t, std::chrono::steady_clock::time_point>> fed_at;
    int64_t inside_total_us = 0;
    int64_t inside_worst_us = 0;
    int inside_frames = 0;
    // And how many of those came out within 2ms of the next frame going in:
    // nearly all of them means the decoder releases a picture only when the
    // next frame pushes it out, rather than when it is done.
    std::chrono::steady_clock::time_point last_fed = std::chrono::steady_clock::now();
    int pushed_out = 0;
    std::chrono::steady_clock::time_point inside_since = std::chrono::steady_clock::now();

    // Where pictures go when they stay on the GPU; null when they are copied
    // out as bytes. Made on the renderer's thread before the worker starts,
    // and not changed after, except that the worker gives up on it if the
    // decoder refuses it (use_surface).
    std::shared_ptr<SurfaceOutput> surface;
    bool use_surface = false;

    std::thread worker;

    explicit Impl(std::shared_ptr<SurfaceOutput> output)
        : surface(std::move(output)), use_surface(surface != nullptr),
          worker([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        work.notify_all();
        worker.join();
        destroy_codec();
        surface.reset();
    }

    void run() {
        pthread_setname_np(pthread_self(), "MediaCodec");
        while (true) {
            std::deque<Unit> units;
            {
                std::unique_lock<std::mutex> lock(mutex);
                // With nothing inside the decoder there is nothing to collect,
                // so this sleeps until a frame comes; otherwise it goes
                // straight on to see whether one has come out.
                if (codec == nullptr || in_flight == 0) {
                    work.wait(lock, [this] { return stopping || !queued.empty(); });
                }
                if (stopping) return;
                units.swap(queued);
            }
            std::string error;
            bool ok = true;
            for (Unit& unit : units) {
                if (!(ok = feed(unit, &error))) break;
            }
            if (ok && codec != nullptr) {
                ok = collect(units.empty() && in_flight > 0 ? kOutputWaitUs : 0, &error);
            }
            if (!ok) {
                // The codec is not trusted again: the session reopens the
                // decoder when it hears, and asks for a keyframe.
                destroy_codec();
                sets = ParameterSets();
                std::lock_guard<std::mutex> lock(mutex);
                failure = error;
                queued.clear();
                pictures.notify_all();
            }
        }
    }

    // Whatever the decoder has finished, newest kept, handed to decode().
    bool collect(int64_t wait, std::string* error) {
        DecodedFrame newest;
        int64_t pts = -1;
        if (!drain(newest, &pts, wait, error)) return false;
        if (newest.valid()) {
            std::lock_guard<std::mutex> lock(mutex);
            ready = std::move(newest);
            ready_pts = pts;
            pictures.notify_all();
        }
        return true;
    }

    bool feed(const Unit& unit, std::string* error) {
        // Parameter sets arrive inside a keyframe, and only a change is worth
        // acting on: a stream repeats them on every keyframe, and a rebuilt
        // codec drops whatever was in flight through it.
        if (collect_parameter_sets(unit.data.data(), unit.data.size(), sets) || codec == nullptr) {
            if (sets.complete() && !build_codec(error)) return false;
        }
        if (codec == nullptr) {
            // Nothing to decode against yet; everything before the first
            // keyframe is dropped, and the session asks for one.
            return true;
        }
        const auto began = std::chrono::steady_clock::now();
        ssize_t slot = AMediaCodec_dequeueInputBuffer(codec, kInputWaitUs);
        while (slot < 0) {
            // Full. Taking its pictures out is what frees room to put one in.
            if (!collect(0, error)) return false;
            if (micros_since(began) > kInputGiveUpUs) {
                if (error != nullptr) *error = "MediaCodec has taken no frame in for too long";
                return false;
            }
            slot = AMediaCodec_dequeueInputBuffer(codec, kInputWaitUs);
        }
        std::size_t capacity = 0;
        uint8_t* input = AMediaCodec_getInputBuffer(codec, static_cast<size_t>(slot), &capacity);
        const std::size_t size = unit.data.size();
        if (input == nullptr || capacity < size) {
            AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(slot), 0, 0, unit.pts, 0);
            if (error != nullptr) {
                *error = "a frame of " + std::to_string(size) +
                         " bytes does not fit MediaCodec's " + std::to_string(capacity);
            }
            return false;
        }
        // The access unit exactly as it came, parameter sets included:
        // MediaCodec takes Annex B, and in-band SPS/PPS are what a stream
        // carries anyway.
        std::memcpy(input, unit.data.data(), size);
        const media_status_t queued_ok = AMediaCodec_queueInputBuffer(
            codec, static_cast<size_t>(slot), 0, size, unit.pts, 0);
        if (queued_ok != AMEDIA_OK) {
            if (error != nullptr) *error = "MediaCodec refused a frame: " + std::to_string(queued_ok);
            return false;
        }
        ++in_flight;
        last_fed = std::chrono::steady_clock::now();
        fed_at.emplace_back(unit.pts, last_fed);
        if (fed_at.size() > 64) fed_at.pop_front();
        return true;
    }

    void destroy_codec() {
        if (codec != nullptr) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
            codec = nullptr;
        }
        layout = OutputLayout();
        in_flight = 0;
    }

    AMediaFormat* make_format(bool low_latency) const {
        AMediaFormat* format = AMediaFormat_new();
        AMediaFormat_setString(format, "mime", "video/avc");
        AMediaFormat_setInt32(format, "width", configured_width);
        AMediaFormat_setInt32(format, "height", configured_height);
        // The parameter sets, start codes and all: "csd-0" and "csd-1" are
        // documented as Annex B for H.264.
        std::vector<uint8_t> csd;
        auto set_csd = [&](const char* key, const std::vector<uint8_t>& nal) {
            csd.assign({0, 0, 0, 1});
            csd.insert(csd.end(), nal.begin(), nal.end());
            AMediaFormat_setBuffer(format, key, csd.data(), csd.size());
        };
        set_csd("csd-0", sets.sps);
        set_csd("csd-1", sets.pps);
        // What the host's encoder makes: full range, BT.601 -- see
        // VideoEncoder, and expand_planar_yuv() in companion_session.cpp. The
        // stream says full range but not which matrix, and decoding into a
        // Surface leaves the conversion to colour to the system, which on a
        // G Cloud took it for limited range and drew every picture too dark.
        // MediaFormat.COLOR_RANGE_FULL, COLOR_STANDARD_BT601_NTSC (the same
        // matrix as PAL's), COLOR_TRANSFER_SDR_VIDEO.
        AMediaFormat_setInt32(format, "color-range", 1);
        AMediaFormat_setInt32(format, "color-standard", 4);
        AMediaFormat_setInt32(format, "color-transfer", 3);
        if (low_latency) {
            // Hand each picture out as soon as it is decoded rather than
            // holding a few for smoothness. The standard key since Android 11,
            // and the vendors' own before that -- the same set Moonlight uses.
            // Keys a decoder does not know are ignored, but a decoder that
            // dislikes them can refuse to configure at all, which is why
            // configure() is tried again without them.
            AMediaFormat_setInt32(format, "low-latency", 1);
            AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
            AMediaFormat_setInt32(format, "vendor.qti-ext-dec-picture-order.enable", 1);
            AMediaFormat_setInt32(format, "vendor.rtc-ext-dec-low-latency.enable", 1);
            AMediaFormat_setInt32(format, "vendor.low-latency.enable", 1);
            // Realtime, as against best effort.
            AMediaFormat_setInt32(format, "priority", 0);
            // No "operating-rate". A G Cloud's decoder refuses Short.MAX_VALUE
            // (what Moonlight asks of some), and takes 240 -- and then
            // reserves the hardware for 1080p at 240fps, which it does not
            // have: every frame after fails with HW_OVERLOAD and there is no
            // picture at all.
        }
        return format;
    }

    void time_inside(int64_t pts) {
        while (!fed_at.empty() && fed_at.front().first < pts) fed_at.pop_front();
        if (fed_at.empty() || fed_at.front().first != pts) return;
        const int64_t inside = micros_since(fed_at.front().second);
        fed_at.pop_front();
        inside_total_us += inside;
        inside_worst_us = std::max(inside_worst_us, inside);
        ++inside_frames;
        if (micros_since(last_fed) < 2000 && fed_at.size() >= 1) ++pushed_out;
        if (micros_since(inside_since) < 5000000) return;
        char text[224];
        std::snprintf(text, sizeof(text),
                      "video decoder: a frame spends %.1fms inside MediaCodec on average, %.1fms "
                      "at worst, over %d frames, %d of them within 2ms of the next going in; "
                      "%lld in it now",
                      static_cast<double>(inside_total_us) / 1000.0 / inside_frames,
                      static_cast<double>(inside_worst_us) / 1000.0, inside_frames, pushed_out,
                      static_cast<long long>(in_flight));
        log_info(text);
        inside_total_us = inside_worst_us = 0;
        inside_frames = 0;
        pushed_out = 0;
        inside_since = std::chrono::steady_clock::now();
    }

    bool build_codec(std::string* error) {
        destroy_codec();
        fed_at.clear();
        int width = 0;
        int height = 0;
        if (!sps_picture_size(sets.sps.data(), sets.sps.size(), &width, &height)) {
            // Only a hint to the decoder, which reads the real size from the
            // stream; this is what most of them are happy to be told.
            width = 1280;
            height = 720;
        }
        configured_width = width;
        configured_height = height;

        codec = AMediaCodec_createDecoderByType("video/avc");
        if (codec == nullptr) {
            if (error != nullptr) *error = "this device has no H.264 decoder";
            return false;
        }
        media_status_t status = AMEDIA_ERROR_UNKNOWN;
        // Into the Surface if there is one, with the low-latency settings and
        // then without; then the same again into bytes.
        bool on_surface = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const bool low_latency = attempt % 2 == 0;
            on_surface = attempt < 2;
            if (on_surface && !use_surface) continue;
            AMediaFormat* format = make_format(low_latency);
            status = AMediaCodec_configure(codec, format, on_surface ? surface->window() : nullptr,
                                           nullptr, 0);
            AMediaFormat_delete(format);
            if (status == AMEDIA_OK) {
                break;
            }
            log_info(std::string("video decoder: MediaCodec would not configure ") +
                     (on_surface ? "into a Surface" : "into bytes") +
                     (low_latency ? " with the low-latency settings (" : " (") +
                     std::to_string(status) + "), trying another way");
            // A codec that has refused a configure is not promised to take
            // another, so the next try is on a fresh one.
            AMediaCodec_delete(codec);
            codec = AMediaCodec_createDecoderByType("video/avc");
            if (codec == nullptr) break;
        }
        if (codec != nullptr && status == AMEDIA_OK && use_surface && !on_surface) {
            use_surface = false;
            log_info("video decoder: decoding into bytes, since the Surface was refused");
        }
        if (codec == nullptr || status != AMEDIA_OK) {
            if (error != nullptr) {
                *error = "MediaCodec would not configure: " + std::to_string(status);
            }
            if (codec != nullptr) {
                AMediaCodec_delete(codec);
                codec = nullptr;
            }
            return false;
        }
        status = AMediaCodec_start(codec);
        if (status != AMEDIA_OK) {
            if (error != nullptr) *error = "MediaCodec would not start: " + std::to_string(status);
            AMediaCodec_delete(codec);
            codec = nullptr;
            return false;
        }
        name = codec_name_of(codec);
        said_first_picture = false;
        log_info("video decoder: MediaCodec " + (name.empty() ? std::string("(unnamed)") : name) +
                 ", configured for " + std::to_string(width) + "x" + std::to_string(height) +
                 (use_surface ? ", into a Surface the GPU draws from" : ", into bytes"));
        return true;
    }

    void take_output_format() {
        AMediaFormat* format = AMediaCodec_getOutputFormat(codec);
        if (format == nullptr) return;
        layout = read_layout(format, configured_width, configured_height);
        AMediaFormat_delete(format);
        if (!said_format) {
            said_format = true;
            std::string what = "colour format " + hex(layout.color_format);
            if (!layout.known) {
                what += ", a tiled layout this app cannot read";
            } else if (layout.planar) {
                what += " (I420)";
            } else if (layout.color_format == kColorFormatYuv420SemiPlanar ||
                       layout.color_format == kColorFormatQcomYuv420SemiPlanar32m ||
                       layout.color_format == kColorFormatQcomYuv420SemiPlanar) {
                what += " (NV12)";
            } else {
                what += ", read as NV12";
            }
            log_info("video decoder: pictures arrive " + std::to_string(layout.width) + "x" +
                     std::to_string(layout.height) + ", stride " + std::to_string(layout.stride) +
                     ", " + std::to_string(layout.slice_height) + " rows to a plane, " + what);
        }
    }

    // One output buffer, copied out into `out` as planes.
    bool copy_out(const uint8_t* buffer, std::size_t size, DecodedFrame& out, std::string* error) {
        if (!layout.known) {
            if (error != nullptr) *error = "the decoder writes a layout this app cannot read";
            return false;
        }
        const auto began = std::chrono::steady_clock::now();
        const std::size_t stride = static_cast<std::size_t>(layout.stride);
        const std::size_t rows = static_cast<std::size_t>(layout.height);
        const std::size_t chroma_rows = (rows + 1) / 2;
        const std::size_t luma_plane = stride * static_cast<std::size_t>(layout.slice_height);
        const std::size_t luma_at =
            static_cast<std::size_t>(layout.crop_top) * stride + layout.crop_left;

        DecodedFrame frame;
        frame.width = layout.width;
        frame.height = layout.height;
        // `row_bytes` is how much of a row is picture. The last row needs
        // only that, not its padding: a decoder is free to end the buffer
        // straight after it.
        auto copy_plane = [&](int index, std::size_t at, std::size_t plane_stride,
                              std::size_t plane_rows, std::size_t row_bytes) {
            const std::size_t needed = plane_stride * (plane_rows - 1) + row_bytes;
            if (at > size || needed > size - at) {
                return false;
            }
            const std::size_t available = std::min(plane_stride * plane_rows, size - at);
            frame.plane[index].assign(buffer + at, buffer + at + available);
            frame.plane[index].resize(plane_stride * plane_rows);
            frame.stride[index] = static_cast<int>(plane_stride);
            return true;
        };
        const std::size_t width = static_cast<std::size_t>(layout.width);

        bool copied = copy_plane(0, luma_at, stride, rows, width);
        if (layout.planar) {
            frame.layout = DecodedLayout::Yuv420p;
            const std::size_t chroma_stride = stride / 2;
            const std::size_t chroma_plane =
                chroma_stride * static_cast<std::size_t>((layout.slice_height + 1) / 2);
            const std::size_t chroma_at = static_cast<std::size_t>(layout.crop_top / 2) *
                                              chroma_stride + layout.crop_left / 2;
            copied = copied && copy_plane(1, luma_plane + chroma_at, chroma_stride, chroma_rows,
                                (width + 1) / 2) &&
                     copy_plane(2, luma_plane + chroma_plane + chroma_at, chroma_stride,
                                chroma_rows, (width + 1) / 2);
        } else {
            frame.layout = DecodedLayout::Nv12;
            const std::size_t chroma_at =
                static_cast<std::size_t>(layout.crop_top / 2) * stride + layout.crop_left;
            copied = copied && copy_plane(1, luma_plane + chroma_at, stride, chroma_rows,
                                         (width + 1) & ~std::size_t{1});
        }
        if (!copied) {
            if (error != nullptr) {
                *error = "a decoded picture was smaller than its format says (" +
                         std::to_string(size) + " bytes for " + std::to_string(layout.width) +
                         "x" + std::to_string(layout.height) + ", stride " +
                         std::to_string(layout.stride) + ")";
            }
            return false;
        }
        frame.readback_micros = static_cast<int>(micros_since(began));
        frame.lock_micros = out.lock_micros;
        out = std::move(frame);
        return true;
    }

    // Takes everything the decoder has finished, keeping the newest picture:
    // this is a live screen, and an older frame has nothing to offer a
    // player. Waits up to `wait_us_now` for the first one, and says which
    // frame the picture was by the timestamp it went in under.
    bool drain(DecodedFrame& out, int64_t* out_pts, int64_t wait_us_now, std::string* error) {
        bool produced = false;
        int64_t timeout = wait_us_now;
        // Into a Surface, only the newest picture is put on it; any older one
        // that came out in the same go is let go unseen.
        ssize_t newest = -1;
        int64_t newest_pts = 0;
        while (true) {
            AMediaCodecBufferInfo info;
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout);
            if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                break;
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                take_output_format();
                continue;
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;  // the NDK fetches each buffer by index anyway
            }
            if (index < 0) {
                if (error != nullptr) {
                    *error = "MediaCodec failed while decoding: " + std::to_string(index);
                }
                return false;
            }
            // Once one has come out, the rest are only taken if they are
            // already there -- never waited for.
            timeout = 0;
            if (in_flight > 0) --in_flight;
            if (use_surface) {
                // The size says nothing here: a picture on a Surface has no
                // bytes for it to count.
                if ((info.flags & kBufferFlagCodecConfig) != 0) {
                    AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
                    continue;
                }
                if (newest >= 0) {
                    AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(newest), false);
                }
                newest = index;
                newest_pts = info.presentationTimeUs;
                time_inside(info.presentationTimeUs);
                continue;
            }
            if ((info.flags & kBufferFlagCodecConfig) != 0 || info.size <= 0) {
                AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
                continue;
            }
            if (!layout.known && layout.color_format == 0) {
                take_output_format();
            }
            std::size_t capacity = 0;
            uint8_t* buffer = AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(index),
                                                          &capacity);
            bool ok = true;
            if (buffer != nullptr && info.offset >= 0 &&
                static_cast<std::size_t>(info.offset) + static_cast<std::size_t>(info.size) <=
                    capacity) {
                ok = copy_out(buffer + info.offset, static_cast<std::size_t>(info.size), out,
                              error);
                if (ok) {
                    *out_pts = info.presentationTimeUs;
                    time_inside(info.presentationTimeUs);
                }
                produced = produced || ok;
            }
            AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
            if (!ok) {
                return false;
            }
        }
        if (newest >= 0) {
            AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(newest), true);
            out = DecodedFrame();
            out.layout = DecodedLayout::Gpu;
            out.width = layout.width > 0 ? layout.width : configured_width;
            out.height = layout.height > 0 ? layout.height : configured_height;
            out.gpu = std::make_shared<SurfacePicture>(surface);
            *out_pts = newest_pts;
            produced = true;
        }
        if (produced && !said_first_picture) {
            said_first_picture = true;
            log_info("video decoder: first picture out of MediaCodec, " +
                     std::to_string(out.width) + "x" + std::to_string(out.height));
        }
        return true;
    }
};

MediaCodecDecoder::MediaCodecDecoder() = default;
MediaCodecDecoder::~MediaCodecDecoder() = default;

bool MediaCodecDecoder::available() { return true; }

bool MediaCodecDecoder::open(std::string* /*error*/) {
    // Here, on the renderer's thread: the Surface's texture is made in its
    // GL context.
    impl_ = std::make_unique<Impl>(SurfaceOutput::create());
    return true;
}

void MediaCodecDecoder::close() { impl_.reset(); }

bool MediaCodecDecoder::is_open() const { return impl_ != nullptr; }

bool MediaCodecDecoder::decode(const uint8_t* data, std::size_t size, DecodedFrame& out,
                               std::string* error) {
    out = DecodedFrame();
    if (impl_ == nullptr || data == nullptr || size == 0) {
        return true;
    }
    Impl& impl = *impl_;
    const auto began = std::chrono::steady_clock::now();

    std::unique_lock<std::mutex> lock(impl.mutex);
    if (!impl.failure.empty()) {
        if (error != nullptr) *error = impl.failure;
        impl.failure.clear();
        return false;
    }
    if (impl.queued.size() >= kMostFramesQueued) {
        if (error != nullptr) {
            *error = "MediaCodec has fallen " + std::to_string(impl.queued.size()) +
                     " frames behind";
        }
        return false;
    }
    // Steady, made-up timestamps: nothing here is presented by them, but a
    // decoder that sees them go backwards or repeat may reorder or drop.
    impl.next_pts += 16667;
    const int64_t pts = impl.next_pts;
    impl.queued.push_back(Impl::Unit {std::vector<uint8_t>(data, data + size), pts});
    impl.work.notify_one();

    // A decoder that has never handed a picture back has nothing to say
    // about whether waiting works: before the first keyframe nothing comes
    // out at all.
    if (impl.ready_pts >= 0) {
        const int64_t wanted = pts - 16667 * impl.behind;
        const int64_t wait = impl.behind == 0 ? impl.wait_us : kPreviousPictureWaitUs;
        const bool came = impl.pictures.wait_for(
            lock, std::chrono::microseconds(wait),
            [&impl, wanted] { return impl.ready_pts >= wanted || !impl.failure.empty(); });
        if (came) {
            impl.missed_waits = 0;
        } else if (impl.behind == 0 && ++impl.missed_waits >= kMissedWaitsBeforeGivingUp) {
            impl.behind = 1;
            impl.missed_waits = 0;
            log_info("video decoder: MediaCodec takes longer than a frame to hand a picture "
                     "back, so each frame now shows the one before it, evenly");
        }
    }
    if (impl.ready.valid()) {
        out = std::move(impl.ready);
        impl.ready = DecodedFrame();
        const int64_t ready_pts = impl.ready_pts;
        lock.unlock();
        if (out.layout == DecodedLayout::Gpu && impl.surface != nullptr) {
            // Taken into the Surface's texture now, here on the renderer's
            // thread, so that a picture the session never draws still leaves
            // the Surface's queue -- a decoder whose Surface is full stops.
            // Waits a moment for it: a picture released to a Surface arrives
            // there just after, not at once.
            impl.surface->latch(ready_pts, 4000);
            if (impl.surface->broken()) {
                if (error != nullptr) *error = "the decoder's Surface stopped working";
                set_native_frames_usable(false);
                return false;
            }
        }
        // How long this call stood waiting for it; what copying it out cost
        // is the worker's, in readback_micros.
        out.lock_micros = static_cast<int>(micros_since(began));
    }
    return true;
}

}  // namespace ds::video

#else  // not Android

namespace ds::video {

struct MediaCodecDecoder::Impl {};

MediaCodecDecoder::MediaCodecDecoder() = default;
MediaCodecDecoder::~MediaCodecDecoder() = default;

bool MediaCodecDecoder::available() { return false; }

bool MediaCodecDecoder::open(std::string* error) {
    if (error != nullptr) *error = "this build has no MediaCodec";
    return false;
}

void MediaCodecDecoder::close() {}
bool MediaCodecDecoder::is_open() const { return false; }

bool MediaCodecDecoder::decode(const uint8_t*, std::size_t, DecodedFrame& out, std::string*) {
    out = DecodedFrame();
    return false;
}

}  // namespace ds::video

#endif

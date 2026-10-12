#include "render/quad_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/log.hpp"

namespace ds {
namespace {

// GLSL 1.00 ES and GLSL 1.20 desktop share the attribute/varying syntax used
// here, so the only difference between the two is the preamble. A desktop core
// profile removed that syntax outright, so it gets a preamble that defines it
// back rather than a second copy of both shaders -- see core_preamble().
const char* kVertexBody = R"(
attribute vec2 a_position;   // in output pixels, origin top-left
attribute vec2 a_texcoord;
uniform vec2 u_viewport;     // output size in pixels
varying vec2 v_texcoord;
void main() {
    vec2 normalized = a_position / u_viewport;          // [0,1]
    vec2 clip = vec2(normalized.x * 2.0 - 1.0,
                     1.0 - normalized.y * 2.0);         // flip Y: top-left origin
    gl_Position = vec4(clip, 0.0, 1.0);
    v_texcoord = a_texcoord;
}
)";

const char* kFragmentBody = R"(
uniform sampler2D u_texture;
uniform sampler2D u_chroma;   // NV12's interleaved UV, or YUV420P's U
uniform sampler2D u_chroma_v; // YUV420P's V
uniform vec4 u_color;
// 0 solid, 1 screen (XRGB), 2 alpha mask, 3 screen (RGBA),
// 4 screen (NV12), 5 screen (YUV420P), 6 image (RGBA, its own alpha)
uniform int u_mode;
// Which component of NV12's two-channel chroma texel holds V: 1 for .a, which
// is where GL_LUMINANCE_ALPHA puts it, 0 for .g, which is where GL_RG does.
// Set from nv12_chroma_v_in_alpha() -- see texture.cpp, where the choice is
// made once by asking the driver whether it still has the LUMINANCE formats.
uniform int u_chroma_v_in_alpha;
varying vec2 v_texcoord;

// Full-range BT.601, matching what the encoder signals and what the CPU
// conversion this replaces was set up for (see sws_setColorspaceDetails in
// video_decoder.cpp): luma is used as-is rather than stretched from 16-235,
// and chroma is centred on 0.5.
vec3 ds_yuv_to_rgb(float y, float u, float v) {
    float cb = u - 0.5;
    float cr = v - 0.5;
    return vec3(y + 1.402 * cr,
                y - 0.344136 * cb - 0.714136 * cr,
                y + 1.772 * cb);
}

void main() {
    if (u_mode == 0) {
        gl_FragColor = u_color;
    } else if (u_mode == 1) {
        // Core output arrives as XRGB8888, uploaded verbatim as GL_RGBA, so the
        // channels come back reversed and the X byte must be ignored.
        vec4 texel = texture2D(u_texture, v_texcoord);
        gl_FragColor = vec4(texel.bgr * u_color.rgb, u_color.a);
    } else if (u_mode == 2) {
        float coverage = texture2D(u_texture, v_texcoord).a;
        gl_FragColor = vec4(u_color.rgb, u_color.a * coverage);
    } else if (u_mode == 4) {
        // A video frame, still in the planes its decoder produced. The
        // conversion below is the whole reason those planes are uploaded
        // rather than turned into RGB on the CPU first -- here it is a few
        // multiplies per fragment on hardware built for them, and there it
        // was a pass over every pixel of every frame.
        //
        // NV12: one byte of luma, and a half-size plane holding U and V
        // interleaved. U arrives in .r either way; V lands wherever the
        // two-channel format this build could get puts the second byte.
        float y = texture2D(u_texture, v_texcoord).r;
        vec4 chroma = texture2D(u_chroma, v_texcoord);
        float v = u_chroma_v_in_alpha == 1 ? chroma.a : chroma.g;
        gl_FragColor = vec4(ds_yuv_to_rgb(y, chroma.r, v) * u_color.rgb, u_color.a);
    } else if (u_mode == 5) {
        // The same, with U and V in planes of their own.
        float y = texture2D(u_texture, v_texcoord).r;
        float u = texture2D(u_chroma, v_texcoord).r;
        float v = texture2D(u_chroma_v, v_texcoord).r;
        gl_FragColor = vec4(ds_yuv_to_rgb(y, u, v) * u_color.rgb, u_color.a);
    } else if (u_mode == 6) {
        // A picture with see-through parts of its own: a game's icon.
        vec4 texel = texture2D(u_texture, v_texcoord);
        gl_FragColor = vec4(texel.rgb * u_color.rgb, texel.a * u_color.a);
    } else {
        // A screen the GPU filled in itself: already in RGBA order, so
        // swizzling it the way core output needs would turn it inside out.
        vec4 texel = texture2D(u_texture, v_texcoord);
        gl_FragColor = vec4(texel.rgb * u_color.rgb, u_color.a);
    }
}
)";

// What a GLSL 1.20 shader needs spelled differently under a core profile:
// attribute/varying became in/out, gl_FragColor became a declared output, and
// texture2D became texture. Defining them back keeps one copy of each shader.
std::string core_preamble(GLenum type) {
    std::string preamble = "#version 150\n#define texture2D texture\n";
    if (type == GL_VERTEX_SHADER) {
        preamble += "#define attribute in\n#define varying out\n";
    } else {
        preamble +=
            "#define varying in\nout vec4 ds_FragColor;\n#define gl_FragColor ds_FragColor\n";
    }
    return preamble;
}

std::string preamble_for(GLenum type) {
    if (gl::is_gles()) return "#version 100\nprecision mediump float;\n";
    // A core profile is the one that will not take #version 120. Asking the
    // context rather than assuming from what was requested, because the app
    // takes whatever context it can get.
    if (gl::is_core_profile()) return core_preamble(type);
    return "#version 120\n";
}

GLuint compile_shader(GLenum type, const std::string& source, std::string* error) {
    const GLuint shader = gl::CreateShader(type);
    if (shader == 0) {
        if (error != nullptr) *error = "glCreateShader failed";
        return 0;
    }
    const char* text = source.c_str();
    const GLint length = static_cast<GLint>(source.size());
    gl::ShaderSource(shader, 1, &text, &length);
    gl::CompileShader(shader);

    GLint compiled = 0;
    gl::GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
        GLint log_length = 0;
        gl::GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        std::vector<char> log(static_cast<std::size_t>(log_length > 0 ? log_length : 1), '\0');
        gl::GetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        if (error != nullptr) {
            *error = std::string("shader failed to compile: ") + log.data();
        }
        gl::DeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

QuadRenderer::~QuadRenderer() { shutdown(); }

bool QuadRenderer::initialize(std::string* error) {
    const GLuint vertex_shader =
        compile_shader(GL_VERTEX_SHADER, preamble_for(GL_VERTEX_SHADER) + kVertexBody, error);
    if (vertex_shader == 0) return false;
    const GLuint fragment_shader =
        compile_shader(GL_FRAGMENT_SHADER, preamble_for(GL_FRAGMENT_SHADER) + kFragmentBody, error);
    if (fragment_shader == 0) {
        gl::DeleteShader(vertex_shader);
        return false;
    }

    program_ = gl::CreateProgram();
    gl::AttachShader(program_, vertex_shader);
    gl::AttachShader(program_, fragment_shader);
    gl::LinkProgram(program_);
    gl::DeleteShader(vertex_shader);
    gl::DeleteShader(fragment_shader);

    GLint linked = 0;
    gl::GetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        GLint log_length = 0;
        gl::GetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_length);
        std::vector<char> log(static_cast<std::size_t>(log_length > 0 ? log_length : 1), '\0');
        gl::GetProgramInfoLog(program_, static_cast<GLsizei>(log.size()), nullptr, log.data());
        if (error != nullptr) {
            *error = std::string("shader program failed to link: ") + log.data();
        }
        gl::DeleteProgram(program_);
        program_ = 0;
        return false;
    }

    attr_position_ = gl::GetAttribLocation(program_, "a_position");
    attr_texcoord_ = gl::GetAttribLocation(program_, "a_texcoord");
    uniform_viewport_ = gl::GetUniformLocation(program_, "u_viewport");
    uniform_color_ = gl::GetUniformLocation(program_, "u_color");
    uniform_mode_ = gl::GetUniformLocation(program_, "u_mode");
    uniform_sampler_ = gl::GetUniformLocation(program_, "u_texture");
    uniform_chroma_ = gl::GetUniformLocation(program_, "u_chroma");
    uniform_chroma_v_ = gl::GetUniformLocation(program_, "u_chroma_v");
    uniform_chroma_v_in_alpha_ = gl::GetUniformLocation(program_, "u_chroma_v_in_alpha");

    if (attr_position_ < 0 || attr_texcoord_ < 0) {
        if (error != nullptr) *error = "shader program is missing its vertex attributes";
        shutdown();
        return false;
    }

    // A core profile refuses to draw with no vertex array object bound, and
    // there is no default one to fall back on. One for the whole renderer is
    // enough: every draw uses the same single buffer and attribute layout.
    if (gl::has_vertex_arrays()) {
        gl::GenVertexArrays(1, &vertex_array_);
        gl::BindVertexArray(vertex_array_);
    }

    gl::GenBuffers(1, &vertex_buffer_);
    orphan_vertex_buffer();

    // Blend state is not set here: begin_frame establishes it every frame,
    // because a core sharing this context will have changed it by then.
    return true;
}

// How many quads the vertex buffer holds before it is started afresh.
//
// Each quad used to be written over the one before it, in one six-vertex
// buffer. A desktop driver copies behind the scenes to make that cheap; the
// OpenGL ES on an iPhone, iPad or Apple TV, which runs on Metal, instead waits
// for the GPU to finish the draw still reading the old contents -- once per
// quad. A screen of text is hundreds of quads, one a glyph, and an Apple TV's
// main menu took 80 to 90ms a frame at 4K doing it, against 1ms for a game
// picture of one quad. Now every quad of a frame has its own slot, and the
// buffer gets new storage at the start of each frame (or when it fills), so
// nothing ever writes where the GPU may still be reading.
constexpr std::size_t kQuadsPerBuffer = 4096;
constexpr std::size_t kFloatsPerQuad = 24;

void QuadRenderer::orphan_vertex_buffer() {
    if (vertex_buffer_ == 0 || gl::BufferData == nullptr) {
        return;
    }
    gl::BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    gl::BufferData(GL_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(sizeof(float) * kFloatsPerQuad * kQuadsPerBuffer),
                   nullptr, GL_STREAM_DRAW);
    gl::BindBuffer(GL_ARRAY_BUFFER, 0);
    next_quad_ = 0;
}

void QuadRenderer::shutdown() {
    if (vertex_array_ != 0 && gl::DeleteVertexArrays != nullptr) {
        gl::DeleteVertexArrays(1, &vertex_array_);
        vertex_array_ = 0;
    }
    if (vertex_buffer_ != 0 && gl::DeleteBuffers != nullptr) {
        gl::DeleteBuffers(1, &vertex_buffer_);
        vertex_buffer_ = 0;
    }
    if (program_ != 0 && gl::DeleteProgram != nullptr) {
        gl::DeleteProgram(program_);
        program_ = 0;
    }
}

void QuadRenderer::begin_frame(int output_width, int output_height, float target_scale) {
    output_width_ = output_width;
    output_height_ = output_height;
    target_scale_ = target_scale > 0.0f ? target_scale : 1.0f;
    if (gl::Viewport == nullptr) {
        return;
    }
    gl::Viewport(0, 0, static_cast<int>(std::lround(output_width * target_scale_)),
                 static_cast<int>(std::lround(output_height * target_scale_)));
    orphan_vertex_buffer();

    // Every frame, not once at startup: a GPU-rendering core shares this
    // context and leaves its own state behind, so nothing set in initialize()
    // survives the first frame of a game.
    gl::reset_render_state();
    if (vertex_array_ != 0) {
        // A core profile draws nothing at all with no vertex array bound, and
        // a core that binds its own leaves ours unbound.
        gl::BindVertexArray(vertex_array_);
    }
}

void QuadRenderer::set_clip(const PixelRect& rect) {
    if (output_height_ <= 0 || program_ == 0) {
        return;
    }
    // GL measures from the bottom-left; everything above measures from the
    // top-left, so the box is flipped on its way down.
    // In the target's own pixels, which are fewer than the layout's when it
    // is drawn smaller -- see begin_frame().
    const float s = target_scale_;
    const int target_height = static_cast<int>(std::lround(output_height_ * s));
    const int x = static_cast<int>(std::floor(rect.x * s));
    const int width = static_cast<int>(std::ceil((rect.x + rect.w) * s)) - x;
    const int top = static_cast<int>(std::floor(rect.y * s));
    const int height = static_cast<int>(std::ceil((rect.y + rect.h) * s)) - top;
    gl::Enable(GL_SCISSOR_TEST);
    gl::Scissor(x, target_height - top - height, std::max(0, width), std::max(0, height));
}

void QuadRenderer::clear_clip() {
    if (program_ == 0) {
        return;
    }
    gl::Disable(GL_SCISSOR_TEST);
}

void QuadRenderer::clear(const Color& color) {
    if (program_ == 0) {
        return;
    }
    gl::ClearColor(color.r, color.g, color.b, color.a);
    gl::Clear(GL_COLOR_BUFFER_BIT);
}

void QuadRenderer::draw(const PixelRect& rect,
                        const Texture* texture,
                        const Color& color,
                        Mode mode,
                        float u0,
                        float v0,
                        float u1,
                        float v1) {
    const float corners[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
    draw_corners(rect, texture, color, mode, corners);
}

void QuadRenderer::draw_corners(const PixelRect& rect,
                                const Texture* texture,
                                const Color& color,
                                Mode mode,
                                const float corner_uv[8],
                                const bool* smooth) {
    if (program_ == 0 || rect.w <= 0.0f || rect.h <= 0.0f || output_width_ <= 0 || output_height_ <= 0) {
        return;
    }

    const float x0 = rect.x;
    const float y0 = rect.y;
    const float x1 = rect.x + rect.w;
    const float y1 = rect.y + rect.h;

    // Destination corners, clockwise from the top-left, each with the texture
    // coordinate it samples.
    const float& tl_u = corner_uv[0];
    const float& tl_v = corner_uv[1];
    const float& tr_u = corner_uv[2];
    const float& tr_v = corner_uv[3];
    const float& br_u = corner_uv[4];
    const float& br_v = corner_uv[5];
    const float& bl_u = corner_uv[6];
    const float& bl_v = corner_uv[7];

    const float vertices[24] = {
        x0, y0, tl_u, tl_v,
        x1, y0, tr_u, tr_v,
        x1, y1, br_u, br_v,
        x0, y0, tl_u, tl_v,
        x1, y1, br_u, br_v,
        x0, y1, bl_u, bl_v,
    };

    gl::UseProgram(program_);
    gl::Uniform1i(uniform_mode_, static_cast<GLint>(mode));
    gl::Uniform4f(uniform_color_, color.r, color.g, color.b, color.a);
    gl::Uniform2f(uniform_viewport_, static_cast<float>(output_width_),
                  static_cast<float>(output_height_));

    if (mode != Mode::Solid && texture != nullptr && texture->valid()) {
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, texture->id());
        gl::Uniform1i(uniform_sampler_, 0);
        if (smooth != nullptr) {
            const GLint filter = *smooth ? GL_LINEAR : GL_NEAREST;
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        }
        // A planar screen's chroma travels with it -- see ChannelOrder -- so
        // this is still one texture from every caller's point of view, and
        // only here does the second (and third) unit exist at all.
        if (is_planar(texture->channel_order())) {
            gl::ActiveTexture(GL_TEXTURE1);
            gl::BindTexture(GL_TEXTURE_2D, texture->chroma_id());
            gl::Uniform1i(uniform_chroma_, 1);
            gl::Uniform1i(uniform_chroma_v_in_alpha_, texture->chroma_v_in_alpha() ? 1 : 0);
            if (texture->channel_order() == ChannelOrder::Yuv420p) {
                gl::ActiveTexture(GL_TEXTURE2);
                gl::BindTexture(GL_TEXTURE_2D, texture->chroma_v_id());
                gl::Uniform1i(uniform_chroma_v_, 2);
            }
            // Back to unit 0, which everything else in this renderer assumes
            // is the active one.
            gl::ActiveTexture(GL_TEXTURE0);
        }
    }

    static_assert(sizeof(vertices) == sizeof(float) * kFloatsPerQuad, "one quad's vertices");
    if (next_quad_ >= kQuadsPerBuffer) {
        orphan_vertex_buffer();
    }
    gl::BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    gl::BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(next_quad_ * sizeof(vertices)),
                      static_cast<GLsizeiptr>(sizeof(vertices)), vertices);

    const GLuint position = static_cast<GLuint>(attr_position_);
    const GLuint texcoord = static_cast<GLuint>(attr_texcoord_);
    gl::EnableVertexAttribArray(position);
    gl::EnableVertexAttribArray(texcoord);
    gl::VertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                            reinterpret_cast<const void*>(0));
    gl::VertexAttribPointer(texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                            reinterpret_cast<const void*>(2 * sizeof(float)));

    gl::DrawArrays(GL_TRIANGLES, static_cast<GLint>(next_quad_ * 6), 6);
    ++next_quad_;

    gl::DisableVertexAttribArray(position);
    gl::DisableVertexAttribArray(texcoord);
    gl::BindBuffer(GL_ARRAY_BUFFER, 0);
}

void QuadRenderer::draw_solid(const PixelRect& rect, const Color& color) {
    draw(rect, nullptr, color, Mode::Solid, 0.0f, 0.0f, 1.0f, 1.0f);
}

void QuadRenderer::draw_screen(const PixelRect& rect,
                               const Texture& texture,
                               const Color& tint,
                               ScreenRotation rotation,
                               bool smooth) {
    // The texture itself says which order its bytes are in, so a caller never
    // has to know whether this screen came from the CPU or the GPU.
    Mode mode = Mode::Screen;
    switch (texture.channel_order()) {
        case ChannelOrder::Rgba: mode = Mode::ScreenRgba; break;
        case ChannelOrder::Nv12: mode = Mode::ScreenNv12; break;
        case ChannelOrder::Yuv420p: mode = Mode::ScreenYuv420p; break;
        case ChannelOrder::XrgbBytes: mode = Mode::Screen; break;
    }
    // Which source corner each destination corner samples. Turning the picture
    // clockwise means the destination's top-left shows what used to be the
    // bottom-left, and so on round.
    switch (rotation) {
        case ScreenRotation::Clockwise: {
            const float corners[8] = {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f};
            draw_corners(rect, &texture, tint, mode, corners, &smooth);
            return;
        }
        case ScreenRotation::CounterClockwise: {
            const float corners[8] = {1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f};
            draw_corners(rect, &texture, tint, mode, corners, &smooth);
            return;
        }
        case ScreenRotation::None: break;
    }
    const float upright[8] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    draw_corners(rect, &texture, tint, mode, upright, &smooth);
}

void QuadRenderer::draw_alpha_mask(const PixelRect& rect,
                                   const Texture& texture,
                                   const Color& color,
                                   float u0,
                                   float v0,
                                   float u1,
                                   float v1) {
    draw(rect, &texture, color, Mode::AlphaMask, u0, v0, u1, v1);
}

void QuadRenderer::draw_image(const PixelRect& rect, const Texture& texture, const Color& tint) {
    draw(rect, &texture, tint, Mode::Image, 0.0f, 0.0f, 1.0f, 1.0f);
}

void QuadRenderer::draw_outline(const PixelRect& rect, const Color& color, float thickness) {
    if (thickness <= 0.0f) return;
    const float t = thickness;
    draw_solid(PixelRect{rect.x, rect.y, rect.w, t}, color);
    draw_solid(PixelRect{rect.x, rect.y + rect.h - t, rect.w, t}, color);
    draw_solid(PixelRect{rect.x, rect.y + t, t, rect.h - 2.0f * t}, color);
    draw_solid(PixelRect{rect.x + rect.w - t, rect.y + t, t, rect.h - 2.0f * t}, color);
}

// The disc the capsule caps come from: one channel of coverage, opaque
// inside, fading over the last pixel at the rim so the curve has no stair on
// it. Drawn at a fraction of this size and filtered down, which is why it is
// built generously rather than at the size it will be used.
bool QuadRenderer::build_disc() {
    if (disc_.valid()) {
        return true;
    }
    constexpr int kSize = 128;
    const float centre = kSize * 0.5f;
    std::vector<uint8_t> coverage(static_cast<std::size_t>(kSize) * kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - centre;
            const float dy = static_cast<float>(y) + 0.5f - centre;
            const float inside = centre - std::sqrt(dx * dx + dy * dy);
            const float alpha = inside <= 0.0f ? 0.0f : (inside >= 1.0f ? 1.0f : inside);
            coverage[static_cast<std::size_t>(y) * kSize + x] =
                static_cast<uint8_t>(alpha * 255.0f + 0.5f);
        }
    }
    disc_.upload_alpha8(coverage.data(), kSize, kSize);
    return disc_.valid();
}

void QuadRenderer::draw_capsule(const PixelRect& rect, const Color& color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    // Half the height, or half the width when the thing is shorter than it is
    // tall -- at which point it is a circle and there is no middle left.
    const float radius = std::min(rect.h, rect.w) * 0.5f;
    if (!build_disc()) {
        // No disc, so square ends rather than nothing at all. A prompt that
        // draws with corners is a cosmetic fault; one that does not draw is a
        // control nobody can find.
        draw_solid(rect, color);
        return;
    }
    const float middle = rect.w - radius * 2.0f;
    if (middle > 0.0f) {
        draw_solid(PixelRect{rect.x + radius, rect.y, middle, rect.h}, color);
    }
    // Each cap is one half of the disc, stretched to the cap's box. The disc
    // is square and the box is radius by height, which is exactly half of a
    // square when the radius is half the height -- so the curve arrives
    // unstretched.
    draw_alpha_mask(PixelRect{rect.x, rect.y, radius, rect.h}, disc_, color, 0.0f, 0.0f, 0.5f,
                    1.0f);
    draw_alpha_mask(PixelRect{rect.x + rect.w - radius, rect.y, radius, rect.h}, disc_, color,
                    0.5f, 0.0f, 1.0f, 1.0f);
}

void QuadRenderer::draw_rounded_rect(const PixelRect& rect, float radius, const Color& color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    radius = std::max(0.0f, std::min(radius, std::min(rect.w, rect.h) * 0.5f));
    if (radius < 0.5f || !build_disc()) {
        draw_solid(rect, color);
        return;
    }
    // Nine pieces that do not overlap -- where two pieces of a translucent
    // fill overlap, the overlap is drawn darker: a column the full height, the
    // two sides between the corners, and a quarter of the disc in each corner.
    draw_solid(PixelRect{rect.x + radius, rect.y, rect.w - radius * 2.0f, rect.h}, color);
    const float side = rect.h - radius * 2.0f;
    if (side > 0.0f) {
        draw_solid(PixelRect{rect.x, rect.y + radius, radius, side}, color);
        draw_solid(PixelRect{rect.x + rect.w - radius, rect.y + radius, radius, side}, color);
    }
    const float right = rect.x + rect.w - radius;
    const float bottom = rect.y + rect.h - radius;
    draw_alpha_mask(PixelRect{rect.x, rect.y, radius, radius}, disc_, color, 0.0f, 0.0f, 0.5f, 0.5f);
    draw_alpha_mask(PixelRect{right, rect.y, radius, radius}, disc_, color, 0.5f, 0.0f, 1.0f, 0.5f);
    draw_alpha_mask(PixelRect{rect.x, bottom, radius, radius}, disc_, color, 0.0f, 0.5f, 0.5f, 1.0f);
    draw_alpha_mask(PixelRect{right, bottom, radius, radius}, disc_, color, 0.5f, 0.5f, 1.0f, 1.0f);
}

}  // namespace ds

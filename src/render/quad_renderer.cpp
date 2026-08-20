#include "render/quad_renderer.hpp"

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
uniform vec4 u_color;
uniform int u_mode;          // 0 solid, 1 screen (XRGB), 2 alpha mask, 3 screen (RGBA)
varying vec2 v_texcoord;
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
    gl::BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    gl::BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 24), nullptr, GL_DYNAMIC_DRAW);
    gl::BindBuffer(GL_ARRAY_BUFFER, 0);

    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return true;
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

void QuadRenderer::begin_frame(int output_width, int output_height) {
    output_width_ = output_width;
    output_height_ = output_height;
    gl::Viewport(0, 0, output_width, output_height);
}

void QuadRenderer::clear(const Color& color) {
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
    }

    gl::BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    gl::BufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(vertices)), vertices);

    const GLuint position = static_cast<GLuint>(attr_position_);
    const GLuint texcoord = static_cast<GLuint>(attr_texcoord_);
    gl::EnableVertexAttribArray(position);
    gl::EnableVertexAttribArray(texcoord);
    gl::VertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                            reinterpret_cast<const void*>(0));
    gl::VertexAttribPointer(texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                            reinterpret_cast<const void*>(2 * sizeof(float)));

    gl::DrawArrays(GL_TRIANGLES, 0, 6);

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
    const Mode mode = texture.channel_order() == ChannelOrder::Rgba ? Mode::ScreenRgba
                                                                    : Mode::Screen;
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

void QuadRenderer::draw_outline(const PixelRect& rect, const Color& color, float thickness) {
    if (thickness <= 0.0f) return;
    const float t = thickness;
    draw_solid(PixelRect{rect.x, rect.y, rect.w, t}, color);
    draw_solid(PixelRect{rect.x, rect.y + rect.h - t, rect.w, t}, color);
    draw_solid(PixelRect{rect.x, rect.y + t, t, rect.h - 2.0f * t}, color);
    draw_solid(PixelRect{rect.x + rect.w - t, rect.y + t, t, rect.h - 2.0f * t}, color);
}

}  // namespace ds

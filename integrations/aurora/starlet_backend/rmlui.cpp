#include "rmlui.hpp"
#include "backend.hpp"

#if STARLET_AURORA_UPSTREAM && defined(AURORA_ENABLE_RMLUI)
#include "aurora_host.hpp"
#include "rmlui/RuntimeTextureProvider.hpp"

#include <GLES3/gl31.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Vertex.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace aurora::starlet_backend::rmlui {

constexpr int kMaxStops = 16;

struct Geometry {
  std::vector<Rml::Vertex> vertices;
  std::vector<int> indices;
  GLsizei index_count = 0;
  GLuint buffers[2] = {}; // GL thread
};

struct Gradient {
  int function = 0; // 0 linear, 1 radial, 2 conic; +3 repeating
  Rml::Vector2f p, v;
  int stops = 0;
  float positions[kMaxStops] = {};
  float colors[kMaxStops][4] = {};
};

struct Texture;

struct Command {
  std::shared_ptr<Geometry> geometry;
  std::shared_ptr<Texture> texture;
  std::shared_ptr<const Gradient> gradient;
  std::shared_ptr<const Rml::Matrix4f> transform;
  Rml::Vector2f translation;
  Rml::Rectanglei scissor;
  bool scissored = false;
  std::array<float, 4> tint{1.f, 1.f, 1.f, 1.f};
};

struct Texture {
  int width = 0, height = 0;
  bool mipmaps = false;
  std::vector<Rml::byte> pixels; // premultiplied RGBA8, until the GL thread uploads it
  GLuint name = 0;               // GL thread
  // A layer saved as a texture: drawn once into it on the GL thread.
  std::vector<Command> layer;
  Rml::Rectanglei region;
  bool layer_pending = false;
};

struct Frame {
  int x = 0, y = 0, width = 0, height = 0;
  std::vector<Command> commands;
  std::vector<std::shared_ptr<Texture>> layers;
  std::vector<std::shared_ptr<Geometry>> dead_geometry;
  std::vector<std::shared_ptr<Texture>> dead_textures;
};

namespace {
Module Log("aurora::starlet::rmlui");

struct Filter {
  std::array<float, 4> tint{1.f, 1.f, 1.f, 1.f};
};

template <typename T> std::shared_ptr<T>& held(std::uintptr_t handle) {
  return *reinterpret_cast<std::shared_ptr<T>*>(handle);
}
template <typename T> std::uintptr_t hold(std::shared_ptr<T> object) {
  return reinterpret_cast<std::uintptr_t>(new std::shared_ptr<T>(std::move(object)));
}

void premultiply(Rml::byte* texels, std::size_t size) {
  for (std::size_t i = 0; i + 3 < size; i += 4) {
    const unsigned alpha = texels[i + 3];
    for (std::size_t c = 0; c < 3; ++c) {
      texels[i + c] = static_cast<Rml::byte>(texels[i + c] * alpha / 255);
    }
  }
}

// Main thread. RmlUi renders through this; each frame's draws go into a Frame the GL thread
// replays.
class Recorder final : public Rml::RenderInterface {
public:
  void begin(int x, int y, int width, int height) {
    frame_ = std::make_shared<Frame>();
    frame_->commands.reserve(last_commands_);
    frame_->x = x;
    frame_->y = y;
    frame_->width = width;
    frame_->height = height;
    layers_.clear();
    scissored_ = false;
    transform_.reset();
  }

  // Nothing drawn, nothing to free and nothing waiting: the GL thread has no work this frame.
  bool idle() const { return !pending_ && dead_geometry_.empty() && dead_textures_.empty(); }

  void publish() {
    auto& frame = *frame_;
    last_commands_ = frame.commands.size();
    frame.dead_geometry = std::move(dead_geometry_);
    frame.dead_textures = std::move(dead_textures_);
    dead_geometry_.clear();
    dead_textures_.clear();
    if (pending_) {
      // The earlier frame was never presented; its layers and releases still have to reach the GL thread.
      frame.layers.insert(frame.layers.begin(), pending_->layers.begin(), pending_->layers.end());
      frame.dead_geometry.insert(frame.dead_geometry.end(), pending_->dead_geometry.begin(),
                                 pending_->dead_geometry.end());
      frame.dead_textures.insert(frame.dead_textures.end(), pending_->dead_textures.begin(),
                                 pending_->dead_textures.end());
    }
    pending_ = std::move(frame_);
  }

  std::shared_ptr<Frame> take() { return std::move(pending_); }
  bool pending_draws() const { return pending_ && (!pending_->commands.empty() || !pending_->layers.empty()); }

  Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override {
    auto geometry = std::make_shared<Geometry>();
    geometry->vertices.assign(vertices.begin(), vertices.end());
    geometry->indices.assign(indices.begin(), indices.end());
    geometry->index_count = static_cast<GLsizei>(indices.size());
    return hold(std::move(geometry));
  }

  void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override {
    push(geometry, translation, texture, nullptr);
  }

  void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override {
    auto* holder = &held<Geometry>(geometry);
    dead_geometry_.push_back(std::move(*holder));
    delete holder;
  }

  Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
    if (const auto runtime = aurora::rmlui::load_runtime_texture(source)) {
      const std::size_t size = std::size_t{runtime->width} * runtime->height * 4;
      if (runtime->width == 0 || runtime->height == 0 || runtime->rgba8.size() < size) {
        Log.error("Runtime texture provider returned an invalid texture for {}", source);
        return {};
      }
      auto texture = std::make_shared<Texture>();
      texture->width = static_cast<int>(runtime->width);
      texture->height = static_cast<int>(runtime->height);
      texture->mipmaps = runtime->generateMipmaps;
      const auto* texels = reinterpret_cast<const Rml::byte*>(runtime->rgba8.data());
      texture->pixels.assign(texels, texels + size);
      if (!runtime->premultipliedAlpha) {
        premultiply(texture->pixels.data(), size);
      }
      dimensions = {texture->width, texture->height};
      return hold(std::move(texture));
    }
    auto texture = load_png(source);
    if (!texture) {
      Log.error("Could not load texture {}", source);
      return {};
    }
    dimensions = {texture->width, texture->height};
    return hold(std::move(texture));
  }

  Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override {
    auto texture = std::make_shared<Texture>();
    texture->width = dimensions.x;
    texture->height = dimensions.y;
    texture->pixels.assign(source.begin(), source.end());
    return hold(std::move(texture));
  }

  void ReleaseTexture(Rml::TextureHandle texture) override {
    auto* holder = &held<Texture>(texture);
    dead_textures_.push_back(std::move(*holder));
    delete holder;
  }

  void EnableScissorRegion(bool enable) override { scissored_ = enable; }
  void SetScissorRegion(Rml::Rectanglei region) override { scissor_ = region; }

  void SetTransform(const Rml::Matrix4f* transform) override {
    transform_ = transform != nullptr ? std::make_shared<const Rml::Matrix4f>(*transform) : nullptr;
  }

  Rml::LayerHandle PushLayer() override {
    layers_.push_back(frame_->commands.size());
    return static_cast<Rml::LayerHandle>(layers_.size());
  }

  void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle, Rml::BlendMode,
                       Rml::Span<const Rml::CompiledFilterHandle> filters) override {
    std::array<float, 4> tint{1.f, 1.f, 1.f, 1.f};
    for (const auto handle : filters) {
      const auto& filter = *reinterpret_cast<const Filter*>(handle);
      for (int c = 0; c < 4; ++c) tint[c] *= filter.tint[c];
    }
    if (tint == std::array<float, 4>{1.f, 1.f, 1.f, 1.f}) {
      return;
    }
    auto& commands = frame_->commands;
    for (std::size_t i = layer_start(source); i < commands.size(); ++i) {
      for (int c = 0; c < 4; ++c) commands[i].tint[c] *= tint[c];
    }
  }

  void PopLayer() override {
    if (!layers_.empty()) layers_.pop_back();
  }

  Rml::TextureHandle SaveLayerAsTexture() override {
    if (!scissored_ || scissor_.Width() <= 0 || scissor_.Height() <= 0) {
      return {};
    }
    auto texture = std::make_shared<Texture>();
    texture->width = scissor_.Width();
    texture->height = scissor_.Height();
    texture->region = scissor_;
    auto& commands = frame_->commands;
    const auto start = static_cast<std::ptrdiff_t>(layers_.empty() ? 0 : layers_.back());
    texture->layer.assign(std::make_move_iterator(commands.begin() + start), std::make_move_iterator(commands.end()));
    commands.erase(commands.begin() + start, commands.end());
    texture->layer_pending = true;
    frame_->layers.push_back(texture);
    return hold(std::move(texture));
  }

  Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override {
    auto* filter = new Filter();
    if (name == "opacity") {
      filter->tint.fill(Rml::Get(parameters, "value", 1.f));
    } else if (name == "brightness") {
      const float value = Rml::Get(parameters, "value", 1.f);
      filter->tint = {value, value, value, 1.f};
    }
    return reinterpret_cast<Rml::CompiledFilterHandle>(filter);
  }

  void ReleaseFilter(Rml::CompiledFilterHandle filter) override { delete reinterpret_cast<Filter*>(filter); }

  Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override {
    auto gradient = std::make_shared<Gradient>();
    const bool repeating = Rml::Get(parameters, "repeating", false);
    if (name == "linear-gradient") {
      gradient->function = 0;
      gradient->p = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
      gradient->v = Rml::Get(parameters, "p1", Rml::Vector2f(0.f)) - gradient->p;
    } else if (name == "radial-gradient") {
      gradient->function = 1;
      gradient->p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
      gradient->v = Rml::Vector2f(1.f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
    } else if (name == "conic-gradient") {
      gradient->function = 2;
      gradient->p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
      const float angle = Rml::Get(parameters, "angle", 0.f);
      gradient->v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
    } else {
      Log.warn("RmlUi shader {} is not supported", name);
      return {};
    }
    gradient->function += repeating ? 3 : 0;
    if (const auto it = parameters.find("color_stop_list");
        it != parameters.end() && it->second.GetType() == Rml::Variant::COLORSTOPLIST) {
      const auto& stops = it->second.GetReference<Rml::ColorStopList>();
      gradient->stops = std::min(static_cast<int>(stops.size()), kMaxStops);
      for (int i = 0; i < gradient->stops; ++i) {
        gradient->positions[i] = stops[i].position.number;
        const auto colour = stops[i].color;
        gradient->colors[i][0] = colour.red / 255.f;
        gradient->colors[i][1] = colour.green / 255.f;
        gradient->colors[i][2] = colour.blue / 255.f;
        gradient->colors[i][3] = colour.alpha / 255.f;
      }
    }
    return hold<const Gradient>(std::move(gradient));
  }

  void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                    Rml::TextureHandle texture) override {
    push(geometry, translation, texture, held<const Gradient>(shader));
  }

  void ReleaseShader(Rml::CompiledShaderHandle shader) override { delete &held<const Gradient>(shader); }

private:
  std::size_t layer_start(Rml::LayerHandle layer) const {
    const auto index = static_cast<std::size_t>(layer);
    return index == 0 || index > layers_.size() ? 0 : layers_[index - 1];
  }

  void push(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture,
            std::shared_ptr<const Gradient> gradient) {
    if (!frame_ || geometry == 0) {
      return;
    }
    Command command;
    command.geometry = held<Geometry>(geometry);
    if (texture != 0) command.texture = held<Texture>(texture);
    command.gradient = std::move(gradient);
    command.transform = transform_;
    command.translation = translation;
    command.scissor = scissor_;
    command.scissored = scissored_;
    frame_->commands.push_back(std::move(command));
  }

  static std::shared_ptr<Texture> load_png(const Rml::String& source) {
    auto* files = Rml::GetFileInterface();
    const Rml::FileHandle file = files != nullptr ? files->Open(source) : Rml::FileHandle{};
    if (file == Rml::FileHandle{}) {
      return nullptr;
    }
    std::vector<Rml::byte> data(files->Length(file));
    const std::size_t read = files->Read(data.data(), data.size(), file);
    files->Close(file);
    SDL_IOStream* stream = SDL_IOFromConstMem(data.data(), read);
    SDL_Surface* loaded = stream != nullptr ? SDL_LoadPNG_IO(stream, true) : nullptr;
    SDL_Surface* rgba = loaded != nullptr ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
    if (loaded != nullptr) SDL_DestroySurface(loaded);
    if (rgba == nullptr) {
      Log.warn("Could not decode image {}: {}", source, SDL_GetError());
      return nullptr;
    }
    auto texture = std::make_shared<Texture>();
    texture->width = rgba->w;
    texture->height = rgba->h;
    const std::size_t row = std::size_t{static_cast<unsigned>(rgba->w)} * 4;
    texture->pixels.resize(row * rgba->h);
    for (int y = 0; y < rgba->h; ++y) {
      std::memcpy(texture->pixels.data() + row * y, static_cast<const Rml::byte*>(rgba->pixels) + std::size_t(rgba->pitch) * y,
                  row);
    }
    SDL_DestroySurface(rgba);
    premultiply(texture->pixels.data(), texture->pixels.size());
    return texture;
  }

  std::shared_ptr<Frame> frame_, pending_;
  std::size_t last_commands_ = 0;
  std::vector<std::size_t> layers_; // command index where each pushed layer starts
  Rml::Rectanglei scissor_;
  bool scissored_ = false;
  std::shared_ptr<const Rml::Matrix4f> transform_;
  std::vector<std::shared_ptr<Geometry>> dead_geometry_;
  std::vector<std::shared_ptr<Texture>> dead_textures_;
};

Recorder& recorder() {
  // RmlUi releases its resources during Rml::Shutdown, which may run after static destruction starts.
  static auto* instance = new Recorder();
  return *instance;
}

// GL thread.
constexpr const char* kVertex = R"(#version 300 es
uniform vec2 u_translate;
uniform mat4 u_transform;
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
out vec2 v_uv;
out vec4 v_color;
void main() {
  v_uv = a_uv;
  v_color = a_color;
  gl_Position = u_transform * vec4(a_position + u_translate, 0.0, 1.0);
}
)";

constexpr const char* kTextured = R"(#version 300 es
precision mediump float;
uniform sampler2D u_texture;
uniform vec4 u_tint;
in vec2 v_uv;
in vec4 v_color;
out vec4 o_color;
void main() { o_color = v_color * texture(u_texture, v_uv) * u_tint; }
)";

// Gradient evaluation follows RmlUi's GL3 renderer (MIT, RmlUi contributors).
constexpr const char* kGradient = R"(#version 300 es
precision highp float;
#define MAX_STOPS 16
uniform int u_function;
uniform vec2 u_p;
uniform vec2 u_v;
uniform vec4 u_stop_colors[MAX_STOPS];
uniform float u_stop_positions[MAX_STOPS];
uniform int u_stops;
uniform vec4 u_tint;
in vec2 v_uv;
in vec4 v_color;
out vec4 o_color;
void main() {
  float t = 0.0;
  int f = u_function % 3;
  if (f == 0) {
    t = dot(u_v, v_uv - u_p) / dot(u_v, u_v);
  } else if (f == 1) {
    t = length(u_v * (v_uv - u_p));
  } else {
    vec2 V = mat2(u_v.x, -u_v.y, u_v.y, u_v.x) * (v_uv - u_p);
    t = 0.5 + atan(-V.x, V.y) / (2.0 * 3.14159265);
  }
  if (u_function >= 3) {
    float t0 = u_stop_positions[0];
    float t1 = u_stop_positions[u_stops - 1];
    t = t0 + mod(t - t0, t1 - t0);
  }
  vec4 color = u_stop_colors[0];
  for (int i = 1; i < u_stops; i++) {
    color = mix(color, u_stop_colors[i], smoothstep(u_stop_positions[i - 1], u_stop_positions[i], t));
  }
  o_color = v_color * color * u_tint;
}
)";

struct Program {
  GLuint name = 0;
  GLint translate = -1, transform = -1, tint = -1;
  GLint function = -1, p = -1, v = -1, stop_colors = -1, stop_positions = -1, stops = -1;
};

struct Gl {
  bool ready = false, failed = false;
  Program textured, gradient;
  GLuint vertex_array = 0, white = 0, framebuffer = 0;
} gl;

GLuint compile(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    Log.error("RmlUi shader failed to compile: {}", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

bool link(Program& program, const char* fragment_source) {
  const GLuint vertex = compile(GL_VERTEX_SHADER, kVertex);
  const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
  if (vertex == 0 || fragment == 0) {
    return false;
  }
  program.name = glCreateProgram();
  glAttachShader(program.name, vertex);
  glAttachShader(program.name, fragment);
  glLinkProgram(program.name);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ok = GL_FALSE;
  glGetProgramiv(program.name, GL_LINK_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[1024] = {};
    glGetProgramInfoLog(program.name, sizeof(log), nullptr, log);
    Log.error("RmlUi program failed to link: {}", log);
    return false;
  }
  const auto at = [&](const char* uniform) { return glGetUniformLocation(program.name, uniform); };
  program.translate = at("u_translate");
  program.transform = at("u_transform");
  program.tint = at("u_tint");
  program.function = at("u_function");
  program.p = at("u_p");
  program.v = at("u_v");
  program.stop_colors = at("u_stop_colors");
  program.stop_positions = at("u_stop_positions");
  program.stops = at("u_stops");
  glUseProgram(program.name);
  glUniform1i(at("u_texture"), 0);
  return true;
}

bool ready() {
  if (gl.ready || gl.failed) {
    return gl.ready;
  }
  gl.failed = true;
  if (!link(gl.textured, kTextured) || !link(gl.gradient, kGradient)) {
    return false;
  }
  glGenVertexArrays(1, &gl.vertex_array);
  glGenFramebuffers(1, &gl.framebuffer);
  glGenTextures(1, &gl.white);
  glBindTexture(GL_TEXTURE_2D, gl.white);
  const std::uint32_t white = 0xffffffff;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl.failed = false;
  gl.ready = true;
  return true;
}

// The GL state the UI changes, put back afterwards for Starlet's renderer and the overlay.
struct SavedState {
  GLint program, vertex_array, array_buffer, active_texture, texture, sampler, framebuffer, unpack_buffer;
  GLint unpack_row_length, unpack_skip_rows, unpack_skip_pixels, unpack_alignment;
  GLint viewport[4], scissor_box[4];
  GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha, blend_eq_rgb, blend_eq_alpha;
  GLboolean blend, scissor, depth, stencil, cull, color_mask[4];

  void save() {
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_alpha);
    blend = glIsEnabled(GL_BLEND);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    depth = glIsEnabled(GL_DEPTH_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
  }

  void restore() const {
    glUseProgram(static_cast<GLuint>(program));
    glBindVertexArray(static_cast<GLuint>(vertex_array));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
    glBindSampler(0, static_cast<GLuint>(sampler));
    glActiveTexture(static_cast<GLenum>(active_texture));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack_buffer));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha);
    glBlendEquationSeparate(blend_eq_rgb, blend_eq_alpha);
    const auto set = [](GLenum capability, GLboolean on) { on ? glEnable(capability) : glDisable(capability); };
    set(GL_BLEND, blend);
    set(GL_SCISSOR_TEST, scissor);
    set(GL_DEPTH_TEST, depth);
    set(GL_STENCIL_TEST, stencil);
    set(GL_CULL_FACE, cull);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
  }
};

void begin_state() {
  glBindVertexArray(gl.vertex_array);
  glActiveTexture(GL_TEXTURE0);
  glBindSampler(0, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void bind_texture(Texture* texture) {
  if (texture == nullptr) {
    glBindTexture(GL_TEXTURE_2D, gl.white);
    return;
  }
  if (texture->name == 0 && !texture->pixels.empty()) {
    glGenTextures(1, &texture->name);
    glBindTexture(GL_TEXTURE_2D, texture->name);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 texture->pixels.data());
    if (texture->mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, texture->mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texture->pixels = {};
    return;
  }
  glBindTexture(GL_TEXTURE_2D, texture->name != 0 ? texture->name : gl.white);
}

void bind_geometry(Geometry& geometry) {
  if (geometry.buffers[0] == 0) {
    glGenBuffers(2, geometry.buffers);
    glBindBuffer(GL_ARRAY_BUFFER, geometry.buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(geometry.vertices.size() * sizeof(Rml::Vertex)),
                 geometry.vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry.buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(geometry.indices.size() * sizeof(int)),
                 geometry.indices.data(), GL_STATIC_DRAW);
    geometry.vertices = {};
    geometry.indices = {};
  } else {
    glBindBuffer(GL_ARRAY_BUFFER, geometry.buffers[0]);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry.buffers[1]);
  }
  constexpr auto stride = static_cast<GLsizei>(sizeof(Rml::Vertex));
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Rml::Vertex, position)));
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(Rml::Vertex, colour)));
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Rml::Vertex, tex_coord)));
}

// Draws commands in content coordinates. scissor_origin maps a content-space scissor rectangle to
// the bound framebuffer's lower-left based coordinates.
template <typename ScissorFn>
void draw_commands(std::vector<Command>& commands, const Rml::Matrix4f& projection, ScissorFn scissor_rect) {
  const Program* bound = nullptr;
  bool scissor_on = false;
  glDisable(GL_SCISSOR_TEST);
  for (auto& command : commands) {
    const Program& program = command.gradient ? gl.gradient : gl.textured;
    if (bound != &program) {
      glUseProgram(program.name);
      bound = &program;
    }
    const Rml::Matrix4f transform = command.transform ? projection * *command.transform : projection;
    glUniformMatrix4fv(program.transform, 1, GL_FALSE, transform.data());
    glUniform2f(program.translate, command.translation.x, command.translation.y);
    glUniform4fv(program.tint, 1, command.tint.data());
    if (command.gradient) {
      const Gradient& g = *command.gradient;
      glUniform1i(program.function, g.function);
      glUniform2f(program.p, g.p.x, g.p.y);
      glUniform2f(program.v, g.v.x, g.v.y);
      glUniform1i(program.stops, std::max(g.stops, 1));
      glUniform4fv(program.stop_colors, kMaxStops, &g.colors[0][0]);
      glUniform1fv(program.stop_positions, kMaxStops, g.positions);
    } else {
      bind_texture(command.texture.get());
    }
    if (command.scissored != scissor_on) {
      command.scissored ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
      scissor_on = command.scissored;
    }
    if (scissor_on) {
      const auto box = scissor_rect(command.scissor);
      glScissor(box[0], box[1], std::max(box[2], 0), std::max(box[3], 0));
    }
    bind_geometry(*command.geometry);
    glDrawElements(GL_TRIANGLES, command.geometry->index_count, GL_UNSIGNED_INT, nullptr);
  }
}

void release(Frame& frame) {
  for (auto& geometry : frame.dead_geometry) {
    if (geometry->buffers[0] != 0) glDeleteBuffers(2, geometry->buffers);
    geometry->buffers[0] = geometry->buffers[1] = 0;
  }
  for (auto& texture : frame.dead_textures) {
    if (texture->name != 0) glDeleteTextures(1, &texture->name);
    texture->name = 0;
  }
  frame.dead_geometry.clear();
  frame.dead_textures.clear();
}
} // namespace

std::shared_ptr<Frame> take_frame() { return recorder().take(); }

bool frame_pending() { return recorder().pending_draws(); }

void prepare(Frame& frame) {
  if (frame.layers.empty() || !ready()) {
    return;
  }
  SavedState saved;
  saved.save();
  begin_state();
  for (auto& texture : frame.layers) {
    if (!texture->layer_pending) continue;
    texture->layer_pending = false;
    glGenTextures(1, &texture->name);
    glBindTexture(GL_TEXTURE_2D, texture->name);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl.framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->name, 0);
    glViewport(0, 0, texture->width, texture->height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    // RmlUi samples texture row 0 as the top, so the region's top maps to the framebuffer's row 0.
    const Rml::Rectanglei region = texture->region;
    const auto projection = Rml::Matrix4f::ProjectOrtho(static_cast<float>(region.Left()), static_cast<float>(region.Right()),
                                                        static_cast<float>(region.Top()), static_cast<float>(region.Bottom()),
                                                        -10000.f, 10000.f);
    draw_commands(texture->layer, projection, [&](Rml::Rectanglei s) {
      return std::array<int, 4>{s.Left() - region.Left(), s.Top() - region.Top(), s.Width(), s.Height()};
    });
    texture->layer = {};
  }
  frame.layers.clear();
  saved.restore();
}

void draw(Frame& frame, int surface_width, int surface_height) {
  if (frame.commands.empty() && frame.dead_geometry.empty() && frame.dead_textures.empty()) {
    return;
  }
  if (!ready()) {
    return;
  }
  SavedState saved;
  saved.save();
  begin_state();
  if (!frame.commands.empty() && frame.width > 0 && frame.height > 0) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    const int bottom = surface_height - frame.y - frame.height;
    glViewport(frame.x, bottom, frame.width, frame.height);
    const auto projection = Rml::Matrix4f::ProjectOrtho(0.f, static_cast<float>(frame.width),
                                                        static_cast<float>(frame.height), 0.f, -10000.f, 10000.f);
    draw_commands(frame.commands, projection, [&](Rml::Rectanglei s) {
      return std::array<int, 4>{frame.x + s.Left(), surface_height - frame.y - s.Bottom(), s.Width(), s.Height()};
    });
  }
  (void)surface_width;
  release(frame);
  saved.restore();
}

void shutdown_gl() {
  if (gl.textured.name != 0) glDeleteProgram(gl.textured.name);
  if (gl.gradient.name != 0) glDeleteProgram(gl.gradient.name);
  if (gl.vertex_array != 0) glDeleteVertexArrays(1, &gl.vertex_array);
  if (gl.framebuffer != 0) glDeleteFramebuffers(1, &gl.framebuffer);
  if (gl.white != 0) glDeleteTextures(1, &gl.white);
  gl = {};
}

Rml::RenderInterface* rmlui_interface() { return enabled() ? &recorder() : nullptr; }

void rmlui_record(Rml::Context& context, int x, int y, int width, int height) {
  Recorder& r = recorder();
  bool visible = false;
  for (int i = 0; i < context.GetNumDocuments() && !visible; ++i) {
    visible = context.GetDocument(i)->IsVisible();
  }
  visible = visible && width > 0 && height > 0;
  if (!visible && r.idle()) {
    return;
  }
  r.begin(x, y, width, height);
  if (visible) {
    context.Render();
  }
  r.publish();
}

} // namespace aurora::starlet_backend::rmlui

namespace aurora::starlet_backend {
Rml::RenderInterface* rmlui_interface() { return rmlui::rmlui_interface(); }
void rmlui_record(Rml::Context& context, int x, int y, int width, int height) {
  rmlui::rmlui_record(context, x, y, width, height);
}
} // namespace aurora::starlet_backend
#endif

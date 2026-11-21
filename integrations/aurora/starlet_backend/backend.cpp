#include "backend.hpp"

#include "aurora_stream.hpp"
#include "host_memory.hpp"
#include "issues.hpp"
#include "overlay.hpp"

#include "aurora_host.hpp"

#include <dolphin/gx/GXCommandList.h>

#if STARLET_AURORA_UPSTREAM
#include <GLES3/gl31.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#endif

#include "starlet/gx/efb_clear.hpp"
#include "starlet/gx/gles/renderer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
#include <vector>

namespace aurora::starlet_backend {
namespace {
Module Log("aurora::starlet");
#if !STARLET_AURORA_UPSTREAM
// Aurora's non-stencil UI passes can leave an invalid cached stencil enum pending.
// Check at ownership boundaries so Starlet still diagnoses only its own GL calls.
void clear_aurora_errors() {
  static std::unordered_set<gl::GLenum> reported;
  for (unsigned i = 0; i < 16; ++i) {
    const auto error = gl::gl.GetError();
    if (error == gl::GL_NO_ERROR) break;
    if (reported.insert(error).second) {
      Log.warn("Shared context had Aurora GL error 0x{:x} before Starlet took control", error);
    }
  }
}
#endif

// Microseconds the GL thread spent in Starlet's GL tasks.
std::atomic<std::uint64_t> gl_task_us{0};

// Counts failures by message, so a frame of rejected draws logs once per kind.
class Failures {
public:
  void note(const char* what, const starlet::gx::State* state = nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++counts_[what];
      ++total_;
    }
    issues::report(what, state);
  }
  std::string summary() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    for (const auto& [what, count] : counts_) {
      text += fmt::format(" [{}x {}]", count, what.substr(0, 80));
    }
    return text;
  }
  std::uint64_t total() const { return total_; }

private:
  std::mutex mutex_;
  std::map<std::string, std::uint64_t> counts_;
  std::atomic<std::uint64_t> total_{0};
};

class DrawSink final : public starlet::gx::CommandSink {
public:
  DrawSink(starlet::gx::GlesRenderer& renderer, Failures& failures) : renderer_(renderer), failures_(failures) {}
  // Rejected draws are skipped and counted; the decoder keeps going.
  bool draw(const starlet::gx::State& state, const starlet::gx::Draw& draw, starlet::gx::MemoryReader& memory) override {
    try {
      renderer_.draw_direct(state, draw, memory, starlet::gx::TextureCmprModel::gx_candidate_v1);
      ++draws;
    } catch (const std::exception& e) {
      failures_.note(e.what(), &state);
      static unsigned traced = 0;
      static const bool trace = [] {
        const char* value = std::getenv("AURORA_STARLET_TRACE");
        return value && *value;
      }();
      static unsigned texgens = 0, stages = 0;
      if (trace && texgens < 4 && std::strstr(e.what(), "reserved texgen")) {
        ++texgens;
        std::fprintf(stderr, "[starlet-trace] texgens %u:", state.xf(0x103f));
        for (unsigned i = 0; i < 8; ++i) std::fprintf(stderr, " %05x/%03x", state.xf(0x1040 + i), state.xf(0x1050 + i));
        std::fprintf(stderr, "\n");
      }
      if (trace && stages < 4 && std::strstr(e.what(), "last TEV stage")) {
        ++stages;
        const unsigned count = ((state.bp(0) >> 10) & 15) + 1;
        std::fprintf(stderr, "[starlet-trace] tev stages %u last color %06x alpha %06x prev color %06x alpha %06x\n", count,
                     state.bp(0xc0 + 2 * (count - 1)), state.bp(0xc1 + 2 * (count - 1)),
                     count > 1 ? state.bp(0xc0 + 2 * (count - 2)) : 0, count > 1 ? state.bp(0xc1 + 2 * (count - 2)) : 0);
      }
      if (trace && traced < 6 && std::strstr(e.what(), "reserved sampler")) {
        ++traced;
        for (unsigned map = 0; map < 8; ++map) {
          const unsigned base = (map < 4 ? 0x80 : 0xa0) + map % 4;
          std::fprintf(stderr, "[starlet-trace] rejected draw map %u words %06x %06x %06x %06x %06x %06x\n", map,
                       state.bp(base), state.bp(base + 4), state.bp(base + 8), state.bp(base + 12), state.bp(base + 16),
                       state.bp(base + 20));
        }
      }
    }
    return true;
  }
  bool bp_write(const starlet::gx::State&, std::uint8_t, std::uint32_t) override { return true; }
  bool invalidate_vertices() override { return true; }
  bool host_command(const starlet::gx::State&, starlet::gx::ByteView input, std::size_t& consumed) override {
    return stream->host_command(input, consumed);
  }
  bool filter_bp(const starlet::gx::State&, std::uint8_t address, std::uint32_t& value) override {
    return stream->filter_bp(address, value);
  }
  AuroraStream* stream = nullptr;
  std::uint64_t draws = 0;

private:
  starlet::gx::GlesRenderer& renderer_;
  Failures& failures_;
};

class Efb final : public EfbTarget {
public:
  Efb(starlet::gx::GlesRenderer& renderer, Failures& failures) : renderer_(renderer), failures_(failures) {}
  void copy_texture(const starlet::gx::TextureCopy& copy, std::uint8_t* destination, std::size_t capacity,
                    std::uint32_t address) override {
    try {
      renderer_.copy_texture_resident(copy, destination, capacity, address);
    } catch (const std::exception& e) {
      failures_.note(e.what());
    }
  }

  void release(std::uint32_t address, std::size_t size) override {
    try {
      renderer_.release_resident(address, size);
    } catch (const std::exception& e) {
      failures_.note(e.what());
    }
  }

  void begin_offscreen() override {
    try {
      renderer_.begin_offscreen();
    } catch (const std::exception& e) {
      failures_.note(e.what());
    }
  }

  void end_offscreen() override {
    try {
      renderer_.end_offscreen();
    } catch (const std::exception& e) {
      failures_.note(e.what());
    }
  }

  void clear(const starlet::gx::EfbClear& plan) override {
    try {
      renderer_.clear_efb(plan);
    } catch (const std::exception& e) {
      failures_.note(e.what());
    }
  }

private:
  starlet::gx::GlesRenderer& renderer_;
  Failures& failures_;
};

starlet::gx::Limits decoder_limits() {
  starlet::gx::Limits limits;
  const char* merge = std::getenv("STARLET_MERGE");
  limits.merge_primitives = !(merge && *merge == '0');
  AuroraStream::configure(limits);
  return limits;
}

std::string data_directory() {
  // Compiled programs and the program list persist here between runs.
  std::filesystem::path path = g_config.cachePath ? g_config.cachePath : ".";
  path /= "starlet";
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path.string();
}

// Pages written through the data-cache hooks this frame, and texture pointers seen rewritten
// while in use; both decide which texture loads are copied (texture_data).
class TextureWrites {
public:
  static constexpr unsigned page_bits = 12;
  static constexpr std::size_t pool_bytes = 4 << 20;

  TextureWrites() : pool_(static_cast<std::uint8_t*>(std::aligned_alloc(32, pool_bytes))) {}
  const std::uint8_t* pool() const { return pool_; }

  void note(const void* pointer, std::size_t size) {
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer) >> page_bits;
    const auto end = (reinterpret_cast<std::uintptr_t>(pointer) + size - 1) >> page_bits;
    std::lock_guard<std::mutex> lock(mutex_);
    ++notes;
    for (auto page = begin; page <= end && page - begin < 4096; ++page) pages_.insert(page);
  }

  const void* load(const void* data, std::size_t size, const HostMemory& memory) {
    ++loads;
    if (!data || !size || memory.copy_destination(data)) return data;
    const auto at = reinterpret_cast<std::uintptr_t>(data);
    bool written = volatile_.count(data) != 0;
    if (!written) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto page = at >> page_bits; page <= (at + size - 1) >> page_bits && !written; ++page) written = pages_.count(page);
    }
    if (!written) return data;
    volatile_.insert(data);
    const std::size_t offset = (used_ + 31) & ~std::size_t{31};
    if (size > pool_bytes - offset) {
      ++overflows;
      return data;
    }
    std::memcpy(pool_ + offset, data, size);
    used_ = offset + size;
    ++copies;
    copied_bytes += size;
    return pool_ + offset;
  }

  // Main thread, after the frame's FIFO drained: its copies have been decoded.
  void end_frame() {
    used_ = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    pages_.clear();
  }

  std::uint64_t copies = 0, copied_bytes = 0, overflows = 0, notes = 0, loads = 0;

private:
  std::uint8_t* pool_;
  std::size_t used_ = 0;
  std::mutex mutex_;
  std::unordered_set<std::uintptr_t> pages_;
  std::unordered_set<const void*> volatile_;
};

TextureWrites& texture_writes() {
  static TextureWrites writes;
  return writes;
}

struct Backend {
  starlet::gx::EfbLayout efb;
  int frame_width; // The game's frame; the EFB is always 640 wide.
  std::unique_ptr<starlet::gx::GlesRenderer> renderer;
  Failures failures;
  HostMemory memory;
  DrawSink sink;
  starlet::gx::Decoder decoder;
  Efb efb_target;
  AuroraStream stream;
  bool frame_started = false;
  // The frame in progress was marked with aurora_starlet_skip_frame.
  bool skipping = false;
  std::atomic<bool> drawn{false};
  std::atomic<bool> last_skipped{false};
  std::atomic<std::uint64_t> presented{0};
  std::uint64_t skipped_frames = 0, last_presented = 0, last_skipped_frames = 0;
  bool reported = false;
  std::uint64_t frames = 0, feed_us = 0;
  starlet::gx::GlesDirectStats last_direct;
  std::uint64_t last_frames = 0, last_feed_us = 0, last_gl_us = 0, last_draws = 0;
  std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();

  Backend(starlet::gx::EfbLayout layout, int width, std::unique_ptr<starlet::gx::GlesRenderer> created)
  : efb(layout)
  , frame_width(width)
  , renderer(std::move(created))
  , memory(MEM1Start, static_cast<std::size_t>(static_cast<std::uint8_t*>(MEM1End) - static_cast<std::uint8_t*>(MEM1Start)))
  , sink(*renderer, failures)
  , decoder(memory, sink, decoder_limits(), layout)
  , efb_target(*renderer, failures)
  , stream(memory, decoder, efb_target) {
    sink.stream = &stream;
    memory.map_region(texture_writes().pool(), TextureWrites::pool_bytes);
    memory.set_snapshot_pool(texture_writes().pool(), TextureWrites::pool_bytes);
  }

  // Aurora's GXCopyDisp does not run; Aurora clears colour, alpha and depth to the copy-clear
  // values when a frame begins, whatever the update masks, and the game relies on it. Nothing
  // draws or reads the EFB's columns right of the frame, so the clear covers them too.
  void clear_efb() {
    try {
      auto clear = starlet::gx::plan_copy_clear(decoder.state(), {0, 0, starlet::gx::EfbLayout::width, efb.height});
      clear.color_write = true;
      clear.alpha_write = clear.rgba6;
      clear.depth_write = true;
      renderer->clear_efb(clear);
    } catch (const std::exception& e) {
      failures.note(e.what());
    }
  }

  StreamStats last_stream{};
  void report() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report < std::chrono::seconds(5)) {
      return;
    }
    const double seconds = std::chrono::duration<double>(now - last_report).count();
    last_report = now;
    const auto& s = stream.stats();
    const auto r = renderer->stats();
    const auto& d = r.direct;
    const double n = std::max<double>(1, double(frames - last_frames));
    const auto gl_us = gl_task_us.load();
    Log.info("[starlet] per frame: gx_draws={:.0f} recorded={:.0f} batched={:.0f} zero_area={:.0f} segments={:.1f} "
             "submissions={:.1f} geometry_hits={:.0f} misses={:.0f} validated_kb={:.0f} texture_uploads={:.1f} "
             "texture_hash_kb={:.0f} programs_missed={:.1f} unready={:.0f} culled={:.0f} feed_ms={:.1f} gl_ms={:.1f} resident_copies={:.2f} resident_hits={:.2f} materialized={:.2f}",
             (sink.draws - last_draws) / n, (d.draws - last_direct.draws) / n, (d.batched_draws - last_direct.batched_draws) / n,
             (d.zero_area - last_direct.zero_area) / n, (d.segments - last_direct.segments) / n,
             (d.submissions - last_direct.submissions) / n, (d.geometry_hits - last_direct.geometry_hits) / n,
             (d.geometry_misses - last_direct.geometry_misses) / n,
             (d.geometry_validated_bytes - last_direct.geometry_validated_bytes) / n / 1024,
             (d.texture_uploads - last_direct.texture_uploads) / n, (d.texture_hash_bytes - last_direct.texture_hash_bytes) / n / 1024,
             (d.program_misses - last_direct.program_misses) / n, (d.unready_draws - last_direct.unready_draws) / n,
             (d.culled_draws - last_direct.culled_draws) / n, (feed_us - last_feed_us) / n / 1000,
             (gl_us - last_gl_us) / n / 1000, (d.resident_copies - last_direct.resident_copies) / n,
             (d.resident_hits - last_direct.resident_hits) / n, (d.materialized - last_direct.materialized) / n);
    const auto shown = presented.load();
    Log.info("[starlet] rates: ticks={:.1f}/s presented={:.1f}/s skipped={:.1f}/s rendered_anyway={} dropped_records={}",
             n / seconds, (shown - last_presented) / seconds, (skipped_frames - last_skipped_frames) / seconds,
             r.skipped_frames_rendered, d.dropped_draws);
    last_presented = shown;
    last_skipped_frames = skipped_frames;
    Log.info("[starlet] stream per frame: aurora={:.0f} decoder_feeds={:.0f} drains={:.1f}",
             (s.aurora_commands - last_stream.aurora_commands) / n, (s.decoder_feeds - last_stream.decoder_feeds) / n,
             (s.drains - last_stream.drains) / n);
    last_stream = s;
    last_direct = d; last_frames = frames; last_feed_us = feed_us; last_gl_us = gl_us; last_draws = sink.draws;
    Log.info("[starlet] totals: frames={} rejected={} copies={} expanded={} ignored={} unmapped={} repaired={} "
             "failed_copies={} resized_copies={} texture_snapshots={} ({} KB, {} overflows, {} notes, {} loads) shaders={}{}",
             frames, failures.total(), s.copies, s.expanded_draws, s.ignored, s.unmapped, s.repaired, s.failed_copies,
             s.resized_copies, texture_writes().copies, texture_writes().copied_bytes / 1024, texture_writes().overflows,
             texture_writes().notes, texture_writes().loads, r.shader_compiles, failures.summary());
  }
};

std::atomic<Backend*> instance{nullptr};
std::atomic<bool> skip_requested{false};
std::once_flag created;

#if STARLET_AURORA_UPSTREAM
// The thread that owns the GL context from initialize() on. SDL creates the context on the
// main thread; the renderer, its submission work and the swap all run here, so Aurora's main
// thread never issues a GL call.
class GlThread {
public:
  bool start(SDL_Window* window, SDL_GLContext context) {
    std::promise<bool> ready;
    auto result = ready.get_future();
    thread_ = std::thread([this, window, context, &ready] {
      const bool bound = SDL_GL_MakeCurrent(window, context);
      if (!bound) {
        Log.error("Starlet could not bind the OpenGL ES context to its thread: {}", SDL_GetError());
      }
      ready.set_value(bound);
      if (!bound) {
        return;
      }
      loop();
      SDL_GL_MakeCurrent(window, nullptr);
    });
    if (!result.get()) {
      thread_.join();
      return false;
    }
    return true;
  }

  // Runs task on the GL thread after the tasks posted before it.
  void post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    ready_.notify_one();
  }

  // post, and wait for it.
  void run(std::function<void()> task) {
    std::promise<void> done;
    post([&] {
      task();
      done.set_value();
    });
    done.get_future().get();
  }

  void stop() {
    if (!thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_one();
    thread_.join();
  }

private:
  void loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      try {
        task();
      } catch (const std::exception& e) {
        Log.error("Starlet GL task failed: {}", e.what());
      }
    }
  }

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_ = false;
};

struct GlContext {
  SDL_Window* window = nullptr;
  SDL_GLContext context = nullptr;
  GlThread thread;
  std::mutex mutex;
  std::condition_variable idle;
  unsigned presents_pending = 0; // Frames posted to the GL thread and not yet swapped.
};

GlContext& gl_context() {
  static GlContext context;
  return context;
}

void post_on_gl(std::function<void()> task) { gl_context().thread.post(std::move(task)); }
void run_on_gl(std::function<void()> task) { gl_context().thread.run(std::move(task)); }
#else
void post_on_gl(std::function<void()> task) { gfx::render_worker::enqueue_work(std::move(task)); }
void run_on_gl(std::function<void()> task) {
  gfx::render_worker::enqueue_work(std::move(task));
  gfx::render_worker::synchronize();
}
#endif

Backend* backend() {
  std::call_once(created, [] {
    const auto size = gx::logical_fb_size();
    starlet::gx::EfbLayout layout;
    layout.height = static_cast<int>(size.y);
    if (!layout.valid() || size.x == 0 || size.x > starlet::gx::EfbLayout::width) {
      Log.error("Starlet needs an EFB frame of at most 640x528; the game uses {}x{}", size.x, size.y);
      return;
    }
    starlet::gx::GlesLimits limits;
    limits.efb = layout;
    limits.direct.shading = starlet::gx::DirectShading::fast;
    // Aurora's own renderer clips at the viewport; game scissors may reach past it.
    limits.direct.guard_band = starlet::gx::GuardBand::clip_to_viewport;
    limits.direct.program_cache = data_directory();
    limits.direct.program_lists = {limits.direct.program_cache + "/programs.list"};
    std::unique_ptr<starlet::gx::GlesRenderer> renderer;
    std::string error;
    run_on_gl([&] {
      try {
        renderer = std::make_unique<starlet::gx::GlesRenderer>(limits);
        renderer->start_submission_on([](std::function<void()> task) {
          post_on_gl([task = std::move(task)] {
#if !STARLET_AURORA_UPSTREAM
            clear_aurora_errors();
#endif
            const auto start = std::chrono::steady_clock::now();
            task();
            gl_task_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
          });
        });
      } catch (const std::exception& e) {
        error = e.what();
        renderer.reset();
      }
    });
    if (!renderer) {
      Log.error("Starlet renderer failed to start: {}", error);
      return;
    }
    renderer->adopt_thread();
    instance.store(new Backend(layout, static_cast<int>(size.x), std::move(renderer)));
    Log.info("Starlet draws the GX stream: frame {}x{}, game memory {} at {} bytes", size.x, layout.height,
             static_cast<const void*>(MEM1Start),
             static_cast<std::size_t>(static_cast<std::uint8_t*>(MEM1End) - static_cast<std::uint8_t*>(MEM1Start)));
  });
  return instance.load();
}
} // namespace

bool enabled() noexcept {
  static const bool on = [] {
    const char* value = std::getenv("AURORA_RENDERER");
    return value && std::strcmp(value, "starlet") == 0;
  }();
  return on;
}

void process(const std::uint8_t* data, std::size_t size) {
  auto* b = backend();
  if (!b) {
    return;
  }
  if (!b->frame_started) {
    b->clear_efb();
    b->frame_started = true;
    if (skip_requested.load()) {
      try {
        b->renderer->skip_frame();
        b->skipping = true;
      } catch (const std::exception& e) {
        b->failures.note(e.what());
      }
    }
  }
  const auto start = std::chrono::steady_clock::now();
  b->memory.begin_feed();
  b->stream.feed(data, size);
  b->feed_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  if (!b->stream.failure().empty() && !b->reported) {
    Log.error("Starlet stopped following the GX stream: {}", b->stream.failure());
    b->reported = true;
  }
}

#if STARLET_AURORA_UPSTREAM
void set_gl_attributes() {
  // SDL needs these before the window exists. The EFB is the only depth and colour Starlet
  // renders; the window's framebuffer only receives the presented frame.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

bool initialize() {
  auto& g = gl_context();
  g.window = window::get_sdl_window();
  if (!g.window) {
    Log.error("Starlet needs an SDL window to draw into");
    return false;
  }
  g.context = SDL_GL_CreateContext(g.window);
  if (!g.context) {
    Log.error("Starlet could not create an OpenGL ES 3.1 context: {}", SDL_GetError());
    return false;
  }
  // The game paces its own frames; the swap should not wait for the display unless asked to.
  int interval = 0;
  if (const char* value = std::getenv("AURORA_STARLET_VSYNC"); value && *value) {
    interval = std::atoi(value);
  }
  if (!SDL_GL_SetSwapInterval(interval)) {
    Log.warn("Starlet could not set swap interval {}: {}", interval, SDL_GetError());
  }
  // One thread owns the context at a time: the GL thread takes it from here.
  if (!SDL_GL_MakeCurrent(g.window, nullptr)) {
    Log.warn("Starlet could not release the OpenGL ES context: {}", SDL_GetError());
  }
  if (!g.thread.start(g.window, g.context)) {
    SDL_GL_DestroyContext(g.context);
    g.context = nullptr;
    return false;
  }
  g.thread.run([] {
    const auto text = [](GLenum name) {
      const auto* value = glGetString(name);
      return value ? reinterpret_cast<const char*>(value) : "?";
    };
    Log.info("OpenGL ES: {} | {} | {}", text(GL_VENDOR), text(GL_RENDERER), text(GL_VERSION));
  });
  return true;
}

// Main thread: hands the finished frame to the GL thread, which draws it into the window's
// framebuffer at 4:3 and swaps. At most two frames wait there, so the game cannot run ahead.
static void present_frame(Backend& b) {
  auto& g = gl_context();
  if (!b.drawn) {
    return;
  }
  int surface_width = 0, surface_height = 0;
  if (!SDL_GetWindowSizeInPixels(g.window, &surface_width, &surface_height) || surface_width <= 0 || surface_height <= 0) {
    return;
  }
  const double content_width = b.frame_width, content_height = b.efb.height;
  int width = surface_width;
  int height = std::min(surface_height, std::max(1, static_cast<int>(std::lround(width * content_height / content_width))));
  if (height == surface_height) {
    width = std::min(surface_width, std::max(1, static_cast<int>(std::lround(height * content_width / content_height))));
  }
  const int left = (surface_width - width) / 2, bottom = (surface_height - height) / 2;
  {
    std::unique_lock<std::mutex> lock(g.mutex);
    g.idle.wait(lock, [&] { return g.presents_pending < 2; });
    ++g.presents_pending;
  }
  g.thread.post([&b, &g, left, bottom, width, height, surface_width, surface_height] {
    try {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDisable(GL_SCISSOR_TEST);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearColor(0.f, 0.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
      b.renderer->present_into(0, b.frame_width, b.efb.height, left, bottom, width, height);
      overlay::frame(surface_width, surface_height);
      if (!SDL_GL_SwapWindow(g.window)) {
        static bool reported = false;
        if (!std::exchange(reported, true)) {
          Log.error("Starlet could not swap the window: {}", SDL_GetError());
        }
      }
      ++b.presented;
    } catch (const std::exception& e) {
      b.failures.note(e.what());
    }
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      --g.presents_pending;
    }
    g.idle.notify_all();
  });
}
#endif

void end_frame() {
  auto* b = instance.load();
  if (!b) {
    return;
  }
  const bool skipped = std::exchange(b->skipping, false) || skip_requested.load();
  skip_requested = false;
  try {
    b->renderer->end_frame();
    if (!skipped) {
      b->drawn = true;
    }
  } catch (const std::exception& e) {
    b->failures.note(e.what());
  }
  b->last_skipped = skipped;
  b->skipped_frames += skipped;
#if STARLET_AURORA_UPSTREAM
  if (!skipped) {
    present_frame(*b);
  }
#else
  // Aurora's own passes this frame (UI, overlays) run after Starlet's work on the same
  // context and must not trust their state shadow.
  gfx::render_worker::enqueue_work([] {
    gl::reset_state_cache();
    gl::invalidate_texture_bindings();
  });
#endif
  texture_writes().end_frame();
  b->frame_started = false;
  ++b->frames;
  issues::set_frame(b->frames);
  b->report();
}

#if !STARLET_AURORA_UPSTREAM
bool present(std::uint32_t target, std::uint32_t surface_width, std::uint32_t surface_height) {
  auto* b = instance.load();
  if (!b || !b->drawn || b->last_skipped) {
    return false;
  }
  clear_aurora_errors();
  gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, target);
  gl::gl.Disable(gl::GL_SCISSOR_TEST);
  gl::gl.ColorMask(gl::GL_TRUE, gl::GL_TRUE, gl::GL_TRUE, gl::GL_TRUE);
  gl::gl.ClearColor(0.f, 0.f, 0.f, 1.f);
  gl::gl.Clear(gl::GL_COLOR_BUFFER_BIT);
  const auto viewport = webgpu::calculate_present_viewport(surface_width, surface_height,
                                                           static_cast<std::uint32_t>(b->frame_width),
                                                           static_cast<std::uint32_t>(b->efb.height));
  try {
    b->renderer->present_into(target, b->frame_width, b->efb.height, static_cast<int>(viewport.left),
                              static_cast<int>(viewport.top), static_cast<int>(viewport.width),
                              static_cast<int>(viewport.height));
  } catch (const std::exception& e) {
    b->failures.note(e.what());
  }
  gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, target);
  gl::reset_state_cache();
  gl::invalidate_texture_bindings();
  ++b->presented;
  return true;
}
#endif

void shutdown() {
  auto* b = instance.exchange(nullptr);
#if STARLET_AURORA_UPSTREAM
  auto& g = gl_context();
  if (b) {
    try {
      b->renderer->stop_submission();
    } catch (const std::exception&) {
    }
    // GL objects die on the thread that owns the context.
    g.thread.run([b] {
      b->renderer->adopt_thread();
      overlay::shutdown();
      delete b;
    });
  }
  g.thread.stop();
  if (g.context) {
    SDL_GL_DestroyContext(g.context);
    g.context = nullptr;
  }
#else
  if (!b) {
    return;
  }
  try {
    b->renderer->stop_submission();
  } catch (const std::exception&) {
  }
  // GL objects die on the thread that owns the context.
  gfx::render_worker::enqueue_work([b] {
    b->renderer->adopt_thread();
    delete b;
  });
  gfx::render_worker::synchronize();
#endif
}

#if STARLET_AURORA_UPSTREAM
bool overlay_event(const SDL_Event& event) { return overlay::event(event); }
#endif

void note_write(const void* pointer, std::size_t size) {
  if (!enabled() || !pointer || !size) {
    return;
  }
  texture_writes().note(pointer, size);
  if (auto* b = instance.load()) {
    b->memory.note_write(pointer, size);
  }
}

const void* texture_data(const void* data, std::size_t size) {
  auto* b = instance.load();
  return b ? texture_writes().load(data, size, b->memory) : data;
}

void note_display_list(const void* data, std::size_t size) {
  auto* b = instance.load();
  if (!b || !size || !b->memory.in_arena(data, size)) {
    return;
  }
  gx::fifo::write_u8(GX_AURORA);
  gx::fifo::write_u16(list_source_command);
  gx::fifo::write_u64(reinterpret_cast<std::uintptr_t>(data));
  gx::fifo::write_u32(static_cast<std::uint32_t>(size));
  gx::fifo::write_u64(b->memory.stamp(data, size));
}

} // namespace aurora::starlet_backend

extern "C" void aurora_starlet_note_write(const void* pointer, unsigned size) {
  aurora::starlet_backend::note_write(pointer, size);
}

extern "C" void aurora_starlet_skip_frame(void) {
  if (aurora::starlet_backend::enabled()) {
    aurora::starlet_backend::skip_requested = true;
  }
}

extern "C" int aurora_starlet_overlay_option(const aurora_starlet_option* option) {
  return option != nullptr && aurora::starlet_backend::overlay::add_option(*option) ? 1 : 0;
}

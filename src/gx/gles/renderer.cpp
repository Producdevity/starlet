#include "starlet/gx/gles/renderer.hpp"
#include "starlet/gx/efb_format.hpp"

#include <GLES3/gl31.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <tuple>

namespace starlet::gx {
namespace {
void gl_check(const char* operation) {
    const auto error = glGetError();
    if (error != GL_NO_ERROR) throw std::runtime_error(std::string(operation) + ": GLES error " + std::to_string(error));
}
GLuint compile_shader(GLenum kind, const std::string& source) {
    const auto shader = glCreateShader(kind);
    if (!shader) throw std::runtime_error("GLES shader allocation failed");
    const char* text = source.c_str(); glShaderSource(shader, 1, &text, nullptr); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]{}; glGetShaderInfoLog(shader, sizeof log, nullptr, log); glDeleteShader(shader);
        throw std::runtime_error(std::string("GLES shader: ") + log);
    }
    return shader;
}
GLuint link_program(const GlesDrawPipeline& pipeline) {
    GLuint vertex = 0, fragment = 0, program = 0;
    try {
        vertex = compile_shader(GL_VERTEX_SHADER, pipeline.vertex);
        fragment = compile_shader(GL_FRAGMENT_SHADER, pipeline.fragment);
        program = glCreateProgram();
        if (!program) throw std::runtime_error("GLES program allocation failed");
        glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
        GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096]{}; glGetProgramInfoLog(program, sizeof log, nullptr, log);
            throw std::runtime_error(std::string("GLES link: ") + log);
        }
        glDeleteShader(vertex); glDeleteShader(fragment); return program;
    } catch (...) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        if (program) glDeleteProgram(program);
        throw;
    }
}
GLuint link_compute(const std::string& source) {
    GLuint shader = 0, program = 0;
    try {
        shader = compile_shader(GL_COMPUTE_SHADER, source);
        program = glCreateProgram();
        if (!program) throw std::runtime_error("GLES program allocation failed");
        glAttachShader(program, shader); glLinkProgram(program);
        GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096]{}; glGetProgramInfoLog(program, sizeof log, nullptr, log);
            throw std::runtime_error(std::string("GLES link: ") + log);
        }
        glDeleteShader(shader); return program;
    } catch (...) {
        if (shader) glDeleteShader(shader);
        if (program) glDeleteProgram(program);
        throw;
    }
}
struct GpuVertex {
    std::array<float, 3> position;
    std::array<float, 9> normal;
    std::array<std::array<std::uint8_t, 4>, 2> colors;
    std::array<std::array<float, 2>, 8> texcoords;
    std::array<std::uint32_t, 3> matrices{};
};
static_assert(std::is_standard_layout<GpuVertex>::value && sizeof(GpuVertex) == 132);
GLenum wrap(TextureWrap value) {
    switch (value) {
    case TextureWrap::clamp: return GL_CLAMP_TO_EDGE;
    case TextureWrap::repeat: return GL_REPEAT;
    case TextureWrap::mirror: return GL_MIRRORED_REPEAT;
    }
    throw std::runtime_error("invalid admitted texture wrap");
}
GLenum factor(unsigned value, bool source) {
    const GLenum factors[] = {GL_ZERO, GL_ONE, static_cast<GLenum>(source ? GL_DST_COLOR : GL_SRC_COLOR),
        static_cast<GLenum>(source ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR), GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA};
    if (value >= 6) throw std::runtime_error("invalid admitted blend factor");
    return factors[value];
}
std::size_t image_bytes(const TextureSnapshot& image) {
    std::size_t bytes=0;
    for (const auto& level:image.levels()) bytes+=level.rgba.size();
    return bytes;
}
GLenum min_filter(TextureMinFilter filter) {
    switch (filter) {
    case TextureMinFilter::nearest: return GL_NEAREST;
    case TextureMinFilter::nearest_mip_nearest: return GL_NEAREST_MIPMAP_NEAREST;
    case TextureMinFilter::nearest_mip_linear: return GL_NEAREST_MIPMAP_LINEAR;
    case TextureMinFilter::linear: return GL_LINEAR;
    case TextureMinFilter::linear_mip_nearest: return GL_LINEAR_MIPMAP_NEAREST;
    case TextureMinFilter::linear_mip_linear: return GL_LINEAR_MIPMAP_LINEAR;
    }
    throw std::runtime_error("invalid texture minification filter");
}
bool same_image(const TextureSnapshot& a, const TextureSnapshot& b) {
    if (&a.levels() == &b.levels()) return true;
    if (a.levels().size()!=b.levels().size()) return false;
    for (unsigned i=0;i<a.levels().size();++i) {
        const auto& x=a.levels()[i]; const auto& y=b.levels()[i];
        if (x.width!=y.width || x.height!=y.height || x.rgba!=y.rgba) return false;
    }
    return true;
}
bool same_texture(const TextureSnapshot& a, const TextureSnapshot& b) {
    const auto& x=a.descriptor().sampler;const auto& y=b.descriptor().sampler;
    return x.wrap_s==y.wrap_s && x.wrap_t==y.wrap_t && x.min_filter==y.min_filter &&
           x.linear_magnification==y.linear_magnification && same_image(a,b);
}

void append_vertices(std::vector<GpuVertex>& destination, const VertexSnapshot& source) {
    for (const auto& v : source.vertices()) {
        GpuVertex gpu{v.position, v.normal, v.colors, v.texcoords, {}};
        for (unsigned i=0;i<9;++i) gpu.matrices[i/3] |= std::uint32_t(v.matrix_indices[i]) << ((i%3)*8);
        gpu.matrices[0] |= std::uint32_t(transform_normal_row(v.matrix_indices[0] & 63u)) << 24;
        destination.push_back(gpu);
    }
}
bool same_rect(const RasterRect& a,const RasterRect& b) {
    return std::tie(a.x,a.y,a.width,a.height)==std::tie(b.x,b.y,b.width,b.height);
}
bool same_raster(const GlesRasterPlan& a,const GlesRasterPlan& b) {
    return same_rect(a.viewport,b.viewport) && same_rect(a.scissor,b.scissor) &&
        !std::memcmp(a.depth_range.data(),b.depth_range.data(),sizeof(a.depth_range)) &&
        std::tie(a.blend,a.color_write,a.depth_test,a.depth_write,a.early_depth,a.depth_compare,a.source_factor,a.destination_factor,a.cull)==
        std::tie(b.blend,b.color_write,b.depth_test,b.depth_write,b.early_depth,b.depth_compare,b.source_factor,b.destination_factor,b.cull);
}
bool batch_compatible(const DrawPacket& a,const GlesDrawPipeline& x,const DrawPacket& b,const GlesDrawPipeline& y) {
    if (x.key!=y.key || !same_raster(x.raster,y.raster) ||
        std::tie(x.rgba6_copy,x.full_replacement,x.alpha_write)!=std::tie(y.rgba6_copy,y.full_replacement,y.alpha_write) ||
        x.pixel_controls!=y.pixel_controls ||
        std::memcmp(x.coordinate_scale.data(),y.coordinate_scale.data(),sizeof(x.coordinate_scale)) ||
        std::memcmp(x.image_size.data(),y.image_size.data(),sizeof(x.image_size)) ||
        std::memcmp(x.indirect_scale.data(),y.indirect_scale.data(),sizeof(x.indirect_scale)) ||
        std::memcmp(x.sampler_lod.data(),y.sampler_lod.data(),sizeof(x.sampler_lod)) ||
        std::memcmp(&a.transform->uniforms,&b.transform->uniforms,sizeof(TransformUniforms)) ||
        a.tev->uniforms.registers!=b.tev->uniforms.registers || a.tev->uniforms.konst!=b.tev->uniforms.konst ||
        a.tev->uniforms.alpha_reference!=b.tev->uniforms.alpha_reference ||
        a.tev->uniforms.z_texture_bias!=b.tev->uniforms.z_texture_bias) return false;
    for (unsigned i=0;i<3;++i) if(a.indirect->matrices[i].rows!=b.indirect->matrices[i].rows) return false;
    for (unsigned i=0;i<8;++i) {
        if(bool(a.textures[i])!=bool(b.textures[i])) return false;
        if(a.textures[i] && !same_texture(*a.textures[i],*b.textures[i])) return false;
    }
    return true;
}

class DrawPreparation {
    struct Job {
        const DrawPacket* packets;
        FramebufferFetch fetch;
        std::vector<GlesDrawPipeline>& plans;
        std::vector<std::exception_ptr>& errors;
        std::atomic<std::size_t> next{0};
    };
    std::mutex mutex_;
    std::condition_variable changed_, completed_;
    std::vector<std::thread> workers_;
    Job* job_ = nullptr;
    std::uint64_t generation_ = 0;
    unsigned remaining_ = 0;
    bool stopping_ = false;
    static void run(Job& job) noexcept {
        for (;;) {
            const auto index = job.next.fetch_add(1, std::memory_order_relaxed);
            if (index >= job.plans.size()) return;
            try {
                auto result = plan_gles_draw(job.packets[index], job.fetch, false);
                if (!result) throw std::runtime_error(result.status.message);
                job.plans[index] = std::move(result.pipeline);
            } catch (...) { job.errors[index] = std::current_exception(); }
        }
    }
    void stop() noexcept {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        changed_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }
public:
    DrawPreparation() {
        const auto cores = std::thread::hardware_concurrency();
        const auto count = cores > 1 ? std::min(2u, cores - 1) : 0;
        try {
            for (unsigned i = 0; i < count; ++i) workers_.emplace_back([this] {
                std::uint64_t seen = 0;
                std::unique_lock<std::mutex> lock(mutex_);
                for (;;) {
                    changed_.wait(lock, [&] { return stopping_ || generation_ != seen; });
                    if (stopping_) return;
                    seen = generation_;
                    auto* job = job_;
                    lock.unlock(); run(*job); lock.lock();
                    if (!--remaining_) completed_.notify_one();
                }
            });
        } catch (...) { stop(); throw; }
    }
    ~DrawPreparation() { stop(); }
    std::vector<GlesDrawPipeline> plan(const DrawPacket* packets, std::size_t count, FramebufferFetch fetch) {
        std::vector<GlesDrawPipeline> plans(count);
        std::vector<std::exception_ptr> errors(count);
        Job job{packets, fetch, plans, errors};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job; remaining_ = static_cast<unsigned>(workers_.size()); ++generation_;
        }
        changed_.notify_all();
        run(job);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            completed_.wait(lock, [&] { return remaining_ == 0; });
            job_ = nullptr;
        }
        for (const auto& error : errors) if (error) std::rethrow_exception(error);
        return plans;
    }
};

template<class T> struct UniformValue {
    GLint location = -1;
    T value{};
    bool initialized = false;
    bool update(const T& next) {
        if (location < 0 || (initialized && !std::memcmp(value.data(), next.data(), sizeof(T)))) return false;
        value = next; initialized = true; return true;
    }
};
}

struct GlesRenderer::Impl {
    struct Program {
        GLuint name = 0;
        std::vector<std::uint8_t> key;
        std::array<UniformValue<TevSignedColor>, 4> registers{};
        std::array<UniformValue<TevColor>, 4> konst{};
        std::array<UniformValue<std::array<float,2>>, 16> scales{}, image_sizes{};
        std::array<UniformValue<std::array<float,4>>, 4> indirect_scales{};
        std::array<UniformValue<std::array<std::int32_t,4>>, 6> indirect_rows{};
        std::array<UniformValue<std::array<float,4>>, 8> sampler_lod{};
        UniformValue<std::array<std::uint8_t,2>> alpha;
        UniformValue<std::array<std::uint32_t,4>> pixel;
        UniformValue<std::array<std::uint32_t,1>> z_bias;
        std::uint64_t used = 0;
    };
    std::thread::id owner = std::this_thread::get_id();
    GlesLimits limits;
    const int efb_width = EfbLayout::width, efb_height = limits.efb.height, efb_strip_height = limits.efb.strip_height();
    // GLSL constant uEfb: the EFB frame size in pixels.
    // Internal shaders address EFB rows from the top, as GX does, through GX_EFB_TOP.
    GLuint link_efb_program(GlesDrawPipeline pipeline) const {
        const auto end = pipeline.fragment.find('\n') + 1;
        pipeline.fragment.insert(end, "#define GX_EFB_TOP " + std::to_string(efb_height - 1) + "\n");
        return link_program(pipeline);
    }
    std::string efb_size_glsl() const {
        return "const highp vec2 uEfb=vec2(" + std::to_string(efb_width) + ".0," + std::to_string(efb_height) + ".0);\n";
    }
    // Host bloom targets: the masked quarter-size downsample, its blur, and the
    // eighth-size blur of that.
    std::array<std::array<int, 2>, 3> bloom_sizes() const {
        return {{{{efb_width / 4, efb_height / 4}}, {{efb_width / 4, efb_height / 4}}, {{efb_width / 8, efb_height / 8}}}};
    }
    GlesStats stats;
    TextureCache capture_cache;
    VertexCache geometry_cache;
    bool context_valid = false;
    std::atomic<bool> failed{false};
    bool rgba6 = false;
    FramebufferFetch fetch = FramebufferFetch::none;
    GLuint framebuffer = 0, color_framebuffer = 0, color = 0, depth = 0, vao = 0, vbo = 0, ibo = 0, ubo = 0;
    // EFB rows below the displayed frame.
    GLuint strip_framebuffer = 0, strip_color = 0, strip_depth = 0;
    GLuint clear_program = 0, format_program = 0;
    GLint format_source = -1;
    GLuint depth_read_program = 0;
    GLint depth_read_position = -1;
    // Panfrost serves glReadPixels on the CPU, into a buffer too (Mesa's texture_transfer_modes
    // 0), waiting for the GPU at each call; there readbacks go through a compute pass instead.
    // Mali's driver reads back faster than that pass and the wait for it.
    bool compute_readback = false;
    GLuint store_program = 0, materialize_buffer = 0;
    GLint store_source = -1, store_offset = -1;
    GLsizeiptr materialize_bytes = 0;
    GLuint copy_program = 0, present_program = 0, copy_framebuffer = 0, copy_texture = 0;
    GLint copy_geometry = -1, copy_filter = -1, present_source = -1, present_destination = -1;
    // Fast shading keeps XFB copies in RGB: the copy filter still applies, but the
    // YUYV 4:2:2 encode and the VI decode, two full-screen integer passes, are skipped.
    GLuint copy_rgb_program = 0, present_rgb_program = 0, copy_rgb_taps_program = 0;
    GLint copy_rgb_taps = -1;
    GLint copy_rgb_geometry = -1, copy_rgb_filter = -1, present_rgb_source = -1, present_rgb_destination = -1;
    bool rgb_xfb() const { return direct && limits.direct.shading == DirectShading::fast; }
    std::array<GLint,16> clear_colors{};
    struct Texture {
        GLuint name;
        std::shared_ptr<const TextureSnapshot> image;
        std::uint64_t used;
    };
    std::vector<Texture> textures;
    std::vector<Program> programs;
    std::vector<GLsync> frames;
    std::array<GLint, 2> max_viewport{};
    std::uint64_t sequence = 0;
    TransformUniforms uploaded_transform{};
    bool transform_uploaded = false;
    std::unique_ptr<DrawPreparation> preparation;
    std::unique_ptr<GlesDirect> direct;
    // direct for resident_in on other threads, once created.
    std::atomic<GlesDirect*> shared_direct{nullptr};
    // Resident copy programs specialized per format, scale and source; mode-keyed.
    struct ResidentProgram { std::uint32_t mode = 0; GLuint name = 0; GLint rect = -1, filter = -1, rows = -1; };
    GLuint copy_samplers[2] = {}; // Nearest and linear clamped EFB lookups for fast copies.
    // Host bloom: the scene kept aside while the mask is drawn and the bloom_sizes() targets.
    struct Bloom {
        GLuint scene = 0, textures[3] = {}, framebuffers[3] = {}, programs[4] = {};
        GLint threshold = -1, weights[2] = {-1, -1}, offsets[2] = {-1, -1}, intensity = -1;
    } bloom;
    bool bloom_open = false;
    bool offscreen_open = false;
    GLuint offscreen_color = 0, offscreen_depth = 0; // The EFB's own while an offscreen pass is open.
    std::vector<ResidentProgram> resident_programs;
    struct Xfb { const std::uint8_t* data; GLuint texture; int width, height; GLuint framebuffer = 0; };
    std::vector<Xfb> xfbs;
    struct Presented {
        const std::uint8_t* data; std::uint64_t copies; int width, height; bool black;
        bool operator==(const Presented& o) const {
            return data == o.data && copies == o.copies && width == o.width && height == o.height && black == o.black;
        }
    };
    std::optional<Presented> presented;
    // Owner-thread serial of the latest display copy into each recent destination.
    std::uint64_t display_copies = 0;
    std::deque<std::pair<const std::uint8_t*, std::uint64_t>> copy_serials;
    std::uint64_t copy_serial(const std::uint8_t* data) const {
        for (const auto& [destination, serial] : copy_serials)
            if (destination == data) return serial;
        return 0;
    }
    struct Snapshot { GLuint texture = 0, framebuffer = 0, pack = 0; GLsync ready = nullptr; bool busy = false; };
    std::vector<Snapshot> snapshots;
    // Snapshot slot use as seen by the recording thread; GL objects follow by index.
    std::vector<bool> snapshot_busy;
    // Holders of each busy slot. A snapshot requested with no EFB write since the
    // last one shares its slot instead of packing the same depth again.
    std::vector<unsigned> snapshot_refs;
    int last_snapshot = -1;
    std::uint64_t efb_writes = 0, last_snapshot_writes = 0;
    // Each packed snapshot is read back asynchronously into its buffer; once the fence
    // signals, the GL thread maps it and publishes the generation, so peeks read single
    // pixels from the mapping instead of waiting for queued GL work. The mapping lasts
    // until the slot's next snapshot, which only follows its release.
    static constexpr std::size_t snapshot_slots = 16;
    std::array<std::atomic<std::uint64_t>, snapshot_slots> snapshot_generation{}, snapshot_readable{};
    std::array<const std::uint8_t*, snapshot_slots> snapshot_pixels{};
    // Each slot's read-back region (empty: all of it) and the byte range of its readback
    // that is mapped; set by the recording thread before the slot's generation is published.
    std::array<EfbRegion, snapshot_slots> snapshot_region{};
    std::array<std::size_t, snapshot_slots> snapshot_mapped_from{}, snapshot_mapped_bytes{};
    std::array<std::uint8_t, 4> region_miss_pixel{};
    const std::uint8_t* snapshot_pixel(std::size_t index, int x, int y, bool color) const {
        return snapshot_pixels[index] + snapshot_offset(x, y, color) - snapshot_mapped_from[index];
    }
    struct PendingRead { std::size_t index; std::uint64_t generation; };
    std::vector<PendingRead> pending_reads;
    // GL thread: copies signalled snapshot readbacks; wait forces the one for index.
    void collect_snapshots(int wait = -1) {
        for (auto it = pending_reads.begin(); it != pending_reads.end();) {
            auto& d = snapshots[it->index];
            const bool forced = int(it->index) == wait;
            const auto status = glClientWaitSync(d.ready, forced ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, forced ? 1000000000 : 0);
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
                if (forced) throw std::runtime_error("EFB snapshot readback did not retire within one second");
                ++it; continue;
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, d.pack);
            snapshot_pixels[it->index] = static_cast<const std::uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER,
                GLintptr(snapshot_mapped_from[it->index]), GLsizeiptr(snapshot_mapped_bytes[it->index]), GL_MAP_READ_BIT));
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            if (!snapshot_pixels[it->index]) throw std::runtime_error("EFB snapshot readback map failed");
            glDeleteSync(d.ready); d.ready = nullptr;
            snapshot_readable[it->index].store(it->generation, std::memory_order_release);
            it = pending_reads.erase(it);
        }
    }
    int selected_snapshot = -1;
    bool snapshot_ready(int index) const {
        if (index < 0 || std::size_t(index) >= snapshot_slots) return false;
        return snapshot_readable[std::size_t(index)].load(std::memory_order_acquire) ==
               snapshot_generation[std::size_t(index)].load(std::memory_order_relaxed);
    }
    // The selected snapshot's bytes for an EFB pixel, once its readback is mapped;
    // null when peeks go to the live EFB.
    const std::uint8_t* peek_snapshot(int x, int y, bool color);
    // Submission thread: GL work runs there in call order; inline when not started.
    struct Submission {
        std::thread thread;
        std::thread::id id;
        std::mutex mutex;
        std::condition_variable ready, done;
        std::deque<std::function<void()>> queue;
        std::uint64_t submitted = 0, completed = 0;
        bool running = false, stopping = false;
        std::exception_ptr failure;
        std::function<void()> unbind;
        // Set when GL work runs on a thread the host owns instead of this renderer's own.
        std::function<void(std::function<void()>)> post;
    } submission;
    std::function<void()> swap;
    void execute(std::function<void()> op, bool wait) {
        auto& q = submission;
        if (!q.running || std::this_thread::get_id() == q.id) { op(); return; }
        std::unique_lock<std::mutex> lock(q.mutex);
        if (q.failure) std::rethrow_exception(q.failure);
        // Bounded lag: the recording thread runs at most a few segments ahead.
        q.done.wait(lock, [&] { return (q.post ? q.submitted - q.completed : q.queue.size()) < 32 || q.failure; });
        if (q.failure) std::rethrow_exception(q.failure);
        const auto target = ++q.submitted;
        if (q.post) {
            lock.unlock();
            q.post([this, op = std::move(op)]() mutable { run_task(op); });
            if (!wait) return;
            lock.lock();
        } else {
            q.queue.push_back(std::move(op));
            q.ready.notify_one();
            if (!wait) return;
        }
        q.done.wait(lock, [&] { return q.completed >= target || q.failure; });
        if (q.failure) std::rethrow_exception(q.failure);
    }
    // Runs one task on the GL thread. A failure latches; the recording thread sees it next.
    void run_task(std::function<void()>& op) {
        auto& q = submission;
        try { op(); if (!pending_reads.empty()) collect_snapshots(); }
        catch (...) {
            std::lock_guard<std::mutex> lock(q.mutex);
            if (!q.failure) {
                q.failure = std::current_exception();
                try { throw; } catch (const std::exception& e) { std::fprintf(stderr, "GLES renderer failed: %s\n", e.what()); } catch (...) {}
            }
            failed = true;
        }
        op = nullptr;
        {
            std::lock_guard<std::mutex> lock(q.mutex);
            ++q.completed;
        }
        q.done.notify_all();
    }
    void run_submission() {
        auto& q = submission;
        for (;;) {
            std::function<void()> op;
            {
                std::unique_lock<std::mutex> lock(q.mutex);
                q.ready.wait(lock, [&] { return q.stopping || !q.queue.empty(); });
                if (q.queue.empty()) break;
                op = std::move(q.queue.front());
                q.queue.pop_front();
            }
            run_task(op);
        }
        if (q.unbind) q.unbind();
    }
    explicit Impl(GlesLimits budget) : limits(budget), capture_cache({budget.texture_bytes, std::min<std::size_t>(budget.textures, 256)}), geometry_cache(budget.geometry) {}
    ~Impl() {
        if (owner != std::this_thread::get_id()) std::terminate();
        if (!context_valid) return;
        shared_direct.store(nullptr, std::memory_order_release);
        direct.reset();
        for (const auto& p : resident_programs) glDeleteProgram(p.name);
        if (copy_samplers[0]) glDeleteSamplers(2, copy_samplers);
        for (auto program : bloom.programs) if (program) glDeleteProgram(program);
        glDeleteFramebuffers(3, bloom.framebuffers);
        glDeleteTextures(3, bloom.textures);
        if (bloom.scene) glDeleteTextures(1, &bloom.scene);
        if (offscreen_color) glDeleteTextures(1, &offscreen_color);
        if (offscreen_depth) glDeleteTextures(1, &offscreen_depth);
        for (const auto& x : xfbs) { glDeleteFramebuffers(1, &x.framebuffer); glDeleteTextures(1, &x.texture); }
        for (std::size_t i = 0; i < snapshots.size(); ++i) if (snapshot_pixels[i]) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, snapshots[i].pack); glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        for (const auto& d : snapshots) {
            if (d.ready) glDeleteSync(d.ready);
            glDeleteBuffers(1, &d.pack); glDeleteFramebuffers(1, &d.framebuffer); glDeleteTextures(1, &d.texture);
        }
        if (clear_program) glDeleteProgram(clear_program);
        if (format_program) glDeleteProgram(format_program);
        if (copy_program) glDeleteProgram(copy_program);
        if (present_program) glDeleteProgram(present_program);
        if (copy_rgb_program) glDeleteProgram(copy_rgb_program);
        if (copy_rgb_taps_program) glDeleteProgram(copy_rgb_taps_program);
        if (present_rgb_program) glDeleteProgram(present_rgb_program);
        if (copy_framebuffer) glDeleteFramebuffers(1,&copy_framebuffer);
        if (copy_texture) glDeleteTextures(1,&copy_texture);
        for (auto frame : frames) glDeleteSync(frame);
        for (const auto& p : programs) glDeleteProgram(p.name);
        for (const auto& texture : textures) if (texture.name) glDeleteTextures(1, &texture.name);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ibo) glDeleteBuffers(1, &ibo);
        if (ubo) glDeleteBuffers(1, &ubo);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (depth) glDeleteTextures(1, &depth);
        if (depth_read_program) glDeleteProgram(depth_read_program);
        if (store_program) glDeleteProgram(store_program);
        if (materialize_buffer) glDeleteBuffers(1, &materialize_buffer);
        if (color) glDeleteTextures(1, &color);
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
        if (color_framebuffer) glDeleteFramebuffers(1, &color_framebuffer);
        if (strip_framebuffer) glDeleteFramebuffers(1, &strip_framebuffer);
        if (strip_color) glDeleteTextures(1, &strip_color);
        if (strip_depth) glDeleteTextures(1, &strip_depth);
    }
    void ensure_direct() {
        if (direct) return;
        auto& g = *this;
        if (!color_framebuffer) execute([&g] {
            glGenFramebuffers(1, &g.color_framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, g.color_framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.color, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("EFB color-only framebuffer is incomplete");
        }, true);
        direct = std::make_unique<GlesDirect>(limits.direct, fetch, GlesDirect::Target{framebuffer, color_framebuffer, [&g](bool rgba6) {
            if (rgba6 == g.rgba6) return false;
            g.reinterpret_format(rgba6); return true;
        }, [&g](const GlesResidentCopy& copy) { g.execute([&g, &copy] { g.materialize(copy); }, true); },
        [&g](std::function<void()> op, bool wait) { g.execute(std::move(op), wait); }, strip_framebuffer});
        shared_direct.store(direct.get(), std::memory_order_release);
    }
    void flush_direct() {
        if (!direct) return;
        try { direct->flush(); } catch (...) { failed = true; throw; }
    }
    void check_owner() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("GLES renderer called from non-owner thread");
    }
    void require_ready() {
        check_owner();
        if (failed) throw std::runtime_error("GLES renderer failed; destroy it before further rendering");
        flush_direct();
    }
    void submit(const DrawPacket&,const GlesDrawPipeline&,const std::vector<GpuVertex>&,
                const std::vector<std::uint16_t>&,std::size_t logical_draws);
    void initialize() {
        if (!limits.programs || limits.programs > 256 || !limits.texture_bytes || !limits.textures || limits.textures > 256)
            throw std::invalid_argument("invalid GLES cache budget");
        if (limits.aspect_width < 1 || limits.aspect_height < 1 || limits.aspect_width > 10000 || limits.aspect_height > 10000)
            throw std::invalid_argument("invalid GLES presentation shape");
        if (const int s = limits.direct.snapshot_scale; s && s != 1 && s != 2 && s != 4)
            throw std::invalid_argument("invalid EFB snapshot scale");
        if (!glGetString(GL_VERSION)) throw std::runtime_error("GLES context is not current");
        context_valid = true;
        GLint major = 0, minor = 0, attributes = 0, block_bytes = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major); glGetIntegerv(GL_MINOR_VERSION, &minor);
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes); glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &block_bytes);
        if (major < 3 || (major == 3 && minor < 1) || attributes < 15 || block_bytes < sizeof(TransformUniforms))
            throw std::runtime_error("GLES context cannot support the GX draw layout");
        GLint extensions = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS,&extensions);
        for (GLint i=0;i<extensions;++i) {
            const auto* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS,i));
            if (name && !std::strcmp(name,"GL_EXT_shader_framebuffer_fetch")) fetch = FramebufferFetch::ext;
            if (name && !std::strcmp(name,"GL_ARM_shader_framebuffer_fetch") && fetch==FramebufferFetch::none)
                fetch = FramebufferFetch::arm;
        }
        if (const auto* name = reinterpret_cast<const char*>(glGetString(GL_RENDERER)))
            compute_readback = std::strstr(name, "Panfrost") != nullptr;
        glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_viewport.data());
        glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, efb_width, efb_height);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glGenTextures(1, &depth); glBindTexture(GL_TEXTURE_2D, depth);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, efb_width, efb_height);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_MODE,GL_NONE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("GX RGBA8/D24 EFB allocation failed");
        glGenFramebuffers(1, &strip_framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, strip_framebuffer);
        glGenTextures(1, &strip_color); glBindTexture(GL_TEXTURE_2D, strip_color);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, efb_width, efb_strip_height);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, strip_color, 0);
        glGenTextures(1, &strip_depth); glBindTexture(GL_TEXTURE_2D, strip_depth);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, efb_width, efb_strip_height);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, strip_depth, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("EFB strip allocation failed");
        glClearColor(0, 0, 0, 1); glClearDepthf(1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glGenVertexArrays(1, &vao); glBindVertexArray(vao);
        glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        auto attribute = [](unsigned location, int count, GLenum type, bool normalized, std::size_t offset) {
            glEnableVertexAttribArray(location);
            glVertexAttribPointer(location, count, type, normalized, sizeof(GpuVertex), reinterpret_cast<const void*>(offset));
        };
        attribute(0, 3, GL_FLOAT, false, offsetof(GpuVertex, position));
        for (unsigned i = 0; i < 3; ++i) attribute(1 + i, 3, GL_FLOAT, false, offsetof(GpuVertex, normal) + i * 12);
        for (unsigned i = 0; i < 2; ++i) attribute(4 + i, 4, GL_UNSIGNED_BYTE, true, offsetof(GpuVertex, colors) + i * 4);
        for (unsigned i = 0; i < 8; ++i) attribute(6 + i, 2, GL_FLOAT, false, offsetof(GpuVertex, texcoords) + i * 8);
        glEnableVertexAttribArray(14);
        glVertexAttribIPointer(14, 3, GL_UNSIGNED_INT, sizeof(GpuVertex), reinterpret_cast<const void*>(offsetof(GpuVertex, matrices)));
        glGenBuffers(1, &ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glGenBuffers(1, &ubo);
        programs.reserve(limits.programs); frames.reserve(2);
        gl_check("renderer initialization");
    }
    Program& program(const GlesDrawPipeline& pipeline, const DrawPacket& packet) {
        for (auto& p : programs) if (p.key == pipeline.key) { p.used = ++sequence; return p; }
        Program p;
        p.key = pipeline.key;
        const auto source = plan_gles_draw(packet, fetch);
        if (!source || source.pipeline.key != pipeline.key)
            throw std::runtime_error("GX shader source does not match validated pipeline key");
        p.name = link_program(source.pipeline);
        try {
            const auto block = glGetUniformBlockIndex(p.name, "GXTransform");
            if (block == GL_INVALID_INDEX) throw std::runtime_error("missing GX transform uniform block");
            GLint size = 0; glGetActiveUniformBlockiv(p.name, block, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
            if (size > GLint(transform_shared_bytes)) throw std::runtime_error("GX uniform block layout mismatch");
            glUniformBlockBinding(p.name, block, 0); glUseProgram(p.name);
            auto location = [&](const std::string& name) { return glGetUniformLocation(p.name, name.c_str()); };
            for (unsigned i = 0; i < 4; ++i) {
                p.registers[i].location = location("uGxTevRegisters[" + std::to_string(i) + "]");
                p.konst[i].location = location("uGxTevKonst[" + std::to_string(i) + "]");
            }
            for (unsigned i = 0; i < 16; ++i) p.scales[i].location = location("uGxCoordinateScale[" + std::to_string(i) + "]");
            for (unsigned i = 0; i < 16; ++i) p.image_sizes[i].location = location("uGxImageSize[" + std::to_string(i) + "]");
            for (unsigned i = 0; i < 4; ++i) p.indirect_scales[i].location = location("uGxIndirectScale[" + std::to_string(i) + "]");
            for (unsigned i = 0; i < 6; ++i) p.indirect_rows[i].location = location("uGxIndirectRows[" + std::to_string(i) + "]");
            for (unsigned i = 0; i < 8; ++i) p.sampler_lod[i].location = location("uGxSamplerLod[" + std::to_string(i) + "]");
            p.alpha.location = location("uGxTevAlphaRef");
            p.pixel.location = location("uGxPixel");
            p.z_bias.location = location("uGxZBias");
            for (unsigned i = 0; i < 8; ++i) glUniform1i(location("uGxTexture" + std::to_string(i)), i);
            gl_check("GX program uniforms");
            p.used = ++sequence;
            if (programs.size() == limits.programs) {
                auto oldest = std::min_element(programs.begin(), programs.end(), [](const auto& a, const auto& b) { return a.used < b.used; });
                glDeleteProgram(oldest->name); *oldest = std::move(p); ++stats.shader_compiles; return *oldest;
            }
            programs.push_back(std::move(p)); ++stats.shader_compiles; return programs.back();
        } catch (...) { glDeleteProgram(p.name); throw; }
    }
    void bind_textures(const DrawPacket& packet) {
        std::size_t required=0, count=0;
        for (unsigned i=0; i<8; ++i) if (packet.textures[i]) {
            bool duplicate=false;
            for (unsigned j=0; j<i; ++j)
                if (packet.textures[j] && same_texture(*packet.textures[i],*packet.textures[j])) duplicate=true;
            if (duplicate) continue;
            const auto bytes=image_bytes(*packet.textures[i]);
            if (++count>limits.textures || required>limits.texture_bytes || bytes>limits.texture_bytes-required)
                throw std::runtime_error("GLES active textures exceed cache budget");
            required+=bytes;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT,0);
        glPixelStorei(GL_UNPACK_SKIP_IMAGES,0);
        for (unsigned i=0; i<8; ++i) if (packet.textures[i]) {
            const auto& image=packet.textures[i];
            auto found=std::find_if(textures.begin(),textures.end(),[&](const auto& entry){return same_texture(*entry.image,*image);});
            glBindSampler(i,0);glActiveTexture(GL_TEXTURE0+i);
            if (found!=textures.end()) {
                found->used=++sequence;glBindTexture(GL_TEXTURE_2D,found->name);continue;
            }
            const auto bytes=image_bytes(*image);
            while (textures.size()>=limits.textures || stats.texture_bytes>limits.texture_bytes-bytes) {
                auto oldest=textures.end();
                for (auto it=textures.begin();it!=textures.end();++it) {
                    bool active=false;
                    for (const auto& requested:packet.textures)
                        if (requested && same_texture(*it->image,*requested)) active=true;
                    if (!active && (oldest==textures.end() || it->used<oldest->used)) oldest=it;
                }
                if (oldest==textures.end()) throw std::runtime_error("GLES texture eviction cannot fit active draw");
                glDeleteTextures(1,&oldest->name);stats.texture_bytes-=image_bytes(*oldest->image);textures.erase(oldest);
            }
            textures.push_back({0,image,++sequence});auto& entry=textures.back();
            glGenTextures(1,&entry.name);glBindTexture(GL_TEXTURE_2D,entry.name);
            for (unsigned lod=0;lod<image->levels().size();++lod) {
                const auto& level=image->levels()[lod];
                glTexImage2D(GL_TEXTURE_2D,lod,GL_RGBA8,level.width,level.height,0,GL_RGBA,GL_UNSIGNED_BYTE,level.rgba.data());
            }
            const auto& sampler=image->descriptor().sampler;
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,min_filter(sampler.min_filter));
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,sampler.linear_magnification?GL_LINEAR:GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap(sampler.wrap_s));
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap(sampler.wrap_t));
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,image->levels().size()-1);
            glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MIN_LOD,0);glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MAX_LOD,image->levels().size()-1);
            gl_check("GX texture upload and sampler");++stats.texture_uploads;stats.texture_bytes+=bytes;
        }
    }
    void read_depth(int x, int y, int width, int height, std::uint8_t* pixels, int snapshot);
    void clear_efb_gl(const EfbClear&);
    void attach_color() {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        if (!color_framebuffer) return;
        glBindFramebuffer(GL_FRAMEBUFFER, color_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    }
    void attach_depth() {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    }
    void swap_offscreen() {
        std::swap(color, offscreen_color);
        std::swap(depth, offscreen_depth);
        attach_color();
        attach_depth();
    }
    void begin_offscreen_gl();
    void bloom_resources();
    void begin_bloom_gl();
    void end_bloom_gl(const EfbBloom&);
    void copy_display_gl(const DisplayCopy&, std::uint8_t* destination);
    void present_gl(const FramebufferView&, int width, int height, bool black);
    // Packs D24 depth as RGB8 into target; with_color appends the EFB colour as a
    // second block of width columns.
    void pack_efb(GLuint target, int x, int y, int width, int height, int scale = 1, bool with_color = false);
    // Stores width x height RGBA8 texels of texture from (x, y) into buffer as bytes, rows
    // stride texels apart from texel offset, as glReadPixels would pack them.
    void store_texels(GLuint texture, GLuint buffer, int x, int y, int width, int height, int offset, int stride) {
        if (!store_program) {
            store_program = link_compute(R"GLSL(#version 310 es
precision highp float;precision highp int;
layout(local_size_x=64)in;
uniform highp sampler2D uImage;
uniform ivec4 uSource;
uniform int uOffset;
layout(std430,binding=0)writeonly buffer Pack{highp uint texels[];};
void main(){
ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=uSource.z)return;
texels[uOffset+p.y*uSource.w+p.x]=packUnorm4x8(texelFetch(uImage,uSource.xy+p,0));
})GLSL");
            ++stats.shader_compiles;
            glUseProgram(store_program);
            glUniform1i(glGetUniformLocation(store_program, "uImage"), 0);
            store_source = glGetUniformLocation(store_program, "uSource");
            store_offset = glGetUniformLocation(store_program, "uOffset");
        }
        glUseProgram(store_program);
        glUniform1i(store_offset, offset);
        glUniform4i(store_source, x, y, width, stride);
        glActiveTexture(GL_TEXTURE0); glBindSampler(0, 0); glBindTexture(GL_TEXTURE_2D, texture);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
        glDispatchCompute(GLuint(width + 63) / 64, GLuint(height), 1);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    }
    // The snapshot's rows from row0, its depth and then colour columns from column0, into its
    // buffer at the offsets a whole readback would use.
    void store_snapshot(const Snapshot& snapshot, int row0, int row_count, int column0, int column_count) {
        const int columns = efb_width / snapshot_scale();
        for (int block = 0; block < 2; ++block)
            store_texels(snapshot.texture, snapshot.pack, block * columns + column0, row0, column_count, row_count,
                         row0 * 2 * columns + block * columns + column0, 2 * columns);
    }
    // Fast shading keeps draw-sync snapshots at quarter resolution unless the limits ask
    // for another; the deferred callbacks peek single pixels and read the nearest sample.
    int snapshot_scale() const {
        if (limits.direct.snapshot_scale) return limits.direct.snapshot_scale;
        return limits.direct.shading == DirectShading::fast ? 4 : 1;
    }
    GLsizeiptr snapshot_bytes() const {
        const int scale = snapshot_scale();
        return GLsizeiptr(2 * (efb_width / scale)) * (efb_height / scale) * 4;
    }
    // Snapshot rows hold the packed depth, then the colour, of one scaled EFB row.
    std::size_t snapshot_offset(int x, int y, bool color) const {
        const int scale = snapshot_scale(), width = efb_width / scale;
        return (std::size_t((efb_height - 1 - y) / scale) * 2 * width + (color ? width : 0) + x / scale) * 4;
    }
    void copy_surface() {
        if (copy_texture)
            return;
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, 0);
        glGenTextures(1, &copy_texture);
        glBindTexture(GL_TEXTURE_2D, copy_texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, efb_width, efb_height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glGenFramebuffers(1, &copy_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, copy_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("native display-copy framebuffer is incomplete");
        gl_check("native display-copy surface");
    }
    static void fullscreen_state() {
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DITHER);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_SAMPLE_COVERAGE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
    }
    void reinterpret_format(bool target_rgba6) {
        if (rgba6 == target_rgba6) return;
        // Fast direct shading keeps RGBA6 as plain RGBA8 values (direct.md): copies and
        // clears after a format change must not reinterpret what fast programs drew.
        if (direct && limits.direct.shading == DirectShading::fast) { rgba6 = target_rgba6; return; }
        ++stats.format_conversions;
        copy_surface();
        if (!format_program) {
            GlesDrawPipeline p;
            p.vertex = "#version 310 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
            p.fragment = "#version 310 es\nprecision highp float;precision highp int;\nuniform sampler2D uImage;uniform bool uFromRgba6;layout(location=0)out vec4 color;\n";
            p.fragment += efb_format_glsl();
            p.fragment += "void main(){uvec4 c=uvec4(floor(texelFetch(uImage,ivec2(gl_FragCoord.xy),0)*255.0+0.5));color=vec4(gx_reinterpret_efb(c,uFromRgba6))/255.0;}";
            format_program = link_program(p);
            ++stats.shader_compiles;
            glUseProgram(format_program);
            glUniform1i(glGetUniformLocation(format_program,"uImage"),0);
            format_source = glGetUniformLocation(format_program,"uFromRgba6");
        }
        fullscreen_state();
        glBindFramebuffer(GL_READ_FRAMEBUFFER,framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,copy_framebuffer);
        glBlitFramebuffer(0,0,efb_width,efb_height,0,0,efb_width,efb_height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
        glViewport(0,0,efb_width,efb_height);
        glBindVertexArray(vao);
        glUseProgram(format_program);
        glUniform1i(format_source,rgba6);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0,0);
        glBindTexture(GL_TEXTURE_2D,copy_texture);
        glDrawArrays(GL_TRIANGLES,0,3);
        gl_check("EFB format reinterpretation");
        rgba6 = target_rgba6;
    }
    void display_programs(bool copy) {
        auto &name = copy ? copy_program : present_program;
        if (name)
            return;
        GlesDrawPipeline pipeline;
        pipeline.vertex = "#version 310 es\nvoid main(){vec2 "
                          "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
        if (copy)
            pipeline.fragment = R"GLSL(#version 310 es
precision highp float;
precision highp int;
uniform sampler2D uImage;
uniform ivec4 uGeometry;
uniform ivec4 uFilter;
layout(location=0) out vec4 color;
ivec3 sampleRgb(int x,int y){return ivec3(floor(texelFetch(uImage,ivec2(x,GX_EFB_TOP-y),0).rgb*255.0+0.5));}
ivec3 filtered(int x,int y){
 x=clamp(x,uGeometry.x,uGeometry.x+uGeometry.z-1);
 int top=((uFilter.w&1)!=0)?uGeometry.y:0;
 int bottom=((uFilter.w&2)!=0)?uGeometry.y+uGeometry.w-1:GX_EFB_TOP;
 ivec3 sum=sampleRgb(x,clamp(y-1,top,bottom))*uFilter.x+sampleRgb(x,y)*uFilter.y+sampleRgb(x,clamp(y+1,top,bottom))*uFilter.z;
 ivec3 c=clamp(sum/64,ivec3(0),ivec3(255));
 return ivec3((257*c.r+504*c.g+98*c.b+16000)/1000,
              (-148*c.r-291*c.g+439*c.b+128000)/1000,
              (439*c.r-368*c.g-71*c.b+128000)/1000);
}
void main(){
 ivec2 p=ivec2(gl_FragCoord.xy);int x=uGeometry.x+p.x*2;int y=uGeometry.y+p.y;
 ivec3 l=filtered(x-1,y),m=filtered(x,y),r=filtered(x+1,y);
 color=vec4(m.x,(l.y+2*m.y+r.y)/4,r.x,(l.z+2*m.z+r.z)/4)/255.0;
})GLSL";
        else
            pipeline.fragment = R"GLSL(#version 310 es
precision highp float;
precision highp int;
uniform sampler2D uImage;
uniform ivec4 uSource;
uniform ivec4 uDestination;
layout(location=0) out vec4 color;
void main(){
 ivec2 p=ivec2(gl_FragCoord.xy)-uDestination.xy;
 ivec2 n=(p*2+1)*uSource.xy,d=uDestination.zw*2;
 // n/d without a per-pixel integer division (see the RGB presentation).
 ivec2 source=ivec2(vec2(n)/vec2(d));
 source+=ivec2(lessThanEqual((source+1)*d,n))-ivec2(greaterThan(source*d,n));source.y=uSource.y-1-source.y;
 ivec4 a=ivec4(floor(texelFetch(uImage,ivec2(source.x/2,source.y),0)*255.0+0.5));
 ivec4 b=ivec4(floor(texelFetch(uImage,ivec2(min(source.x/2+1,uSource.x/2-1),source.y),0)*255.0+0.5));
 int y=2*(((source.x&1)!=0?a.b:a.r)-16);
 int u=((source.x&1)!=0?a.g+b.g:2*a.g)-256;
 int v=((source.x&1)!=0?a.a+b.a:2*a.a)-256;
 ivec3 c=clamp(max(ivec3(0),ivec3(1164*y+1596*v,1164*y-392*u-813*v,1164*y+2017*u)+1000)/2000,ivec3(0),ivec3(255));
 color=vec4(vec3(c)/255.0,1.0);
})GLSL";
        name = link_efb_program(pipeline);
        ++stats.shader_compiles;
        glUseProgram(name);
        glUniform1i(glGetUniformLocation(name, "uImage"), 0);
        if (copy) {
            copy_geometry = glGetUniformLocation(name, "uGeometry");
            copy_filter = glGetUniformLocation(name, "uFilter");
        } else {
            present_source = glGetUniformLocation(name, "uSource");
            present_destination = glGetUniformLocation(name, "uDestination");
        }
        gl_check("native display-copy/presentation shader");
    }
    void rgb_display_programs() {
        if (copy_rgb_program) return;
        GlesDrawPipeline copy, present;
        copy.vertex = present.vertex = "#version 310 es\nvoid main(){vec2 "
                          "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
        copy.fragment = R"GLSL(#version 310 es
precision mediump float;
precision highp int;
uniform sampler2D uImage;
uniform ivec4 uGeometry;
uniform ivec4 uFilter;
layout(location=0) out vec4 color;
void main(){
 ivec2 p=ivec2(gl_FragCoord.xy);int x=uGeometry.x+p.x;int y=uGeometry.y+p.y;
 int top=((uFilter.w&1)!=0)?uGeometry.y:0;
 int bottom=((uFilter.w&2)!=0)?uGeometry.y+uGeometry.w-1:GX_EFB_TOP;
 vec3 sum=texelFetch(uImage,ivec2(x,GX_EFB_TOP-clamp(y-1,top,bottom)),0).rgb*float(uFilter.x)+
          texelFetch(uImage,ivec2(x,GX_EFB_TOP-y),0).rgb*float(uFilter.y)+
          texelFetch(uImage,ivec2(x,GX_EFB_TOP-clamp(y+1,top,bottom)),0).rgb*float(uFilter.z);
 color=vec4(clamp(sum/64.0,0.0,1.0),1.0);
})GLSL";
        present.fragment = R"GLSL(#version 310 es
precision highp float;
precision highp int;
uniform sampler2D uImage;
uniform ivec4 uSource;
uniform ivec4 uDestination;
layout(location=0) out vec4 color;
void main(){
 ivec2 p=ivec2(gl_FragCoord.xy)-uDestination.xy;
 // A nearest blit's mapping, rows flipped: the window's top row shows source row 0.
 p.y=uDestination.w-1-p.y;
 ivec2 n=(p*2+1)*uSource.xy,d=uDestination.zw*2;
 // n/d without an integer division, which Mali-G31 runs as a long sequence for every window
 // pixel: the float quotient is off by at most one, and the products correct it exactly.
 ivec2 source=ivec2(vec2(n)/vec2(d));
 source+=ivec2(lessThanEqual((source+1)*d,n))-ivec2(greaterThan(source*d,n));
 color=vec4(texelFetch(uImage,source,0).rgb,1.0);
})GLSL";
        // A symmetric three-tap vertical filter (a,64-2a,a) over full-height copies is two
        // bilinear lookups 2a/64 rows above and below; clamping matches at the EFB edges.
        GlesDrawPipeline taps = copy;
        taps.fragment = "#version 310 es\nprecision mediump float;\nuniform mediump sampler2D uImage;\n"
                        "uniform highp vec3 uTaps;\nlayout(location=0) out vec4 color;\n" + efb_size_glsl() +
                        "void main(){\n"
                        " highp vec2 p=vec2(uTaps.x+gl_FragCoord.x,uEfb.y-uTaps.y-gl_FragCoord.y)/uEfb;\n"
                        " highp vec2 d=vec2(0.0,uTaps.z/uEfb.y);\n"
                        " color=vec4(0.5*(texture(uImage,p+d).rgb+texture(uImage,p-d).rgb),1.0);\n}\n";
        copy_rgb_taps_program = link_program(taps);
        glUseProgram(copy_rgb_taps_program);
        glUniform1i(glGetUniformLocation(copy_rgb_taps_program, "uImage"), 0);
        copy_rgb_taps = glGetUniformLocation(copy_rgb_taps_program, "uTaps");
        copy_rgb_program = link_efb_program(copy); present_rgb_program = link_efb_program(present);
        stats.shader_compiles += 2;
        glUseProgram(copy_rgb_program);
        glUniform1i(glGetUniformLocation(copy_rgb_program, "uImage"), 0);
        copy_rgb_geometry = glGetUniformLocation(copy_rgb_program, "uGeometry");
        copy_rgb_filter = glGetUniformLocation(copy_rgb_program, "uFilter");
        glUseProgram(present_rgb_program);
        glUniform1i(glGetUniformLocation(present_rgb_program, "uImage"), 0);
        present_rgb_source = glGetUniformLocation(present_rgb_program, "uSource");
        present_rgb_destination = glGetUniformLocation(present_rgb_program, "uDestination");
        gl_check("native RGB display-copy/presentation shader");
    }
    // Fast shading copies colour formats without the 4/5/6-bit channel quantization
    // and box-filters half-scale copies with one bilinear lookup.
    bool fast_copy(const TextureCopy& p) const {
        // Half-scale copies box-filter in the copy shader with GX's integer rounding; a linear
        // blit rounds as the driver chooses, and on Mesa every blit flushes all queued work.
        return limits.direct.shading == DirectShading::fast && !p.depth && !p.filtered() && (!p.half_scale || p.strip) &&
               (p.format == 1 || p.format == 4 || p.format == 6 || (p.format >= 7 && p.format <= 10));
    }
    GLuint copy_sampler(bool linear) {
        if (!copy_samplers[0]) {
            glGenSamplers(2, copy_samplers);
            for (unsigned i = 0; i < 2; ++i) {
                glSamplerParameteri(copy_samplers[i], GL_TEXTURE_MIN_FILTER, i ? GL_LINEAR : GL_NEAREST);
                glSamplerParameteri(copy_samplers[i], GL_TEXTURE_MAG_FILTER, i ? GL_LINEAR : GL_NEAREST);
                glSamplerParameteri(copy_samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glSamplerParameteri(copy_samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        }
        return copy_samplers[linear ? 1 : 0];
    }
    std::string fast_copy_fragment(const TextureCopy& p) const {
        std::string value;
        if (p.format == 6) value = "c";
        else if (p.format == 4) value = "vec4(c.rgb,1.0)";
        else if (p.format == 7) value = "vec4(c.a)";
        else if (p.luminance) value = "vec4(dot(c.rgb,vec3(0.257,0.504,0.098))+16.0/255.0)";
        else value = std::string("vec4(c.") + "rrgb"[p.format == 1 ? 0 : p.format - 7] + ")";
        return std::string("#version 310 es\nprecision mediump float;\nuniform mediump sampler2D uImage;\nuniform highp vec3 uRect;\n"
                           "layout(location=0) out vec4 color;\n") + efb_size_glsl() + "void main(){\n"
                           " highp vec2 p=vec2(uRect.x+uRect.z*gl_FragCoord.x,uEfb.y-uRect.y-uRect.z*gl_FragCoord.y)/uEfb;\n"
                           " vec4 c=texture(uImage,p);\n" + (p.rgba6 ? "" : " c.a=1.0;\n") + " color=" + value + ";\n}\n";
    }
    // A copy whose clear rewrites a whole attachment the copy does not read leaves that
    // attachment's contents dead: invalidating it spares the GPU writing it back when the
    // copy ends the pass. Colour invalidation needs alpha cleared too (RGB8 keeps it opaque).
    void invalidate_cleared(const TextureCopy& p) {
        const auto& c = *p.clear;
        if (p.strip || c.rectangle.x || c.rectangle.y || c.rectangle.width != efb_width || c.rectangle.height != efb_height) return;
        GLenum attachments[1];
        GLsizei count = 0;
        if (!p.depth && c.depth_write) attachments[count++] = GL_DEPTH_ATTACHMENT;
        if (p.depth && c.color_write && c.alpha_write) attachments[count++] = GL_COLOR_ATTACHMENT0;
        if (!count) return;
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, count, attachments);
        ++stats.invalidations;
    }
    void resident_pass(const TextureCopy& p, GlesDirect::ResidentTarget target) {
        const bool fast = fast_copy(p);
        // Fast copies other than luminance are blits: the EFB colour already holds the
        // texel the copy decodes to, and the target's swizzle selects the copied channel
        // (one where the copy format or EFB has none).
        if (p.strip && !(fast && !(p.format == 1 && p.luminance)))
            throw std::runtime_error("EFB copies below the displayed frame need the fast blit");
        if (fast && !(p.format == 1 && p.luminance)) {
            fullscreen_state();
            const int top = p.strip ? EfbLayout::rows : efb_height;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, p.strip ? strip_framebuffer : color_framebuffer ? color_framebuffer : framebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.framebuffer);
            glBlitFramebuffer(p.x, top - p.y - p.height, p.x + p.width, top - p.y,
                              0, p.destination_height(), p.destination_width(), 0, GL_COLOR_BUFFER_BIT,
                              p.half_scale ? GL_LINEAR : GL_NEAREST);
            const GLint alpha = p.rgba6 ? GL_ALPHA : GL_ONE;
            GLint swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, p.format == 4 ? GL_ONE : alpha};
            if (p.format == 7) std::fill(swizzle, swizzle + 4, alpha);
            else if (p.format == 1 || p.format >= 8) std::fill(swizzle, swizzle + 4, p.format == 9 ? GL_GREEN : p.format == 10 ? GL_BLUE : GL_RED);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, target.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, swizzle[0]); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, swizzle[1]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, swizzle[2]); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, swizzle[3]);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            gl_check("resident EFB texture blit");
            return;
        }
        const std::uint32_t mode = std::uint32_t(p.format) | (std::uint32_t(p.half_scale) << 8) | (std::uint32_t(p.rgba6) << 9) |
                                   (std::uint32_t(p.luminance) << 10) | (std::uint32_t(p.depth) << 11) | (std::uint32_t(fast) << 12) |
                                   (std::uint32_t(p.filtered()) << 13);
        auto found = std::find_if(resident_programs.begin(), resident_programs.end(), [&](const auto& r) { return r.mode == mode; });
        if (found == resident_programs.end()) {
            GlesDrawPipeline pipeline;
            pipeline.vertex = "#version 310 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
            // Mirrors decode_texture_copy: copy encoding followed by its sample-format decode,
            // with the copy mode fixed at compile time.
            if (fast) pipeline.fragment = fast_copy_fragment(p);
            else pipeline.fragment = "#version 310 es\nprecision highp float;\nprecision highp int;\n#define FORMAT " + std::to_string(int(p.format)) +
                "\n#define HALF " + std::to_string(int(p.half_scale)) + "\n#define RGBA6 " + std::to_string(int(p.rgba6)) +
                "\n#define LUMINANCE " + std::to_string(int(p.luminance)) + "\n#define DEPTH " + std::to_string(int(p.depth)) +
                "\n#define FILTER " + std::to_string(int(p.filtered())) + R"GLSL(
uniform highp sampler2D uImage;
uniform ivec2 uRect;
#if FILTER
uniform ivec3 uFilter;
uniform ivec2 uRows;
#endif
layout(location=0) out vec4 color;
ivec4 efb(int x,int y){
 vec4 t=texelFetch(uImage,ivec2(x,GX_EFB_TOP-y),0);
#if DEPTH
 uint z=uint(roundEven(t.r*16777215.0));return ivec4(int(z>>16u),int((z>>8u)&255u),int(z&255u),255);
#else
 return ivec4(floor(t*255.0+0.5));
#endif
}
// The vertical filter weighs each source row with its neighbours before any reduction.
ivec4 source(int x,int y){
 ivec4 c=efb(x,y);
#if FILTER
 ivec3 v=((efb(x,clamp(y-1,uRows.x,uRows.y)).rgb*uFilter.x+c.rgb*uFilter.y+efb(x,clamp(y+1,uRows.x,uRows.y)).rgb*uFilter.z)>>6)&0x1ff;
 c=ivec4(min(v,ivec3(255)),c.a);
#endif
 return c;
}
int e4(int v){return v*17;}
int e5(int v){return (v<<3)|(v>>2);}
int e6(int v){return (v<<2)|(v>>4);}
void main(){
 ivec2 o=ivec2(gl_FragCoord.xy);int x=uRect.x+o.x*(HALF+1);int y=uRect.y+o.y*(HALF+1);
 ivec4 c=source(x,y);
#if HALF
 c=(c+source(x+1,y)+source(x,y+1)+source(x+1,y+1))/4;
#endif
 int alpha=RGBA6!=0?c.a:255;const int f=FORMAT;ivec4 d;
 if(f==3||f==11||f==12){int i=f==12?c.g:c.r;d=ivec4(i,i,i,f==3?alpha:(f==11?c.g:c.b));}
 else if(f==1||f>=7){int v=f==7?alpha:(LUMINANCE!=0?(257*c.r+504*c.g+98*c.b+16000)/1000:(f==1?c.r:c[clamp(f-8,0,2)]));d=ivec4(v);}
 else if(f==4)d=ivec4(e5(c.r>>3),e6(c.g>>2),e5(c.b>>3),255);
 else if(f==5){if(alpha>=224)d=ivec4(e5(c.r>>3),e5(c.g>>3),e5(c.b>>3),255);else{int a=alpha>>5;d=ivec4(e4(c.r>>4),e4(c.g>>4),e4(c.b>>4),(a<<5)|(a<<2)|(a>>1));}}
 else d=ivec4(c.rgb,alpha);
 color=vec4(d)/255.0;
})GLSL";
            ResidentProgram created;
            created.mode = mode;
            created.name = link_efb_program(pipeline);
            ++stats.shader_compiles;
            glUseProgram(created.name);
            glUniform1i(glGetUniformLocation(created.name, "uImage"), 0);
            created.rect = glGetUniformLocation(created.name, "uRect");
            created.filter = glGetUniformLocation(created.name, "uFilter");
            created.rows = glGetUniformLocation(created.name, "uRows");
            resident_programs.push_back(created);
            found = std::prev(resident_programs.end());
        }
        fullscreen_state();
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glViewport(0, 0, p.destination_width(), p.destination_height());
        glBindVertexArray(vao);
        glUseProgram(found->name);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, target.texture); // Targets are reused across formats.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
        glBindTexture(GL_TEXTURE_2D, p.depth ? depth : color);
        if (fast) {
            glBindSampler(0, copy_sampler(p.half_scale));
            glUniform3f(found->rect, float(p.x), float(p.y), p.half_scale ? 2.0f : 1.0f);
        } else {
            glBindSampler(0, 0);
            glUniform2i(found->rect, p.x, p.y);
            if (p.filtered()) {
                const auto [top, bottom] = texture_copy_filter_rows(p);
                glUniform3i(found->filter, p.filter[0], p.filter[1], p.filter[2]);
                glUniform2i(found->rows, top, bottom);
            }
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindSampler(0, 0);
        gl_check("resident EFB texture copy");
    }
    // Newer copies own the covered ranges: their bytes stay.
    static void store_uncovered(const GlesResidentCopy& c, const std::vector<std::uint8_t>& pixels) {
        std::vector<std::uint8_t> kept;
        for (const auto& r : c.covered) kept.insert(kept.end(), c.destination + r.first, c.destination + r.second);
        store_texture_copy(c.plan, {pixels.data(), pixels.size()}, c.destination, c.capacity);
        auto from = kept.data();
        for (const auto& r : c.covered) { std::memcpy(c.destination + r.first, from, r.second - r.first); from += r.second - r.first; }
    }
    void materialize(const GlesResidentCopy& c) {
        // A broken stamp means the game reused the memory: the copy no longer exists there.
        if (!c.intact()) return;
        const auto width = c.plan.destination_width(), height = c.plan.destination_height();
        if (!compute_readback) {
            std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4);
            glBindFramebuffer(GL_FRAMEBUFFER, c.framebuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            gl_check("resident texture-copy materialization");
            store_uncovered(c, pixels);
            return;
        }
        const auto bytes = GLsizeiptr(width) * height * 4;
        if (bytes > materialize_bytes) {
            if (!materialize_buffer) glGenBuffers(1, &materialize_buffer);
            glBindBuffer(GL_COPY_READ_BUFFER, materialize_buffer);
            glBufferData(GL_COPY_READ_BUFFER, bytes, nullptr, GL_STREAM_READ);
            materialize_bytes = bytes;
        }
        store_texels(c.texture, materialize_buffer, 0, 0, width, height, 0, width);
        const auto done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!done) throw std::runtime_error("resident texture-copy fence allocation failed");
        const auto wait = glClientWaitSync(done, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
        glDeleteSync(done);
        if (wait != GL_ALREADY_SIGNALED && wait != GL_CONDITION_SATISFIED)
            throw std::runtime_error("resident texture-copy materialization did not retire within one second");
        glBindBuffer(GL_COPY_READ_BUFFER, materialize_buffer);
        const auto* mapped = static_cast<const std::uint8_t*>(glMapBufferRange(GL_COPY_READ_BUFFER, 0, bytes, GL_MAP_READ_BIT));
        if (!mapped) throw std::runtime_error("resident texture-copy materialization map failed");
        // Panfrost maps buffers write-combined: one bulk copy, then the encoder reads cached memory.
        std::vector<std::uint8_t> pixels(mapped, mapped + bytes);
        glUnmapBuffer(GL_COPY_READ_BUFFER); glBindBuffer(GL_COPY_READ_BUFFER, 0);
        store_uncovered(c, pixels);
        gl_check("resident texture-copy materialization");
    }
    void bound_frames() {
        if (!frames.empty()) {
            const auto wait = glClientWaitSync(frames.front(), GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
            if (wait != GL_ALREADY_SIGNALED && wait != GL_CONDITION_SATISFIED)
                throw std::runtime_error("GLES previous frame did not retire within one second");
            glDeleteSync(frames.front()); frames.clear();
        }
        const auto fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!fence) throw std::runtime_error("GLES frame fence allocation failed");
        frames.push_back(fence);
    }
};

void GlesRenderer::Impl::pack_efb(GLuint target, int x, int y, int width, int height, int scale, bool with_color) {
    if (!depth_read_program) {
        GlesDrawPipeline p;
        p.vertex="#version 310 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
        p.fragment="#version 310 es\nprecision highp float;precision highp int;uniform highp sampler2D uDepth;uniform highp sampler2D uColor;"
            "uniform ivec4 uPosition;layout(location=0)out vec4 color;\n"
            "void main(){ivec2 p=ivec2(gl_FragCoord.xy);"
            "if(p.x>=uPosition.w){color=texelFetch(uColor,uPosition.xy+ivec2(p.x-uPosition.w,p.y)*uPosition.z,0);return;}"
            "uint z=uint(roundEven(texelFetch(uDepth,uPosition.xy+p*uPosition.z,0).r*16777215.0));"
            "color=vec4(uvec4((z>>16u)&255u,(z>>8u)&255u,z&255u,255u))/255.0;}";
        depth_read_program=link_program(p);++stats.shader_compiles;
        glUseProgram(depth_read_program);
        glUniform1i(glGetUniformLocation(depth_read_program,"uDepth"),0);
        glUniform1i(glGetUniformLocation(depth_read_program,"uColor"),1);
        depth_read_position=glGetUniformLocation(depth_read_program,"uPosition");
    }
    fullscreen_state();glBindFramebuffer(GL_FRAMEBUFFER,target);
    glViewport(0,0,with_color?2*width:width,height);glBindVertexArray(vao);glUseProgram(depth_read_program);
    glUniform4i(depth_read_position,x,y,scale,width);
    glActiveTexture(GL_TEXTURE1);glBindSampler(1,0);glBindTexture(GL_TEXTURE_2D,color);
    glActiveTexture(GL_TEXTURE0);glBindSampler(0,0);glBindTexture(GL_TEXTURE_2D,depth);
    glDrawArrays(GL_TRIANGLES,0,3);
}
void GlesRenderer::Impl::read_depth(int x, int y, int width, int height, std::uint8_t* pixels, int snapshot_index) {
    if (snapshot_index >= 0) {
        const auto& snapshot = snapshots.at(static_cast<std::size_t>(snapshot_index));
        const int scale = snapshot_scale();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, snapshot.framebuffer);
        for (int row = 0; row < height; ++row) for (int column = 0; column < width; ++column)
            glReadPixels((x + column) / scale, (y + row) / scale, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                         pixels + (std::size_t(row) * width + column) * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        gl_check("native D24 snapshot query");
        return;
    } else {
        copy_surface();
        pack_efb(copy_framebuffer, x, y, width, height);
        x = 0; y = 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
    glReadPixels(x,y,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    gl_check("native D24 pixel query");
}
GlesRenderer::GlesRenderer(GlesLimits limits) : impl_(std::make_unique<Impl>(limits)) { impl_->initialize(); clear({{0,0,0}}); }
GlesRenderer::~GlesRenderer() = default;
void GlesRenderer::clear(std::array<std::uint8_t, 3> color) {
    auto& g = *impl_; g.require_ready();
    try {
        glBindFramebuffer(GL_FRAMEBUFFER, g.framebuffer);
        glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDepthMask(GL_TRUE);
        glClearColor(color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1); glClearDepthf(1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); gl_check("EFB clear"); g.rgba6 = false;
    } catch (...) { g.failed = true; throw; }
}
void GlesRenderer::clear_efb(const EfbClear& operation) {
    auto& g = *impl_; g.require_ready();
    ++g.efb_writes;
    const auto& r = operation.rectangle;
    if (operation.efb != g.limits.efb) throw std::invalid_argument("EFB clear planned for another EFB layout");
    if (r.x<0 || r.y<0 || r.width<0 || r.height<0 || r.x>g.efb_width || r.y>g.efb_height ||
        r.width>g.efb_width-r.x || r.height>g.efb_height-r.y || operation.depth>0xffffff)
        throw std::invalid_argument("invalid native GLES EFB clear rectangle");
    if (!r.width || !r.height) return;
    g.execute([&g, operation] { g.clear_efb_gl(operation); }, false);
}
void GlesRenderer::begin_offscreen() {
    auto& g = *impl_; g.require_ready();
    if (g.offscreen_open) throw std::logic_error("offscreen pass already open");
    if (g.bloom_open) throw std::logic_error("offscreen pass inside native bloom");
    g.offscreen_open = true;
    ++g.efb_writes;
    g.execute([&g] { g.begin_offscreen_gl(); }, false);
}
void GlesRenderer::end_offscreen() {
    auto& g = *impl_; g.require_ready();
    if (!g.offscreen_open) throw std::logic_error("offscreen pass ended without begin");
    g.offscreen_open = false;
    ++g.efb_writes;
    g.execute([&g] {
        g.swap_offscreen();
        glBindFramebuffer(GL_FRAMEBUFFER, g.framebuffer);
        gl_check("offscreen end");
    }, false);
}
void GlesRenderer::Impl::begin_offscreen_gl() {
    if (!offscreen_color) {
        glGenTextures(1, &offscreen_color); glBindTexture(GL_TEXTURE_2D, offscreen_color);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, efb_width, efb_height);
        glGenTextures(1, &offscreen_depth); glBindTexture(GL_TEXTURE_2D, offscreen_depth);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, efb_width, efb_height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    }
    swap_offscreen();
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("offscreen framebuffer is incomplete");
    fullscreen_state();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 0); glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl_check("offscreen begin");
}
void GlesRenderer::begin_bloom() {
    auto& g = *impl_; g.require_ready();
    if (g.bloom_open) throw std::logic_error("native bloom already open");
    if (g.offscreen_open) throw std::logic_error("native bloom inside an offscreen pass");
    g.bloom_open = true;
    g.execute([&g] { g.begin_bloom_gl(); }, false);
}
void GlesRenderer::end_bloom(const EfbBloom& bloom) {
    auto& g = *impl_; g.require_ready();
    if (!g.bloom_open) throw std::logic_error("native bloom ended without begin");
    g.bloom_open = false;
    g.execute([&g, bloom] { g.end_bloom_gl(bloom); }, false);
}
void GlesRenderer::Impl::bloom_resources() {
    auto& b = bloom;
    if (b.scene) return;
    const auto storage = [](GLuint& name, int width, int height) {
        glGenTextures(1, &name); glBindTexture(GL_TEXTURE_2D, name);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    };
    storage(b.scene, efb_width, efb_height);
    const auto sizes = bloom_sizes();
    glGenFramebuffers(3, b.framebuffers);
    for (unsigned i = 0; i < 3; ++i) {
        storage(b.textures[i], sizes[i][0], sizes[i][1]);
        glBindFramebuffer(GL_FRAMEBUFFER, b.framebuffers[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.textures[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("native bloom framebuffer is incomplete");
    }
    const std::string vertex = "#version 310 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
                               "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
    const std::string header = "#version 310 es\nprecision highp float;\nuniform highp sampler2D uImage;\n"
                               "layout(location=0) out vec4 color;\n" + efb_size_glsl();
    // Each quarter-size texel is the 4x4 box of its EFB block (a half-scale copy sampled
    // at half size), kept where its I8 luminance exceeds the threshold.
    const std::string mask = header + "uniform float uThreshold;\nvoid main(){\n"
        " vec2 base=floor(gl_FragCoord.xy)*4.0;vec2 texel=1.0/uEfb;\n"
        " vec3 c=(texture(uImage,(base+vec2(1.0,1.0))*texel).rgb+texture(uImage,(base+vec2(3.0,1.0))*texel).rgb+\n"
        "  texture(uImage,(base+vec2(1.0,3.0))*texel).rgb+texture(uImage,(base+vec2(3.0,3.0))*texel).rgb)*0.25;\n"
        " vec3 q=floor(c*255.0+0.5);float y=floor((66.0*q.r+129.0*q.g+25.0*q.b+128.0)/256.0)+16.0;\n"
        " color=vec4(y>uThreshold?q/255.0:vec3(0.0),1.0);\n}\n";
    // Tap sums never exceed one per draw, so only the accumulated EFB value clamps.
    const auto blur = [&](unsigned taps, std::array<int, 2> size) {
        return header + "uniform float uWeight;\nuniform vec2 uOffsets[" + std::to_string(taps) + "];\nvoid main(){\n"
               " vec2 p=gl_FragCoord.xy/vec2(" + std::to_string(size[0]) + ".0," + std::to_string(size[1]) + ".0);vec3 s=vec3(0.0);\n"
               " for(int i=0;i<" + std::to_string(taps) + ";++i)s+=texture(uImage,p+uOffsets[i]).rgb;\n"
               " color=vec4(min(s*uWeight,vec3(1.0)),1.0);\n}\n";
    };
    const std::string add = header + "uniform highp sampler2D uCoarse;\nuniform float uIntensity;\nvoid main(){\n"
        " vec2 p=gl_FragCoord.xy/uEfb;\n"
        " color=vec4(min((texture(uImage,p).rgb+texture(uCoarse,p).rgb)*uIntensity,vec3(1.0)),0.0);\n}\n";
    const std::string fragments[4] = {mask, blur(12, sizes[1]), blur(36, sizes[2]), add};
    for (unsigned i = 0; i < 4; ++i) {
        GlesDrawPipeline pipeline;
        pipeline.vertex = vertex; pipeline.fragment = fragments[i];
        b.programs[i] = link_program(pipeline);
        ++stats.shader_compiles;
        glUseProgram(b.programs[i]);
        glUniform1i(glGetUniformLocation(b.programs[i], "uImage"), 0);
    }
    b.threshold = glGetUniformLocation(b.programs[0], "uThreshold");
    for (unsigned i = 0; i < 2; ++i) {
        b.weights[i] = glGetUniformLocation(b.programs[1 + i], "uWeight");
        b.offsets[i] = glGetUniformLocation(b.programs[1 + i], "uOffsets");
    }
    glUniform1i(glGetUniformLocation(b.programs[3], "uCoarse"), 1);
    b.intensity = glGetUniformLocation(b.programs[3], "uIntensity");
    gl_check("native bloom resources");
}
void GlesRenderer::Impl::begin_bloom_gl() {
    bloom_resources();
    std::swap(color, bloom.scene);
    attach_color();
    // Cleared where the mask draws render, so the clear starts their render pass.
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    fullscreen_state();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    gl_check("native bloom begin");
}
void GlesRenderer::Impl::end_bloom_gl(const EfbBloom& p) {
    auto& b = bloom;
    fullscreen_state();
    glBindVertexArray(vao);
    const auto linear = copy_sampler(true);
    glBindSampler(0, linear); glBindSampler(1, linear);
    glActiveTexture(GL_TEXTURE0);
    const GLenum attachment = GL_COLOR_ATTACHMENT0;
    const auto pass = [&](unsigned level, GLuint source, int width, int height) {
        glBindFramebuffer(GL_FRAMEBUFFER, b.framebuffers[level]);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &attachment);
        glViewport(0, 0, width, height);
        glBindTexture(GL_TEXTURE_2D, source);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    };
    glUseProgram(b.programs[0]); glUniform1f(b.threshold, p.threshold);
    const auto sizes = bloom_sizes();
    pass(0, color, sizes[0][0], sizes[0][1]);
    // GX texture t grows downwards; these levels keep the EFB's lower-left origin.
    std::array<float, 72> offsets{};
    for (unsigned i = 0; i < 12; ++i) { offsets[i * 2] = p.level1[i][0]; offsets[i * 2 + 1] = -p.level1[i][1]; }
    glUseProgram(b.programs[1]); glUniform1f(b.weights[0], p.level1_weight / 255.0f);
    glUniform2fv(b.offsets[0], 12, offsets.data());
    pass(1, b.textures[0], sizes[1][0], sizes[1][1]);
    for (unsigned i = 0; i < 36; ++i) { offsets[i * 2] = p.level2[i][0]; offsets[i * 2 + 1] = -p.level2[i][1]; }
    glUseProgram(b.programs[2]); glUniform1f(b.weights[1], p.level2_weight / 255.0f);
    glUniform2fv(b.offsets[1], 36, offsets.data());
    pass(2, b.textures[1], sizes[2][0], sizes[2][1]);
    std::swap(color, b.scene);
    attach_color();
    // The colour and depth framebuffer, as the game's draws and depth clear that follow
    // use it: switching framebuffers would end this render pass and reload the EFB.
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, efb_width, efb_height);
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_ONE, GL_ONE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glUseProgram(b.programs[3]); glUniform1f(b.intensity, p.intensity / 255.0f);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, b.textures[2]);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, b.textures[1]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindSampler(0, 0); glBindSampler(1, 0);
    gl_check("native bloom");
    ++stats.blooms;
}
void GlesRenderer::Impl::clear_efb_gl(const EfbClear& operation) {
    auto& g = *this;
    const auto& r = operation.rectangle;
    const bool format_change = operation.rgba6 != g.rgba6;
    const bool replaces_color = !r.x && !r.y && r.width==efb_width && r.height==efb_height &&
        operation.color_write && (!operation.rgba6 || operation.alpha_write);
    try {
        if (!replaces_color) g.reinterpret_format(operation.rgba6);
        glBindFramebuffer(GL_FRAMEBUFFER,g.framebuffer);
        glViewport(0,0,efb_width,efb_height); glEnable(GL_SCISSOR_TEST); glScissor(r.x,r.y,r.width,r.height);
        glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDepthMask(operation.depth_write);
        glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); glDisable(GL_SAMPLE_COVERAGE);
        glClearDepthf(operation.depth/16777215.0f);
        glColorMask(operation.color_write,operation.color_write,operation.color_write,operation.alpha_write || (format_change && !operation.rgba6));
        const bool constant = std::all_of(operation.tile.begin(),operation.tile.end(),
            [&](const auto& c){return c==operation.tile[0];});
        if (constant) {
            const auto& c=operation.tile[0];glClearColor(c[0]/255.0f,c[1]/255.0f,c[2]/255.0f,c[3]/255.0f);
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);gl_check("MainLoop constant EFB clear");g.rgba6=operation.rgba6;return;
        }
        if (!g.clear_program) {
            GlesDrawPipeline p;
            p.vertex = "#version 310 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
            p.fragment = "#version 310 es\nprecision highp float;precision highp int;\nuniform ivec4 uClear[16];layout(location=0) out vec4 color;\nvoid main(){ivec2 p=ivec2(gl_FragCoord.xy);int i=((GX_EFB_TOP-p.y)&3)*4+(p.x&3);color=vec4(uClear[i])/255.0;}";
            g.clear_program = g.link_efb_program(p);
            for (unsigned i=0;i<16;++i)
                g.clear_colors[i] = glGetUniformLocation(g.clear_program,("uClear["+std::to_string(i)+"]").c_str());
        }
        if(operation.depth_write) glClear(GL_DEPTH_BUFFER_BIT);
        glBindVertexArray(g.vao); glUseProgram(g.clear_program);
        for (unsigned i=0;i<16;++i) {
            const auto& c = operation.tile[i]; glUniform4i(g.clear_colors[i],c[0],c[1],c[2],c[3]);
        }
        glDrawArrays(GL_TRIANGLES,0,3); gl_check("MainLoop EFB clear"); g.rgba6=operation.rgba6;
    } catch (...) { g.failed = true; throw; }
}
void GlesRenderer::draw_direct(const State& state, const Draw& command, MemoryReader& memory, TextureCmprModel cmpr) {
    auto& g = *impl_; g.check_owner();
    ++g.efb_writes;
    if (g.failed) throw std::runtime_error("GLES renderer failed; destroy it before further rendering");
    if (state.efb() != g.limits.efb) throw std::invalid_argument("GX state uses another EFB layout");
    g.ensure_direct();
    g.direct->draw(state, command, memory, cmpr);
}
void GlesRenderer::draw(const State& state, const Draw& command, MemoryReader& memory, TextureCmprModel cmpr) {
    ++impl_->efb_writes;
    auto& g = *impl_; g.require_ready();
    if (command.merged()) throw std::invalid_argument("GX merged draws need draw_direct");
    if (state.efb() != g.limits.efb) throw std::invalid_argument("GX state uses another EFB layout");
    auto captured = capture_draw(state, command, memory, {}, cmpr, &g.capture_cache, &g.geometry_cache);
    if (!captured) throw std::runtime_error(captured.status.message);
    draw(*captured.packet);
}
void GlesRenderer::Impl::submit(const DrawPacket& packet,const GlesDrawPipeline& p,
                                const std::vector<GpuVertex>& vertices,
                                const std::vector<std::uint16_t>& indices,std::size_t logical_draws) {
    auto& g=*this;
    try {
        if (!p.full_replacement) g.reinterpret_format(p.rgba6_copy);
        auto& program = g.program(p,packet);
        glUseProgram(program.name); glBindFramebuffer(GL_FRAMEBUFFER, g.framebuffer);
        glBindVertexArray(g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo); glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(GpuVertex), vertices.data(), GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(std::uint16_t), indices.data(), GL_STREAM_DRAW);
        if (!g.transform_uploaded || std::memcmp(&g.uploaded_transform, &packet.transform->uniforms, sizeof(TransformUniforms))) {
            glBindBuffer(GL_UNIFORM_BUFFER, g.ubo);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(TransformUniforms), &packet.transform->uniforms, GL_STREAM_DRAW);
            g.uploaded_transform = packet.transform->uniforms; g.transform_uploaded = true;
        }
        // The snapshot holds every block at an offset any uniform buffer alignment divides.
        glBindBufferRange(GL_UNIFORM_BUFFER, 0, g.ubo, transform_shared_offset, transform_shared_bytes);
        glBindBufferRange(GL_UNIFORM_BUFFER, 2, g.ubo, 0, transform_matrix_bytes);
        glBindBufferRange(GL_UNIFORM_BUFFER, 3, g.ubo, transform_post_offset, transform_post_bytes);
        for (unsigned i = 0; i < 4; ++i) {
            const auto& r = packet.tev->uniforms.registers[i]; const auto& k = packet.tev->uniforms.konst[i];
            if (program.registers[i].update(r)) glUniform4i(program.registers[i].location, r[0], r[1], r[2], r[3]);
            if (program.konst[i].update(k)) glUniform4i(program.konst[i].location, k[0], k[1], k[2], k[3]);
        }
        if (p.rgba6_copy && program.pixel.update(p.pixel_controls)) glUniform4ui(program.pixel.location,p.pixel_controls[0],p.pixel_controls[1],p.pixel_controls[2],p.pixel_controls[3]);
        if (program.alpha.update(packet.tev->uniforms.alpha_reference))
            glUniform2i(program.alpha.location, packet.tev->uniforms.alpha_reference[0], packet.tev->uniforms.alpha_reference[1]);
        if (packet.tev->pipeline.z_texture_op && program.z_bias.update({{packet.tev->uniforms.z_texture_bias}}))
            glUniform1ui(program.z_bias.location, packet.tev->uniforms.z_texture_bias);
        for (unsigned i = 0; i < 16; ++i) if (program.scales[i].update(p.coordinate_scale[i]))
            glUniform2f(program.scales[i].location, p.coordinate_scale[i][0], p.coordinate_scale[i][1]);
        if (packet.indirect->lookup_mask) {
            for (unsigned i = 0; i < 16; ++i) if (program.image_sizes[i].update(p.image_size[i]))
                glUniform2f(program.image_sizes[i].location,p.image_size[i][0],p.image_size[i][1]);
            for (unsigned i = 0; i < 4; ++i) {
                const auto& scale = p.indirect_scale[i];
                if (program.indirect_scales[i].update(scale))
                    glUniform4f(program.indirect_scales[i].location,scale[0],scale[1],scale[2],scale[3]);
            }
            for (unsigned i = 0; i < 6; ++i) {
                const auto& row = packet.indirect->matrices[i/2].rows[i%2];
                if (program.indirect_rows[i].update(row))
                    glUniform4i(program.indirect_rows[i].location,row[0],row[1],row[2],row[3]);
            }
        }
        for (unsigned i=0;i<8;++i) if (packet.textures[i]) {
            const auto& lod=p.sampler_lod[i];
            if (program.sampler_lod[i].update(lod))
                glUniform4f(program.sampler_lod[i].location,lod[0],lod[1],lod[2],lod[3]);
        }
        g.bind_textures(packet);
        const auto& vp = p.raster.viewport; const auto& sc = p.raster.scissor;
        glViewport(vp.x, vp.y, vp.width, vp.height); glEnable(GL_SCISSOR_TEST); glScissor(sc.x, sc.y, sc.width, sc.height);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_CULL_FACE); glDisable(GL_DITHER);
        glDisable(GL_STENCIL_TEST); glDisable(GL_RASTERIZER_DISCARD); glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); glDisable(GL_SAMPLE_COVERAGE);
        glDepthRangef(p.raster.depth_range[0],p.raster.depth_range[1]);
        glDepthMask(p.raster.depth_write);
        if (p.raster.depth_test) {
            constexpr GLenum comparison[] = {GL_NEVER,GL_LESS,GL_EQUAL,GL_LEQUAL,GL_GREATER,GL_NOTEQUAL,GL_GEQUAL,GL_ALWAYS};
            glEnable(GL_DEPTH_TEST); glDepthFunc(comparison[p.raster.depth_compare]);
        }
        if (p.raster.cull) {
            glEnable(GL_CULL_FACE); glFrontFace(GL_CW);
            glCullFace(p.raster.cull==1 ? GL_BACK : p.raster.cull==2 ? GL_FRONT : GL_FRONT_AND_BACK);
        }
        glColorMask(p.raster.color_write, p.raster.color_write, p.raster.color_write, p.alpha_write || (!p.rgba6_copy && p.full_replacement));
        if (p.raster.blend) {
            glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(factor(p.raster.source_factor, true), factor(p.raster.destination_factor, false), GL_ONE, GL_ZERO);
        } else glDisable(GL_BLEND);
        glDrawElements(GL_TRIANGLES, indices.size(), GL_UNSIGNED_SHORT, nullptr);
        gl_check("GX draw"); g.rgba6=p.rgba6_copy; g.stats.draws+=logical_draws; ++g.stats.submissions;
    } catch (...) { g.failed = true; throw; }
}
void GlesRenderer::draw(const DrawPacket& packet) {
    ++impl_->efb_writes;
    auto& g=*impl_;g.require_ready();
    if(packet.raster && packet.raster->efb!=g.limits.efb) throw std::invalid_argument("draw planned for another EFB layout");
    const auto planned=plan_gles_draw(packet,g.fetch,false);
    if(!planned) throw std::runtime_error(planned.status.message);
    const auto& p=planned.pipeline;
    if(p.raster.viewport.width>g.max_viewport[0] || p.raster.viewport.height>g.max_viewport[1])
        throw std::runtime_error("GX viewport exceeds driver limit");
    if(packet.vertices->indices().empty() || p.discarded) return;
    std::vector<GpuVertex> vertices;vertices.reserve(packet.vertices->vertices().size());
    append_vertices(vertices,*packet.vertices);
    g.submit(packet,p,vertices,packet.vertices->indices(),1);
}
void GlesRenderer::draw_batch(const DrawPacket* packets,std::size_t count) {
    ++impl_->efb_writes;
    auto& g=*impl_;g.require_ready();
    if(count>4096 || (!packets && count)) throw std::invalid_argument("invalid GLES draw batch span");
    std::vector<GlesDrawPipeline> plans;
    if (count >= 64) {
        if (!g.preparation) g.preparation = std::make_unique<DrawPreparation>();
        plans = g.preparation->plan(packets, count, g.fetch);
    } else {
        plans.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto planned = plan_gles_draw(packets[i], g.fetch, false);
            if (!planned) throw std::runtime_error(planned.status.message);
            plans.push_back(std::move(planned.pipeline));
        }
    }
    for(std::size_t i=0;i<count;++i) {
        if(packets[i].vertices->vertices().size()>65535 || packets[i].vertices->indices().size()>3*65535)
            throw std::length_error("GLES draw exceeds batch geometry bounds");
        if(plans[i].raster.viewport.width>g.max_viewport[0] || plans[i].raster.viewport.height>g.max_viewport[1])
            throw std::runtime_error("GX viewport exceeds driver limit");
    }
    for(std::size_t first=0;first<count;) {
        if(packets[first].vertices->indices().empty() || plans[first].discarded) {++first;continue;}
        std::size_t end=first+1,vertex_count=packets[first].vertices->vertices().size(),index_count=packets[first].vertices->indices().size();
        while(end<count && !packets[end].vertices->indices().empty() && !plans[end].discarded &&
              packets[end].vertices->vertices().size()<=65535-vertex_count &&
              packets[end].vertices->indices().size()<=3*65535-index_count &&
              batch_compatible(packets[first],plans[first],packets[end],plans[end])) {
            vertex_count+=packets[end].vertices->vertices().size();
            index_count+=packets[end].vertices->indices().size();++end;
        }
        std::vector<GpuVertex> vertices;vertices.reserve(vertex_count);
        std::vector<std::uint16_t> indices;indices.reserve(index_count);
        for(std::size_t i=first;i<end;++i) {
            const auto base=vertices.size();append_vertices(vertices,*packets[i].vertices);
            for(auto index:packets[i].vertices->indices()) indices.push_back(static_cast<std::uint16_t>(base+index));
        }
        g.submit(packets[first],plans[first],vertices,indices,end-first);first=end;
    }
}

void GlesRenderer::present(int width, int height) {
    auto& g = *impl_; g.require_ready();
    if (width <= 0 || height <= 0) throw std::invalid_argument("invalid drawable extent");
    g.execute([&g, width, height] {
        try {
            glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g.framebuffer); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, g.efb_width, g.efb_height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            g.bound_frames(); gl_check("EFB presentation");
            if (g.swap) g.swap();
        } catch (...) { g.failed = true; throw; }
    }, false);
    if (g.direct) g.direct->next_frame();
}
void GlesRenderer::copy_display(const DisplayCopy &p, std::uint8_t *destination, std::size_t capacity) {
    ++impl_->efb_writes;
    auto &g = *impl_;
    g.require_ready();
    if (p.efb != g.limits.efb) throw std::invalid_argument("display copy planned for another EFB layout");
    const auto required = display_copy_bytes(p);
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    if (!destination || (address & 31) || capacity < required || capacity > UINTPTR_MAX - address)
        throw std::invalid_argument("invalid native display-copy destination span");
    if (p.clear && (p.clear->rgba6 != p.rgba6 || p.clear->depth > 0xffffff || p.clear->rectangle.x != p.x ||
                    p.clear->rectangle.y != g.efb_height - p.y - p.height ||
                    p.clear->rectangle.width != p.width || p.clear->rectangle.height != p.height))
        throw std::invalid_argument("invalid display-copy clear operation");
    const auto serial = ++g.display_copies;
    const auto old = std::find_if(g.copy_serials.begin(), g.copy_serials.end(),
                                  [&](const auto& entry) { return entry.first == destination; });
    if (old != g.copy_serials.end()) g.copy_serials.erase(old);
    else if (g.copy_serials.size() == 8) g.copy_serials.pop_front();
    g.copy_serials.emplace_back(destination, serial);
    g.execute([&g, p, destination] { g.copy_display_gl(p, destination); }, false);
}
void GlesRenderer::Impl::copy_display_gl(const DisplayCopy& p, std::uint8_t* destination) {
    auto& g = *this;
    try {
        g.reinterpret_format(p.rgba6);
        g.copy_surface();
        g.display_programs(true);
        g.fullscreen_state();
        auto xfb = std::find_if(g.xfbs.begin(), g.xfbs.end(), [&](const auto& x) { return x.data == destination; });
        const bool rgb = g.rgb_xfb();
        if (rgb) g.rgb_display_programs();
        const int texels = rgb ? p.width : p.width / 2;
        if (g.direct) {
            if (xfb == g.xfbs.end() || xfb->width != texels || xfb->height != p.height) {
                if (xfb != g.xfbs.end()) { glDeleteFramebuffers(1, &xfb->framebuffer); glDeleteTextures(1, &xfb->texture); g.xfbs.erase(xfb); }
                Impl::Xfb created{destination, 0, texels, p.height};
                glGenTextures(1, &created.texture); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, created.texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, created.width, created.height);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glGenFramebuffers(1, &created.framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, created.framebuffer);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, created.texture, 0);
                g.xfbs.push_back(created); xfb = g.xfbs.end() - 1;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, xfb->framebuffer);
        } else {
            if (xfb != g.xfbs.end()) { glDeleteFramebuffers(1, &xfb->framebuffer); glDeleteTextures(1, &xfb->texture); g.xfbs.erase(xfb); }
            glBindFramebuffer(GL_FRAMEBUFFER, g.copy_framebuffer);
        }
        glViewport(0, 0, texels, p.height);
        glBindVertexArray(g.vao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g.color);
        const bool taps = rgb && p.y == 0 && p.height == efb_height && p.filter[0] == p.filter[2] &&
                          p.filter[0] + p.filter[1] + p.filter[2] == 64;
        if (taps) {
            glUseProgram(g.copy_rgb_taps_program);
            glBindSampler(0, g.copy_sampler(true));
            glUniform3f(g.copy_rgb_taps, float(p.x), 0.0f, float(p.filter[0]) / 32.0f);
        } else {
            glUseProgram(rgb ? g.copy_rgb_program : g.copy_program);
            glBindSampler(0, 0);
            glUniform4i(rgb ? g.copy_rgb_geometry : g.copy_geometry, p.x, p.y, p.width, p.height);
            glUniform4i(rgb ? g.copy_rgb_filter : g.copy_filter, p.filter[0], p.filter[1], p.filter[2],
                        int(p.clamp_top) | (int(p.clamp_bottom) << 1));
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindSampler(0, 0);
        if (!g.direct) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_PACK_ROW_LENGTH, p.stride / 4);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glReadPixels(0, 0, p.width / 2, p.height, GL_RGBA, GL_UNSIGNED_BYTE, destination);
        }
        gl_check("native EFB to canonical YUYV copy");
        if (p.clear)
            g.clear_efb_gl(*p.clear);
        ++g.stats.display_copies;
    } catch (...) {
        g.failed = true;
        throw;
    }
}
void GlesRenderer::copy_texture(const TextureCopy& p, std::uint8_t* destination, std::size_t capacity) {
    ++impl_->efb_writes;
    auto& g = *impl_; g.require_ready();
    if (p.efb != g.limits.efb) throw std::invalid_argument("texture copy planned for another EFB layout");
    const auto required = texture_copy_bytes(p);
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    if (!destination || (address & 31) || capacity < required || capacity > UINTPTR_MAX-address)
        throw std::invalid_argument("invalid native texture-copy destination span");
    if (p.clear && (p.clear->rgba6 != p.rgba6 || p.clear->depth > 0xffffff ||
        p.clear->rectangle.x != p.x || p.clear->rectangle.y != g.efb_height-p.y-p.height ||
        p.clear->rectangle.width != p.width || p.clear->rectangle.height != p.height))
        throw std::invalid_argument("invalid texture-copy clear operation");
    if (p.strip) throw std::runtime_error("EFB copies below the displayed frame need the direct renderer");
    std::vector<std::uint8_t> pixels(std::size_t(p.width)*p.height*4);
    try {
        if (p.depth && p.filtered()) {
            const auto [top, bottom] = texture_copy_filter_rows(p);
            std::vector<std::uint8_t> rows(std::size_t(p.width)*(bottom-top+1)*4);
            g.read_depth(p.x,g.efb_height-1-bottom,p.width,bottom-top+1,rows.data(),g.selected_snapshot);
            pixels = filter_texture_copy(p,{rows.data(),rows.size()},top);
        } else if (p.depth) {
            g.read_depth(p.x,g.efb_height-p.y-p.height,p.width,p.height,pixels.data(),g.selected_snapshot);
        } else {
            g.reinterpret_format(p.rgba6);
            glBindFramebuffer(GL_READ_FRAMEBUFFER,g.framebuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
            glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ROW_LENGTH,0);
            glPixelStorei(GL_PACK_SKIP_ROWS,0); glPixelStorei(GL_PACK_SKIP_PIXELS,0);
            if (p.filtered()) {
                const auto [top, bottom] = texture_copy_filter_rows(p);
                std::vector<std::uint8_t> rows(std::size_t(p.width)*(bottom-top+1)*4);
                glReadPixels(p.x,g.efb_height-1-bottom,p.width,bottom-top+1,GL_RGBA,GL_UNSIGNED_BYTE,rows.data());
                pixels = filter_texture_copy(p,{rows.data(),rows.size()},top);
            } else {
                glReadPixels(p.x,g.efb_height-p.y-p.height,p.width,p.height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            }
            gl_check("native EFB texture-copy readback");
        }
        encode_texture_copy(p,{pixels.data(),pixels.size()},destination,capacity);
        if (g.direct) g.direct->invalidate(destination, required);
        if (p.clear) clear_efb(*p.clear);
        ++g.stats.texture_copies;
    } catch (...) { g.failed = true; throw; }
}
void GlesRenderer::copy_texture_resident(const TextureCopy& p, std::uint8_t* destination, std::size_t capacity, std::uint32_t token) {
    ++impl_->efb_writes;
    auto& g = *impl_; g.require_ready();
    if (!g.direct) { copy_texture(p, destination, capacity); return; }
    if (p.efb != g.limits.efb) throw std::invalid_argument("texture copy planned for another EFB layout");
    const auto required = texture_copy_bytes(p);
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    if (!destination || (address & 31) || capacity < required || capacity > UINTPTR_MAX-address)
        throw std::invalid_argument("invalid native texture-copy destination span");
    if (p.clear && (p.clear->rgba6 != p.rgba6 || p.clear->depth > 0xffffff ||
        p.clear->rectangle.x != p.x || p.clear->rectangle.y != g.efb_height-p.y-p.height ||
        p.clear->rectangle.width != p.width || p.clear->rectangle.height != p.height))
        throw std::invalid_argument("invalid texture-copy clear operation");
    try {
        const auto target = g.direct->resident_copy(p, token, destination, capacity);
        g.direct->flush();
        g.execute([&g, p, target] {
            try {
                if (!p.depth) g.reinterpret_format(p.rgba6);
                if (p.clear) g.invalidate_cleared(p);
                g.resident_pass(p, target);
                if (p.clear) g.clear_efb_gl(*p.clear);
                ++g.stats.texture_copies;
            } catch (...) { g.failed = true; throw; }
        }, false);
    } catch (...) { g.failed = true; throw; }
}
void GlesRenderer::release_resident(std::uint32_t token, std::size_t bytes) {
    auto& g = *impl_; g.check_owner();
    if (g.direct) g.direct->release(token, bytes);
}
void GlesRenderer::before_dma_read(const void* data, std::size_t bytes) {
    auto& g = *impl_; g.check_owner();
    if (!g.direct) return;
    if (g.failed) throw std::runtime_error("GLES renderer failed; destroy it before further rendering");
    try { g.direct->before_read(data, bytes); } catch (...) { g.failed = true; throw; }
}
void GlesRenderer::before_dma_write(const void* data, std::size_t bytes) {
    auto& g = *impl_; g.check_owner();
    if (!g.direct) return;
    if (g.failed) throw std::runtime_error("GLES renderer failed; destroy it before further rendering");
    try { g.direct->before_write(data, bytes); } catch (...) { g.failed = true; throw; }
}
bool GlesRenderer::resident_in(const void* data, std::size_t bytes) const {
    const auto* direct = impl_->shared_direct.load(std::memory_order_acquire);
    return direct && direct->resident_in(data, bytes);
}
bool GlesRenderer::present_framebuffer(const FramebufferView &f, int width, int height, bool black) {
    auto &g = *impl_;
    g.require_ready();
    if (!black)
        validate_framebuffer(f);
    if (width <= 0 || height <= 0 || width > g.max_viewport[0] || height > g.max_viewport[1])
        throw std::invalid_argument("invalid VI drawable extent");
    const long across = g.limits.aspect_width, down = g.limits.aspect_height;
    if (!std::min<long>(width, height * across / down) * down / across)
        throw std::invalid_argument("empty VI presentation viewport");
    if (g.direct) {
        const auto* data = black ? nullptr : f.bytes.data;
        const Impl::Presented next{data, data ? g.copy_serial(data) : 0, width, height, black};
        if (g.presented == next) return false;
        g.presented = next;
    }
    g.execute([&g, f, width, height, black] { g.present_gl(f, width, height, black); }, false);
    if (g.direct) g.direct->next_frame();
    return true;
}
void GlesRenderer::Impl::present_gl(const FramebufferView& f, int width, int height, bool black) {
    auto& g = *this;
    const long across = g.limits.aspect_width, down = g.limits.aspect_height;
    const int w = int(std::min<long>(width, height * across / down)), h = int(w * down / across),
              left = (width - w) / 2, bottom = (height - h) / 2;
    try {
        g.fullscreen_state();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!black) {
            g.copy_surface();
            g.display_programs(false);
            glActiveTexture(GL_TEXTURE0);
            glBindSampler(0, 0);
            const bool rgb = g.rgb_xfb();
            const auto xfb = std::find_if(g.xfbs.begin(), g.xfbs.end(), [&](const auto& x) {
                return x.data == f.bytes.data && x.width == int(rgb ? f.width : f.width / 2) && x.height == int(f.height);
            });
            const bool resident_rgb = rgb && xfb != g.xfbs.end();
            if (xfb != g.xfbs.end()) glBindTexture(GL_TEXTURE_2D, xfb->texture);
            else {
                glBindTexture(GL_TEXTURE_2D, g.copy_texture);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, f.stride / 4);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width / 2, f.height, GL_RGBA, GL_UNSIGNED_BYTE,
                                f.bytes.data);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(left, bottom, w, h);
            glBindVertexArray(g.vao);
            glUseProgram(resident_rgb ? g.present_rgb_program : g.present_program);
            glUniform4i(resident_rgb ? g.present_rgb_source : g.present_source, f.width, f.height, 0, 0);
            glUniform4i(resident_rgb ? g.present_rgb_destination : g.present_destination, left, bottom, w, h);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        g.bound_frames();
        gl_check("native VI YUYV presentation");
        ++g.stats.presentations;
        if (g.swap) g.swap();
    } catch (...) {
        g.failed = true;
        throw;
    }
}
void GlesRenderer::wait_until_idle() {
    auto& g = *impl_; g.require_ready();
    // Direct draws consume their inputs when recorded and keep copies GPU-resident,
    // so completion needs only GL command order; presentation bounds the queue.
    if (g.direct) return;
    g.execute([&g] {
        GLsync fence = nullptr;
        try {
            fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (!fence) throw std::runtime_error("GLES completion fence allocation failed");
            const auto result = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
            glDeleteSync(fence); fence = nullptr;
            if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED)
                throw std::runtime_error("GLES GPU work did not retire within one second");
            gl_check("GX GPU completion");
        } catch (...) {
            if (fence) glDeleteSync(fence);
            g.failed = true; throw;
        }
    }, true);
}
int GlesRenderer::snapshot_efb() { return snapshot_efb(EfbRegion{}); }
int GlesRenderer::snapshot_efb(const EfbRegion& requested) {
    auto& g = *impl_; g.require_ready();
    if (!g.direct) return -1;
    EfbRegion region;
    if (!requested.empty()) {
        region.x = std::clamp(requested.x, 0, g.efb_width);
        region.y = std::clamp(requested.y, 0, g.efb_height);
        region.width = std::clamp(requested.x + requested.width, 0, g.efb_width) - region.x;
        region.height = std::clamp(requested.y + requested.height, 0, g.efb_height) - region.y;
        if (region.empty()) region.width = region.height = 1;   // still a region: every peek reads the retained snapshot
    }
    const auto covers = [](const EfbRegion& held, const EfbRegion& wanted) {
        return held.empty() || (!wanted.empty() && wanted.x >= held.x && wanted.y >= held.y &&
                                wanted.x + wanted.width <= held.x + held.width && wanted.y + wanted.height <= held.y + held.height);
    };
    if (g.last_snapshot >= 0 && g.last_snapshot_writes == g.efb_writes && g.snapshot_busy[std::size_t(g.last_snapshot)] &&
        covers(g.snapshot_region[std::size_t(g.last_snapshot)], region)) {
        ++g.snapshot_refs[std::size_t(g.last_snapshot)];
        return g.last_snapshot;
    }
    auto free_slot = std::find(g.snapshot_busy.begin(), g.snapshot_busy.end(), false);
    if (free_slot == g.snapshot_busy.end()) {
        if (g.snapshot_busy.size() == Impl::snapshot_slots) { ++g.stats.refused_snapshots; return -1; } // Caller delivers pending snapshots first.
        g.snapshot_busy.push_back(false);
        g.snapshot_refs.push_back(0);
        free_slot = std::prev(g.snapshot_busy.end());
    }
    const auto index = static_cast<std::size_t>(free_slot - g.snapshot_busy.begin());
    *free_slot = true;
    g.snapshot_refs[index] = 1;
    g.last_snapshot = static_cast<int>(index);
    g.last_snapshot_writes = g.efb_writes;
    // The readback covers whole rows of the packed texture between the region's first and
    // last row; within them only the region's depth and colour columns are transferred.
    const int scale = g.snapshot_scale(), columns = g.efb_width / scale, rows = g.efb_height / scale;
    int row0 = 0, row_count = rows, column0 = 0, column_count = columns;
    if (!region.empty()) {
        row0 = (g.efb_height - region.y - region.height) / scale;
        row_count = (g.efb_height - 1 - region.y) / scale - row0 + 1;
        column0 = region.x / scale;
        column_count = (region.x + region.width - 1) / scale - column0 + 1;
    }
    g.snapshot_region[index] = region;
    g.snapshot_mapped_from[index] = std::size_t(row0) * 2 * std::size_t(columns) * 4;
    g.snapshot_mapped_bytes[index] = std::size_t(row_count) * 2 * std::size_t(columns) * 4;
    const auto generation = g.snapshot_generation[index].fetch_add(1, std::memory_order_relaxed) + 1;
    g.execute([&g, index, generation, row0, row_count, column0, column_count] {
        try {
            if (index == g.snapshots.size()) {
                Impl::Snapshot created;
                glActiveTexture(GL_TEXTURE0); glGenTextures(1, &created.texture); glBindTexture(GL_TEXTURE_2D, created.texture);
                const int scale = g.snapshot_scale();
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2 * (g.efb_width / scale), g.efb_height / scale);
                glGenFramebuffers(1, &created.framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, created.framebuffer);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, created.texture, 0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    throw std::runtime_error("EFB snapshot framebuffer is incomplete");
                g.snapshots.push_back(created);
            }
            // Callbacks read single pixels from the readback a frame later, when the pack
            // has long retired, instead of waiting for it now.
            const int scale = g.snapshot_scale();
            auto& snapshot = g.snapshots[index];
            g.pack_efb(snapshot.framebuffer, 0, 0, g.efb_width / scale, g.efb_height / scale, scale, true);
            const auto bytes = g.snapshot_bytes();
            if (!snapshot.pack) {
                glGenBuffers(1, &snapshot.pack); glBindBuffer(GL_PIXEL_PACK_BUFFER, snapshot.pack);
                glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
            }
            if (snapshot.ready) {
                glDeleteSync(snapshot.ready); snapshot.ready = nullptr;
                g.pending_reads.erase(std::remove_if(g.pending_reads.begin(), g.pending_reads.end(),
                    [&](const Impl::PendingRead& r) { return r.index == index; }), g.pending_reads.end());
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, snapshot.pack);
            if (g.snapshot_pixels[index]) { glUnmapBuffer(GL_PIXEL_PACK_BUFFER); g.snapshot_pixels[index] = nullptr; }
            if (g.compute_readback) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                g.store_snapshot(snapshot, row0, row_count, column0, column_count);
            } else {
                const int columns = g.efb_width / scale;
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, snapshot.framebuffer);
                if (column_count == columns && row_count == g.efb_height / scale) {
                    glPixelStorei(GL_PACK_ROW_LENGTH, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                    glReadPixels(0, 0, 2 * columns, row_count, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                } else {
                    // Into the buffer at the same offsets as a whole readback: depth, then colour.
                    glPixelStorei(GL_PACK_ROW_LENGTH, 2 * columns); glPixelStorei(GL_PACK_SKIP_ROWS, row0);
                    for (int block = 0; block < 2; ++block) {
                        glPixelStorei(GL_PACK_SKIP_PIXELS, block * columns + column0);
                        glReadPixels(block * columns + column0, row0, column_count, row_count, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    }
                    glPixelStorei(GL_PACK_ROW_LENGTH, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                }
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g.framebuffer);
            snapshot.ready = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (!snapshot.ready) throw std::runtime_error("EFB snapshot fence allocation failed");
            g.pending_reads.push_back({index, generation});
            gl_check("EFB snapshot");
        } catch (...) { g.failed = true; throw; }
    }, false);
    return static_cast<int>(index);
}
bool GlesRenderer::snapshot_ready(int index) const { return impl_->snapshot_ready(index); }
void GlesRenderer::wait_snapshot(int index) {
    auto& g = *impl_; g.check_owner();
    if (index < 0 || index >= static_cast<int>(g.snapshot_busy.size()) || !g.snapshot_busy[std::size_t(index)])
        throw std::out_of_range("unknown EFB snapshot");
    if (g.snapshot_ready(index)) return;
    ++g.stats.peek_waits;
    g.execute([&] { try { g.collect_snapshots(index); } catch (...) { g.failed = true; throw; } }, true);
}
namespace {
std::uint32_t packed_depth(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2]; }
std::uint32_t argb(const std::uint8_t* p) { return (std::uint32_t(p[3]) << 24) | packed_depth(p); }
} // namespace
bool GlesRenderer::peek_depth_snapshot(int index, int x, int y, std::uint32_t& value) const {
    const auto& g = *impl_;
    if (x < 0 || x >= g.efb_width || y < 0 || y >= g.efb_height || !g.snapshot_ready(index) ||
        !g.snapshot_region[std::size_t(index)].contains(x, y))
        return false;
    value = packed_depth(g.snapshot_pixel(std::size_t(index), x, y, false));
    return true;
}
bool GlesRenderer::peek_color_snapshot(int index, int x, int y, std::uint32_t& value) const {
    const auto& g = *impl_;
    if (x < 0 || x >= g.efb_width || y < 0 || y >= g.efb_height || !g.snapshot_ready(index) ||
        !g.snapshot_region[std::size_t(index)].contains(x, y))
        return false;
    value = argb(g.snapshot_pixel(std::size_t(index), x, y, true));
    return true;
}
void GlesRenderer::select_snapshot(int index) {
    auto& g = *impl_; g.check_owner();
    if (index >= static_cast<int>(g.snapshot_busy.size()) || (index >= 0 && !g.snapshot_busy[index]))
        throw std::out_of_range("unknown EFB snapshot");
    g.selected_snapshot = index;
}
void GlesRenderer::release_snapshot(int index) {
    auto& g = *impl_; g.check_owner();
    if (index < 0 || index >= static_cast<int>(g.snapshot_busy.size())) throw std::out_of_range("unknown EFB snapshot");
    if (g.snapshot_refs[std::size_t(index)] > 1) { --g.snapshot_refs[std::size_t(index)]; return; }
    g.snapshot_refs[std::size_t(index)] = 0;
    g.snapshot_busy[index] = false;
    if (g.selected_snapshot == index) g.selected_snapshot = -1;
    if (g.last_snapshot == index) g.last_snapshot = -1;
}
void GlesRenderer::adopt_thread() { impl_->owner = std::this_thread::get_id(); }
void GlesRenderer::start_submission(std::function<void()> release, std::function<void()> bind, std::function<void()> unbind) {
    auto& g = *impl_; g.require_ready();
    auto& q = g.submission;
    if (q.running) throw std::logic_error("GLES submission thread already running");
    release();
    q.stopping = false; q.failure = nullptr; q.unbind = std::move(unbind);
    std::promise<std::thread::id> started;
    auto id = started.get_future();
    q.thread = std::thread([&g, bind = std::move(bind), &started] {
        bind();
        started.set_value(std::this_thread::get_id());
        g.run_submission();
    });
    q.id = id.get();
    q.running = true;
    // Submission serves only the direct path; it exists before the first GX operation.
    g.ensure_direct();
}
void GlesRenderer::start_submission_on(std::function<void(std::function<void()>)> post) {
    auto& g = *impl_; g.require_ready();
    auto& q = g.submission;
    if (q.running) throw std::logic_error("GLES submission already running");
    if (!post) throw std::invalid_argument("GLES submission needs a task poster");
    q.stopping = false; q.failure = nullptr; q.unbind = nullptr;
    q.post = std::move(post);
    q.id = std::this_thread::get_id();
    q.running = true;
    g.ensure_direct();
}
void GlesRenderer::end_frame() {
    auto& g = *impl_; g.require_ready();
    g.execute([&g] {
        try { g.bound_frames(); gl_check("GLES frame end"); } catch (...) { g.failed = true; throw; }
    }, false);
    if (g.direct) g.direct->next_frame();
}
void GlesRenderer::present_into(unsigned framebuffer, int source_width, int source_height, int x, int y, int width,
                                int height) {
    auto& g = *impl_;
    if (!g.submission.post || std::this_thread::get_id() != g.submission.id)
        throw std::logic_error("present_into runs on the host's GL thread");
    if (g.failed) throw std::runtime_error("GLES renderer failed; destroy it before further rendering");
    if (width <= 0 || height <= 0 || source_width <= 0 || source_height <= 0 || source_width > g.efb_width ||
        source_height > g.efb_height)
        throw std::invalid_argument("invalid presentation rectangle");
    try {
        glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g.framebuffer); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        const int top = g.efb_height - source_height;
        glBlitFramebuffer(0, top, source_width, g.efb_height, x, y, x + width, y + height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        gl_check("EFB presentation");
    } catch (...) { g.failed = true; throw; }
}
void GlesRenderer::stop_submission() {
    auto& g = *impl_;
    auto& q = g.submission;
    if (!q.running) return;
    if (g.direct) g.direct->flush();
    if (q.post) {
        std::unique_lock<std::mutex> lock(q.mutex);
        q.done.wait(lock, [&] { return q.completed >= q.submitted; });
        q.post = nullptr; q.running = false; q.id = {};
        return;
    }
    { std::lock_guard<std::mutex> lock(q.mutex); q.stopping = true; }
    q.ready.notify_all();
    q.thread.join();
    q.running = false; q.id = {};
}
void GlesRenderer::set_swap(std::function<void()> swap) { impl_->swap = std::move(swap); }
void GlesRenderer::release_snapshots() {
    auto& g = *impl_; g.check_owner();
    std::fill(g.snapshot_busy.begin(), g.snapshot_busy.end(), false);
    std::fill(g.snapshot_refs.begin(), g.snapshot_refs.end(), 0u);
    g.selected_snapshot = -1;
    g.last_snapshot = -1;
}
const std::uint8_t* GlesRenderer::Impl::peek_snapshot(int x, int y, bool color) {
    const int snapshot = selected_snapshot;
    ++stats.peeks;
    if (snapshot < 0) return nullptr;
    if (!snapshot_ready(snapshot) && ++stats.peek_waits)
        execute([&] { try { collect_snapshots(snapshot); } catch (...) { failed = true; throw; } }, true);
    if (!snapshot_region[std::size_t(snapshot)].contains(x, y)) {
        // Outside the read-back region: the retained snapshot itself, after queued GL work.
        ++stats.region_misses;
        execute([&] {
            try {
                const int scale = snapshot_scale();
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, snapshots[std::size_t(snapshot)].framebuffer);
                glReadPixels((color ? efb_width / scale : 0) + x / scale, (efb_height - 1 - y) / scale, 1, 1, GL_RGBA,
                             GL_UNSIGNED_BYTE, region_miss_pixel.data());
                glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
                gl_check("EFB snapshot region miss");
            } catch (...) { failed = true; throw; }
        }, true);
        return region_miss_pixel.data();
    }
    return snapshot_pixel(std::size_t(snapshot), x, y, color);
}
std::uint32_t GlesRenderer::peek_depth(int x, int y) {
    auto& g=*impl_;g.check_owner();
    if (x<0 || x>=g.efb_width || y<0 || y>=g.efb_height)
        throw std::invalid_argument("depth query outside native EFB");
    g.require_ready();
    if (const auto* pixel = g.peek_snapshot(x, y, false)) return packed_depth(pixel);
    ++g.stats.live_peeks;
    std::array<std::uint8_t,4> pixel{};
    g.execute([&] {
        try { g.read_depth(x,g.efb_height-1-y,1,1,pixel.data(),-1); } catch (...) {g.failed=true;throw;}
    }, true);
    return packed_depth(pixel.data());
}
std::uint32_t GlesRenderer::peek_color(int x, int y) {
    auto& g = *impl_; g.check_owner();
    if (x < 0 || x >= g.efb_width || y < 0 || y >= g.efb_height)
        throw std::invalid_argument("colour query outside native EFB");
    g.require_ready();
    if (const auto* pixel = g.peek_snapshot(x, y, true)) return argb(pixel);
    ++g.stats.live_peeks;
    std::array<std::uint8_t, 4> pixel{};
    g.execute([&] {
        try {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g.framebuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glReadPixels(x, g.efb_height - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            gl_check("native EFB colour query");
        } catch (...) { g.failed = true; throw; }
    }, true);
    return argb(pixel.data());
}
std::vector<std::uint8_t> GlesRenderer::readback() {
    auto& g = *impl_; g.require_ready();
    std::vector<std::uint8_t> pixels(std::size_t(g.efb_width) * g.efb_height * 4);
    try {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g.framebuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glReadPixels(0, 0, g.efb_width, g.efb_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        gl_check("EFB readback"); return pixels;
    } catch (...) { g.failed = true; throw; }
}
// Diagnostic counters; with a submission thread running they are read unsynchronized.
GlesProgramBuilderStats GlesRenderer::program_builder() {
    auto& g = *impl_; g.check_owner();
    g.ensure_direct();
    return g.direct->program_builder();
}

void GlesRenderer::build_programs_in_background() {
    if (impl_->direct) impl_->direct->build_programs_in_background();
}

bool GlesRenderer::failed() const { return impl_->failed; }
GlesStats GlesRenderer::stats() const {
    auto result = impl_->stats;
    result.capture_cache = impl_->capture_cache.stats();
    result.geometry_cache = impl_->geometry_cache.stats();
    if (impl_->direct) result.direct = impl_->direct->stats();
    return result;
}

} // namespace starlet::gx

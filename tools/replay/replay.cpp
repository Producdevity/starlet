// Replays a GX trace (src/gx/trace/format.hpp) through Starlet's decoder and OpenGL ES renderer
// offscreen, and hashes every presented frame. See docs/Replay-traces.md.

#include "gl_context.hpp"
#include "png.hpp"
#include "shaders.hpp"
#include "trace/format.hpp"

#include "starlet/gx/gles/renderer.hpp"

#include <GLES3/gl31.h>
#include <sys/mman.h>

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace starlet::gx;
using namespace starlet::gx::trace;

namespace {

struct Divergence : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::uint64_t hash_bytes(const std::uint8_t* p, std::size_t size) {
    std::uint64_t h = 0xcbf29ce484222325ull ^ size;
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t word;
        std::memcpy(&word, p + i, 8);
        h = (h ^ word) * 0xd6e8feb86659fd93ull;
        h ^= h >> 29;
    }
    for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h ^ (h >> 31);
}

// One recorded address space: 8 GiB reserved so any 32-bit address plus a recorded size fits.
class Shadow {
public:
    Shadow() {
        void* p = mmap(nullptr, reserved, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) throw std::runtime_error("cannot reserve replay memory");
        base_ = static_cast<std::uint8_t*>(p);
    }
    ~Shadow() { munmap(base_, reserved); }
    std::uint8_t* at(std::uint64_t address, std::uint64_t size) const {
        if (address + size > reserved) throw std::runtime_error("trace address out of range");
        return base_ + address;
    }

private:
    static constexpr std::uint64_t reserved = std::uint64_t{8} << 30;
    std::uint8_t* base_;
};

struct Failure {
    Space space;
    std::uint32_t address, size;
};

class Memory final : public MemoryReader {
public:
    Shadow spaces[3];
    std::vector<Failure> failures;
    std::unordered_map<std::uint32_t, MemoryArrayView> arrays;

    bool failed(Space space, std::uint32_t address, std::size_t size) const {
        for (const auto& f : failures)
            if (f.space == space && address < std::uint64_t{f.address} + f.size && f.address < std::uint64_t{address} + size) return true;
        return false;
    }
    std::uint8_t* canonical(std::uint32_t address, std::size_t size) { return spaces[0].at(address, size); }

    bool copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
        if (failed(Space::canonical, address, size)) return false;
        std::memcpy(destination, spaces[0].at(address, size), size);
        return true;
    }
    bool copy_indexed_xf(std::uint8_t, std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
        if (failed(Space::xf, address, size)) return false;
        std::memcpy(destination, spaces[2].at(address, size), size);
        return true;
    }
    ByteView view(std::uint32_t address, std::size_t size) override {
        if (failed(Space::canonical, address, size)) return {};
        return {spaces[0].at(address, size), size};
    }
    MemoryArrayView array_view(std::uint32_t address) override {
        const auto found = arrays.find(address);
        return found == arrays.end() ? MemoryArrayView{} : found->second;
    }
    std::uint64_t range_generation(const MemoryArrayView&, std::uint32_t, std::size_t) override { return 0; }
    std::uint64_t source_generation(const void*) override { return 0; }
    SourceIdentity source_identity(const void* bytes, std::size_t) override { return {bytes, 0}; }
    ByteView begin_borrow(std::uint32_t address, std::size_t size) override { return view(address, size); }
};

// The host GL thread: runs posted tasks in order with the context current.
class GlThread {
public:
    void post(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
        wake_.notify_one();
    }
    template <class F> auto call(F&& f) {
        std::packaged_task<decltype(f())()> task(std::forward<F>(f));
        auto result = task.get_future();
        post([&task] { task(); });
        return result.get();
    }
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return !tasks_.empty(); });
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            if (!task) return;
            task();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> tasks_;
};

constexpr int surface_width = 1280, surface_height = 960;

struct Options {
    std::string trace, dump;
    bool quiet = false;
};

class Replay {
public:
    Replay(Reader& in, const Header& header, const Options& options) : in_(in), header_(header), options_(options), sink_(*this) {
        auto limits = header_.renderer;
        limits.direct.background_programs = false;
        limits.direct.program_lists.clear();
        // A fresh cache: fast shading lists every program it links there (programs.list).
        char directory[] = "/tmp/starlet-replay-XXXXXX";
        if (!mkdtemp(directory)) throw std::runtime_error("cannot make a program cache directory");
        cache_ = limits.direct.program_cache = directory;
        thread_ = std::thread([this] { gl_main(); });
        try {
            gl_.call([this, limits] {
                renderer_ = std::make_unique<GlesRenderer>(limits);
                renderer_->start_submission_on([this](std::function<void()> task) { gl_.post(std::move(task)); });
                return 0;
            });
            renderer_->adopt_thread();
            decoder_ = std::make_unique<Decoder>(memory_, sink_, header_.decoder, header_.efb);
        } catch (...) {
            gl_.post([this] { if (renderer_) renderer_->adopt_thread(); renderer_.reset(); destroy_gl(); });
            gl_.post({});
            thread_.join();
            throw;
        }
    }
    ~Replay() {
        try { renderer_->stop_submission(); } catch (...) {}
        gl_.call([this] { if (renderer_) renderer_->adopt_thread(); renderer_.reset(); destroy_gl(); return 0; });
        gl_.post({});
        thread_.join();
    }

    void run() {
        for (;;) {
            const auto op = in_.op();
            ++events_;
            if (op == Op::end) break;
            dispatch(op);
        }
        gl_.call([] { return 0; });
        const auto stats = renderer_->stats();
        rejected_programs_ = stats.direct.rejected_programs;
        if (std::getenv("STARLET_REPLAY_STATS"))
            std::fprintf(stderr, "replay: renderer draws %llu, zero-area %llu, culled %llu, unready %llu, texture uploads %llu, resident copies %llu hits %llu\n",
                         (unsigned long long)stats.direct.draws, (unsigned long long)stats.direct.zero_area,
                         (unsigned long long)stats.direct.culled_draws, (unsigned long long)stats.direct.unready_draws,
                         (unsigned long long)stats.direct.texture_uploads, (unsigned long long)stats.direct.resident_copies,
                         (unsigned long long)stats.direct.resident_hits);
    }
    std::uint64_t rejected_programs() const { return rejected_programs_; }
    const std::string& cache() const { return cache_; }
    std::vector<std::uint64_t> hashes() {
        std::lock_guard<std::mutex> lock(hash_mutex_);
        return hashes_;
    }
    std::uint64_t rejected() const { return rejected_; }
    // What the trace exercises: renderer calls by kind.
    std::string summary() const {
        auto n = [&](Op op) { return ops_[std::size_t(op)]; };
        char text[256];
        std::snprintf(text, sizeof text, "%llu draws, %llu texture copies, %llu display copies, %llu clears, %llu peeks and snapshots, %llu blooms, %llu offscreen passes",
                      (unsigned long long)(n(Op::draw_direct) + n(Op::draw)), (unsigned long long)(n(Op::copy_known) + n(Op::copy_texture)),
                      (unsigned long long)n(Op::copy_display), (unsigned long long)(n(Op::clear_efb) + n(Op::clear)),
                      (unsigned long long)(n(Op::peek_depth) + n(Op::peek_color) + n(Op::snapshot) + n(Op::request_depth)),
                      (unsigned long long)n(Op::end_bloom), (unsigned long long)n(Op::begin_offscreen));
        return text;
    }

private:
    struct Callback {
        const State* state;
        const Draw* draw;
    };

    class Sink final : public CommandSink {
    public:
        explicit Sink(Replay& r) : r_(r) {}
        bool draw(const State& state, const Draw& command, MemoryReader&) override {
            r_.expect(Op::sink_draw);
            const auto primitive = r_.in_.u8();
            const auto count = r_.in_.u16();
            if (primitive != command.primitive || count != command.vertex_count)
                r_.diverge("draw differs: recorded primitive " + std::to_string(primitive) + " x" + std::to_string(count));
            return r_.nested({&state, &command});
        }
        bool bp_write(const State& state, std::uint8_t address, std::uint32_t value) override {
            r_.expect(Op::sink_bp);
            const auto a = r_.in_.u8();
            const auto v = r_.in_.u32();
            if (a != address || v != value) r_.diverge("BP write differs");
            return r_.nested({&state, nullptr});
        }
        bool observes_bp(std::uint8_t address) const override { return r_.header_.observed_bp[address]; }
        bool invalidate_vertices() override {
            r_.expect(Op::sink_invalidate);
            return r_.nested({nullptr, nullptr});
        }
        bool host_command(const State& state, ByteView input, std::size_t& consumed) override {
            r_.expect(Op::sink_host);
            if (r_.in_.u64() != input.size) r_.diverge("host command input differs");
            return r_.nested({&state, nullptr}, &consumed);
        }
        bool filter_bp(const State& state, std::uint8_t address, std::uint32_t& value) override {
            r_.expect(Op::sink_filter);
            const auto a = r_.in_.u8();
            const auto v = r_.in_.u32();
            if (a != address || v != value) r_.diverge("filtered BP write differs");
            std::uint64_t out = value;
            const bool result = r_.nested({&state, nullptr}, &out);
            value = std::uint32_t(out);
            return result;
        }

    private:
        Replay& r_;
    };

    [[noreturn]] void diverge(const std::string& what) {
        throw Divergence("diverged at event " + std::to_string(events_) + " (frame " + std::to_string(frames_) + "): " + what);
    }
    void expect(Op op) {
        const auto got = in_.op();
        ++events_;
        if (got != op)
            diverge("expected recorded op " + std::to_string(int(op)) + ", trace has " + std::to_string(int(got)));
    }
    template <class Out> bool nested(Callback callback, Out* out) {
        callbacks_.push_back(callback);
        for (;;) {
            const auto op = in_.op();
            ++events_;
            if (op == Op::sink_end) break;
            if (op == Op::end) diverge("trace ended inside a callback");
            dispatch(op);
        }
        callbacks_.pop_back();
        const bool result = in_.u8();
        const auto extra = in_.u64();
        if (out) *out = static_cast<Out>(extra);
        return result;
    }
    bool nested(Callback callback) { return nested<std::uint64_t>(callback, nullptr); }

    void decoder_call(Op op) {
        Status status;
        switch (op) {
        case Op::feed: { const auto b = in_.bytes(); status = decoder_->feed({b.data(), b.size()}); break; }
        case Op::extend: {
            const auto b = in_.bytes();
            const bool extended = decoder_->extend_pending({b.data(), b.size()});
            expect(Op::call_end);
            in_.u8();
            if (bool(in_.u8()) != extended) diverge("extend_pending differs");
            return;
        }
        case Op::finish: status = decoder_->finish(); break;
        case Op::call_list: {
            const bool owned = in_.u8();
            const auto b = in_.bytes();
            status = decoder_->call_display_list({b.data(), b.size()}, owned);
            break;
        }
        case Op::inject: { const auto b = in_.bytes(); status = decoder_->inject({b.data(), b.size()}); break; }
        case Op::texture_extent: {
            const auto a = in_.u32();
            decoder_->set_texture_extent(std::uint8_t(a & 15), {std::uint16_t(a >> 4 & 0x3fff), std::uint16_t(a >> 18 & 0x3fff)});
            status = decoder_->status();
            break;
        }
        case Op::load_tlut: {
            const auto control = in_.u32();
            const auto b = in_.bytes();
            status = decoder_->load_tlut(control, {b.data(), b.size()});
            break;
        }
        default: break;
        }
        expect(Op::call_end);
        const auto error = in_.u8();
        in_.u8();
        if (error != std::uint8_t(status.error)) diverge("decoder status differs: " + status.message);
    }

    std::uint8_t* pointer() {
        if (!in_.u8()) return nullptr;
        return memory_.canonical(in_.u32(), 0);
    }
    int handle(std::map<int, int>& map, int recorded) {
        const auto found = map.find(recorded);
        return found == map.end() ? -1 : found->second;
    }

    template <class F> void renderer_call(F&& f) {
        try {
            f();
        } catch (const std::exception& e) {
            if (renderer_->failed()) throw std::runtime_error(std::string("renderer failed: ") + e.what());
            ++rejected_;
            if (!options_.quiet && rejected_ <= 8) std::fprintf(stderr, "replay: rejected at frame %zu: %s\n", frames_, e.what());
        }
    }

    void dispatch(Op op) {
        auto& r = *renderer_;
        ++ops_[std::size_t(op)];
        switch (op) {
        case Op::mem: {
            const auto space = in_.u8();
            const auto address = in_.u32();
            const auto size = in_.u32();
            if (space > 2) throw std::runtime_error("trace: bad memory space");
            in_.raw(memory_.spaces[space].at(address, size), size);
            break;
        }
        case Op::fail: {
            const auto space = Space(in_.u8());
            const auto address = in_.u32();
            memory_.failures.push_back({space, address, in_.u32()});
            break;
        }
        case Op::array: {
            const auto address = in_.u32();
            MemoryArrayView v;
            const auto size = in_.u64();
            v.bytes = {memory_.spaces[1].at(address, size), size};
            v.native_unit = in_.u8();
            v.host_order = in_.u8();
            memory_.arrays[address] = v;
            break;
        }
        case Op::state: TraceAccess::load(*decoder_, in_); break;
        case Op::feed: case Op::extend: case Op::finish: case Op::call_list: case Op::inject: case Op::texture_extent: case Op::load_tlut:
            decoder_call(op);
            break;
        case Op::draw_direct: case Op::draw: {
            const auto cmpr = TextureCmprModel(in_.u8());
            if (callbacks_.empty() || !callbacks_.back().draw) diverge("draw outside a draw callback");
            const auto& c = callbacks_.back();
            renderer_call([&] {
                if (op == Op::draw_direct) r.draw_direct(*c.state, *c.draw, memory_, cmpr);
                else r.draw(*c.state, *c.draw, memory_, cmpr);
            });
            break;
        }
        case Op::clear: { std::array<std::uint8_t, 3> color; in_.raw(color.data(), 3); renderer_call([&] { r.clear(color); }); break; }
        case Op::clear_efb: { EfbClear c; get(in_, c); renderer_call([&] { r.clear_efb(c); }); break; }
        case Op::begin_offscreen: renderer_call([&] { r.begin_offscreen(); }); break;
        case Op::end_offscreen: renderer_call([&] { r.end_offscreen(); }); break;
        case Op::begin_bloom: renderer_call([&] { r.begin_bloom(); }); break;
        case Op::end_bloom: { EfbBloom b; get(in_, b); renderer_call([&] { r.end_bloom(b); }); break; }
        case Op::copy_display: {
            DisplayCopy c; get(in_, c);
            auto* destination = pointer();
            const auto capacity = in_.u64();
            renderer_call([&] { r.copy_display(c, destination, capacity); });
            break;
        }
        case Op::copy_texture: {
            TextureCopy c; get(in_, c);
            auto* destination = pointer();
            const auto capacity = in_.u64();
            renderer_call([&] { r.copy_texture(c, destination, capacity); });
            break;
        }
        case Op::copy_known: {
            TextureCopy c; get(in_, c);
            auto* destination = pointer();
            const auto capacity = in_.u64();
            const auto token = in_.u32();
            std::vector<std::uint8_t> contents;
            const bool known = in_.u8();
            if (known) contents = in_.bytes();
            renderer_call([&] { r.copy_texture_known(c, destination, capacity, token, known ? contents.data() : nullptr); });
            break;
        }
        case Op::write_back: {
            const auto token = in_.u32();
            const auto bytes = in_.u64();
            const bool on = in_.u8();
            renderer_call([&] { r.write_back(token, bytes, on); });
            break;
        }
        case Op::land_write_backs: {
            const auto through = in_.u64();
            in_.u8();
            // Landing what happens to be read back depends on GPU timing; waiting does not.
            renderer_call([&] { r.land_write_backs(through, true); });
            break;
        }
        case Op::release_resident: {
            const auto token = in_.u32();
            const auto bytes = in_.u64();
            renderer_call([&] { r.release_resident(token, bytes); });
            break;
        }
        case Op::dma_read: case Op::dma_write: {
            auto* data = pointer();
            const auto bytes = in_.u64();
            renderer_call([&] { if (op == Op::dma_read) r.before_dma_read(data, bytes); else r.before_dma_write(data, bytes); });
            break;
        }
        case Op::wait_until_idle: renderer_call([&] { r.wait_until_idle(); }); break;
        case Op::peek_depth: case Op::peek_color: {
            const auto x = in_.i32();
            const auto y = in_.i32();
            renderer_call([&] { if (op == Op::peek_depth) r.peek_depth(x, y); else r.peek_color(x, y); });
            break;
        }
        case Op::snapshot: {
            const bool exact = in_.u8();
            EfbRegion region; get(in_, region);
            const auto recorded = in_.i32();
            int index = -1;
            renderer_call([&] { index = exact ? r.snapshot_efb_exact(region) : r.snapshot_efb(region); });
            snapshots_[recorded] = index;
            break;
        }
        case Op::wait_snapshot: case Op::select_snapshot: case Op::release_snapshot: case Op::retain_snapshot: {
            const auto recorded = in_.i32();
            const int index = recorded < 0 ? recorded : handle(snapshots_, recorded);
            if (recorded >= 0 && index < 0) break; // A snapshot taken before the trace started.
            renderer_call([&] {
                if (op == Op::wait_snapshot) r.wait_snapshot(index);
                else if (op == Op::select_snapshot) r.select_snapshot(index);
                else if (op == Op::release_snapshot) r.release_snapshot(index);
                else r.retain_snapshot(index);
            });
            break;
        }
        case Op::release_snapshots: renderer_call([&] { r.release_snapshots(); }); break;
        case Op::request_depth: {
            const auto recorded = in_.i32();
            int h = -1;
            renderer_call([&] { h = r.request_depth_readback(); });
            depths_[recorded] = h;
            break;
        }
        case Op::wait_depth: case Op::release_depth: {
            const int h = handle(depths_, in_.i32());
            if (h < 0) break; // A readback requested before the trace started.
            renderer_call([&] { if (op == Op::wait_depth) r.wait_depth_readback(h); else r.release_depth_readback(h); });
            break;
        }
        case Op::frames_in_flight: { const auto n = in_.u32(); renderer_call([&] { r.set_frames_in_flight(n); }); break; }
        case Op::present_into: for (int i = 0; i < 6; ++i) in_.i32(); break;
        case Op::packet_draw: throw std::runtime_error("the trace draws captured packets (draw_batch), which a replay cannot rebuild; record the direct path");
        case Op::present_framebuffer: {
            auto* data = pointer();
            FramebufferView view;
            const auto size = in_.u64();
            view.bytes = {data, data ? size : 0};
            view.width = in_.u32();
            view.height = in_.u32();
            view.stride = in_.u64();
            const auto width = in_.i32();
            const auto height = in_.i32();
            const bool black = in_.u8();
            renderer_call([&] { r.present_framebuffer(view, width, height, black); });
            capture_default(width, height);
            break;
        }
        case Op::present: {
            const auto width = in_.i32();
            const auto height = in_.i32();
            renderer_call([&] { r.present(width, height); });
            capture_default(width, height);
            break;
        }
        case Op::end_frame:
            renderer_call([&] { r.end_frame(); });
            capture_efb();
            break;
        default:
            throw std::runtime_error("trace: unknown or misplaced op " + std::to_string(int(op)) + " at event " + std::to_string(events_));
        }
    }

    // Captures read the frame with only framebuffer and pack bindings changed, and restore them.
    struct Bindings {
        GLint read = 0, draw = 0, pack = 0, alignment = 4;
        Bindings() {
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
            glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
        }
        ~Bindings() {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
            glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        }
    };
    void capture_default(int width, int height) {
        width = std::min(width, surface_width);
        height = std::min(height, surface_height);
        const auto frame = next_frame();
        gl_.post([this, frame, width, height] {
            Bindings keep;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            finish_capture(frame, width, height);
        });
    }
    void capture_efb() {
        const auto frame = next_frame();
        const int width = EfbLayout::width, height = header_.efb.height;
        gl_.post([this, frame, width, height] {
            {
                Bindings keep;
                try {
                    renderer_->present_into(capture_framebuffer_, width, height, 0, 0, width, height);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "replay: EFB capture failed: %s\n", e.what());
                }
            }
            Bindings keep;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, capture_framebuffer_);
            finish_capture(frame, width, height);
        });
    }
    std::size_t next_frame() {
        std::lock_guard<std::mutex> lock(hash_mutex_);
        hashes_.push_back(0);
        ++frames_;
        return hashes_.size() - 1;
    }
    void finish_capture(std::size_t frame, int width, int height) {
        Image image;
        image.width = unsigned(width);
        image.height = unsigned(height);
        std::vector<std::uint8_t> rows(std::size_t(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        image.rgba.resize(rows.size());
        const std::size_t stride = std::size_t(width) * 4;
        for (int y = 0; y < height; ++y) std::memcpy(&image.rgba[y * stride], &rows[(height - 1 - y) * stride], stride);
        {
            std::lock_guard<std::mutex> lock(hash_mutex_);
            hashes_[frame] = hash_bytes(image.rgba.data(), image.rgba.size());
        }
        if (!options_.dump.empty()) {
            char name[32];
            std::snprintf(name, sizeof name, "/frame-%03zu.png", frame);
            write_png(options_.dump + name, image);
        }
    }

    void gl_main() {
        if (!gl_context_.make(surface_width, surface_height)) std::exit(2);
        glGenTextures(1, &capture_texture_);
        glBindTexture(GL_TEXTURE_2D, capture_texture_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, EfbLayout::width, EfbLayout::rows);
        glGenFramebuffers(1, &capture_framebuffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, capture_framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capture_texture_, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        gl_.run();
    }
    void destroy_gl() {
        glDeleteFramebuffers(1, &capture_framebuffer_);
        glDeleteTextures(1, &capture_texture_);
        gl_context_.destroy();
    }

    Reader& in_;
    const Header& header_;
    const Options& options_;
    Memory memory_;
    Sink sink_;
    GlThread gl_;
    std::thread thread_;
    GlContext gl_context_;
    GLuint capture_texture_ = 0, capture_framebuffer_ = 0;
    std::unique_ptr<GlesRenderer> renderer_;
    std::unique_ptr<Decoder> decoder_;
    std::vector<Callback> callbacks_;
    std::map<int, int> snapshots_, depths_;
    std::mutex hash_mutex_;
    std::vector<std::uint64_t> hashes_;
    std::size_t frames_ = 0;
    std::uint64_t events_ = 0, rejected_ = 0;
    std::array<std::uint64_t, 256> ops_{};
    std::uint64_t rejected_programs_ = 0;
    std::string cache_;
};

int compare(const std::string& reference, const std::string& actual, const std::string& out) {
    Image a, b;
    if (!read_png(reference, a) || !read_png(actual, b)) {
        std::fprintf(stderr, "compare: cannot read %s or %s\n", reference.c_str(), actual.c_str());
        return 2;
    }
    if (a.width != b.width || a.height != b.height) {
        std::printf("size %ux%u vs %ux%u\n", a.width, a.height, b.width, b.height);
        return 1;
    }
    // Reference, actual and their difference (amplified) side by side.
    Image d;
    d.width = a.width * 3;
    d.height = a.height;
    d.rgba.assign(std::size_t(d.width) * d.height * 4, 255);
    std::uint64_t differing = 0;
    int largest = 0;
    for (unsigned y = 0; y < a.height; ++y)
        for (unsigned x = 0; x < a.width; ++x) {
            const auto* p = &a.rgba[(std::size_t(y) * a.width + x) * 4];
            const auto* q = &b.rgba[(std::size_t(y) * a.width + x) * 4];
            auto* row = &d.rgba[std::size_t(y) * d.width * 4];
            int delta = 0;
            for (int c = 0; c < 3; ++c) {
                row[x * 4 + c] = p[c];
                row[(a.width + x) * 4 + c] = q[c];
                const int e = std::abs(int(p[c]) - int(q[c]));
                delta = std::max(delta, e);
                row[(2 * a.width + x) * 4 + c] = std::uint8_t(std::min(255, e * 8));
            }
            differing += delta != 0;
            largest = std::max(largest, delta);
        }
    write_png(out, d);
    std::printf("%llu pixels differ, largest channel difference %d\n", (unsigned long long)differing, largest);
    return differing ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--compare" && i + 3 < argc) return compare(argv[i + 1], argv[i + 2], argv[i + 3]);
        if (a == "--shaders") {
            GlContext context;
            if (!context.make(64, 64)) return 2;
            const int result = check_shaders({argv + i + 1, argv + argc});
            context.destroy();
            return result;
        }
        if (a == "--dump" && i + 1 < argc) options.dump = argv[++i];
        else if (a == "--quiet") options.quiet = true;
        else if (options.trace.empty()) options.trace = a;
        else { std::fprintf(stderr, "replay: unexpected argument %s\n", a.c_str()); return 2; }
    }
    if (options.trace.empty()) {
        std::fprintf(stderr, "usage: starlet-replay TRACE|- [--dump DIR] [--quiet]\n"
                             "       starlet-replay --shaders [PROGRAM_LIST...]\n"
                             "       starlet-replay --compare REFERENCE.png ACTUAL.png DIFF.png\n");
        return 2;
    }
    std::FILE* file = options.trace == "-" ? stdin : std::fopen(options.trace.c_str(), "rb");
    if (!file) {
        std::fprintf(stderr, "replay: cannot open %s\n", options.trace.c_str());
        return 2;
    }
    try {
        Reader in(file);
        char m[sizeof magic];
        in.raw(m, sizeof m);
        if (std::memcmp(m, magic, sizeof m)) throw std::runtime_error("not a Starlet GX trace");
        if (const auto v = in.u32(); v != version) throw std::runtime_error("trace version " + std::to_string(v) + ", replayer reads " + std::to_string(version));
        Header header;
        get(in, header);
        std::vector<std::uint64_t> hashes;
        std::uint64_t rejected = 0, rejected_programs = 0;
        std::string summary, cache;
        {
            Replay replay(in, header, options);
            try {
                replay.run();
            } catch (const Divergence& d) {
                std::printf("%s\n", d.what());
                return 3;
            }
            hashes = replay.hashes();
            rejected = replay.rejected();
            summary = replay.summary();
            rejected_programs = replay.rejected_programs();
            cache = replay.cache();
        }
        std::error_code ignored;
        if (!options.dump.empty() && std::filesystem::exists(cache + "/programs.list"))
            std::filesystem::copy_file(cache + "/programs.list", options.dump + "/programs.list",
                                       std::filesystem::copy_options::overwrite_existing, ignored);
        if (cache.rfind("/tmp/starlet-replay-", 0) == 0) std::filesystem::remove_all(cache, ignored);
        for (std::size_t i = 0; i < hashes.size(); ++i) std::printf("frame %zu %016llx\n", i, (unsigned long long)hashes[i]);
        std::fprintf(stderr, "replay: %zu frames, %llu rejected calls (%s)\nreplay: %s\n", hashes.size(), (unsigned long long)rejected,
                     header.source.c_str(), summary.c_str());
        if (rejected_programs) {
            std::printf("FAILED: the driver rejected %llu generated programs\n", (unsigned long long)rejected_programs);
            return 4;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "replay: %s\n", e.what());
        return 2;
    }
    return 0;
}

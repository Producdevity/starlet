// The GPU thread: decodes the game's GX command stream with Starlet and hands draws, copies and
// presentation to the renderer, in command order.
#include "gpu_internal.hpp"

#include "starlet/gx/decoder.hpp"
#include "starlet/gx/display_copy.hpp"
#include "starlet/gx/texture_copy.hpp"
#include "starlet/gx_fifo/frame_trace.hpp"
#include "starlet/gx_fifo/gpu.hpp"
#include "starlet/gx_fifo/renderer.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/os_core.hpp"
#include "starlet/os/thread_name.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace starlet::gx_fifo {
namespace {

using namespace starlet::gx;
using os::host_allocation_scope;

// Every GX address is a native address the game can hand to the GPU: game memory as starlet::os
// knows it. Memory holds canonical (big-endian) GX bytes. Arrays the game fills with native numbers
// for the GX (shadow geometry) are registered by their owners; everything else in game memory is
// file data or bytes, which the GX reads as stored. Used by the GPU thread only: registrations are
// posted to it in FIFO order.
class HostOrderArrays {
public:
    void add(std::uintptr_t begin, std::size_t bytes) { ranges_[begin] = begin + bytes; }
    void remove(std::uintptr_t begin) { ranges_.erase(begin); }
    // The registered range containing address, or {0, 0}.
    std::pair<std::uintptr_t, std::uintptr_t> find(std::uintptr_t address) const {
        if (ranges_.empty()) return {0, 0};
        auto it = ranges_.upper_bound(address);
        if (it == ranges_.begin()) return {0, 0};
        --it;
        return address < it->second ? std::make_pair(it->first, it->second) : std::make_pair(std::uintptr_t{0}, std::uintptr_t{0});
    }
    // Byte-swaps the 32-bit words of [address, address + size) that lie in registered ranges.
    void to_canonical(std::uintptr_t address, std::uint8_t* bytes, std::size_t size) const {
        if (ranges_.empty()) return;
        auto it = ranges_.upper_bound(address);
        if (it != ranges_.begin()) --it;
        for (; it != ranges_.end() && it->first < address + size; ++it) {
            const std::uintptr_t begin = std::max(it->first, address), end = std::min(it->second, address + size);
            for (std::uintptr_t word = it->first + ((begin - it->first) & ~std::uintptr_t{3}); word + 4 <= end; word += 4) {
                if (word < address) continue;
                std::uint8_t* w = bytes + (word - address);
                std::swap(w[0], w[3]);
                std::swap(w[1], w[2]);
            }
        }
    }

private:
    std::map<std::uintptr_t, std::uintptr_t> ranges_;
};

HostOrderArrays& host_order_arrays() {
    static HostOrderArrays* arrays = [] {
        host_allocation_scope host;
        return new HostOrderArrays;
    }();
    return *arrays;
}

// GpuSetup::arrays_host_order: arrays are host order unless registered here as canonical bytes.
bool arrays_host_order = false;
HostOrderArrays& big_endian_arrays() {
    static HostOrderArrays* arrays = [] {
        host_allocation_scope host;
        return new HostOrderArrays;
    }();
    return *arrays;
}

// Write generations of the arena's 4 KiB pages. Console code must flush the data cache (DCFlushRange,
// DCStoreRange) before the GPU reads what the CPU wrote, and every DMA into memory (disc, ARAM, card,
// THP) reports its range, so a page's generation changes whenever bytes the GPU reads can have
// changed. Starlet then hashes a texture's source only when its pages changed (and every 16th frame
// regardless).
class PageGenerations {
public:
    static PageGenerations& get() {
        static PageGenerations pages;
        return pages;
    }
    void wrote(const void* data, std::size_t bytes) {
        std::size_t first, end;
        if (!range(data, bytes, first, end)) return;
        const auto generation = counter_.fetch_add(1, std::memory_order_relaxed) + 1;
        for (auto i = first; i < end; ++i) pages_[i].store(generation, std::memory_order_relaxed);
    }
    // The newest generation over the range; 0 when it lies outside the arena.
    std::uint64_t generation(const void* data, std::size_t bytes) const {
        std::size_t first, end;
        if (!range(data, bytes, first, end)) return 0;
        std::uint64_t newest = 0;
        for (auto i = first; i < end; ++i) newest = std::max(newest, pages_[i].load(std::memory_order_relaxed));
        return newest;
    }

private:
    static constexpr std::size_t page = 4096;
    PageGenerations() {
        const auto& arena = os::arena();
        count_ = (arena.size + page - 1) / page;
        pages_.reset(new std::atomic<std::uint64_t>[count_]);
        for (std::size_t i = 0; i < count_; ++i) pages_[i].store(1, std::memory_order_relaxed);
    }
    bool range(const void* data, std::size_t bytes, std::size_t& first, std::size_t& end) const {
        const auto& arena = os::arena();
        const auto* p = static_cast<const std::uint8_t*>(data);
        if (!arena.base || !bytes || p < arena.base || p + bytes > arena.base + arena.size) return false;
        first = std::size_t(p - arena.base) / page;
        end = (std::size_t(p - arena.base) + bytes + page - 1) / page;
        return end <= count_;
    }
    std::unique_ptr<std::atomic<std::uint64_t>[]> pages_;
    std::size_t count_ = 0;
    std::atomic<std::uint64_t> counter_{1};
};

void note_cpu_write(const void* data, std::size_t bytes) { PageGenerations::get().wrote(data, bytes); }

// Every GX address is a physical address in starlet::os's memory window. Memory holds canonical
// (big-endian) GX bytes, except registered host-order arrays.
class GuestMemory final : public MemoryReader {
public:
    GuestMemory() {
        PageGenerations::get();
        os::set_cpu_write_observer(note_cpu_write);
    }
    bool copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
        const void* source = pointer(address);
        if (!os::game_visible(source, size)) return fail(address, size);
        std::memcpy(destination, source, size);
        host_order_arrays().to_canonical(reinterpret_cast<std::uintptr_t>(source), destination, size);
        return true;
    }
    // Matrix and light arrays (draw and normal matrices, texture matrices) hold native floats the CPU
    // computed this frame.
    bool copy_indexed_xf(std::uint8_t, std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
        const void* source = pointer(address);
        if (!os::game_visible(source, size)) return fail(address, size);
        // A few words per load, thousands of loads per frame: swapped inline, not through memcpy.
        const auto* from = static_cast<const std::uint8_t*>(source);
        std::size_t i = 0;
        for (; i + 4 <= size; i += 4) {
            std::uint32_t word;
            std::memcpy(&word, from + i, 4);
            word = __builtin_bswap32(word);
            std::memcpy(destination + i, &word, 4);
        }
        for (; i < size; ++i) destination[i] = from[i];
        return true;
    }
    ByteView view(std::uint32_t address, std::size_t size) override {
        const void* source = pointer(address);
        if (!os::game_visible(source, size)) return {};
        const auto native = reinterpret_cast<std::uintptr_t>(source);
        const auto range = host_order_arrays().find(native);
        if (range.second && range.first < native + size) return {};  // Needs conversion: use copy().
        // Starlet asks for a texture source's generation by its pointer after viewing it.
        viewed_[source] = size;
        return {static_cast<const std::uint8_t*>(source), size};
    }
    std::uint64_t source_generation(const void* data) override {
        const auto found = viewed_.find(data);
        return found == viewed_.end() ? 0 : generation(data, found->second);
    }
    // Display-list payloads parsed in place: their pages' generation names their bytes.
    SourceIdentity source_identity(const void* bytes, std::size_t size) override { return {bytes, generation(bytes, size)}; }
    // Vertex array spans a resident geometry consumed.
    std::uint64_t range_generation(const MemoryArrayView&, std::uint32_t address, std::size_t size) override {
        return generation(pointer(address), size);
    }
    // Display lists are parsed in the game's memory instead of a copy, as the GPU reads them.
    ByteView begin_borrow(std::uint32_t address, std::size_t size) override {
        const void* source = pointer(address);
        if (!os::game_visible(source, size)) return {};
        const auto native = reinterpret_cast<std::uintptr_t>(source);
        const auto range = host_order_arrays().find(native);
        if (range.second && range.first < native + size) return {};
        return {static_cast<const std::uint8_t*>(source), size};
    }
    void end_borrow() override {}
    MemoryArrayView array_view(std::uint32_t address) override {
        const auto* source = static_cast<const std::uint8_t*>(pointer(address));
        if (!os::game_visible(source, 1)) return {};
        MemoryArrayView v;
        const auto range = host_order_arrays().find(reinterpret_cast<std::uintptr_t>(source));
        if (range.second) {
            v.bytes = {source, std::size_t(range.second - reinterpret_cast<std::uintptr_t>(source))};
            v.host_order = true;
            return v;
        }
        // Host-order arrays need no registration; copy() still reads them as stored, so a vertex read
        // past this view (none in practice: the view runs to the end of the region) would be wrong.
        if (arrays_host_order && !big_endian_arrays().find(reinterpret_cast<std::uintptr_t>(source)).second) {
            v.bytes = {source, region_rest(source)};
            v.host_order = true;
            return v;
        }
        v.bytes = {source, region_rest(source)};
        return v;
    }

private:
    static const void* pointer(std::uint32_t address) { return os::physical_to_native(address); }
    // The array runs to the end of its region: the arena, the executable image, or another region
    // of game memory (guest stacks, fixed console regions).
    static std::size_t region_rest(const std::uint8_t* source) {
        const auto& arena = os::arena();
        if (source >= arena.base && source < arena.base + arena.size) return std::size_t(arena.base + arena.size - source);
        std::uintptr_t begin = 0, end = 0;
        os::image_range(begin, end);
        const auto native = reinterpret_cast<std::uintptr_t>(source);
        if (native >= begin && native < end) return std::size_t(end - native);
        std::size_t visible = 1, step = std::size_t{1} << 30;
        for (; step; step >>= 1)
            if (os::game_visible(source, visible + step)) visible += step;
        return visible;
    }
    // Page generations; unknown (0) for host-order arrays, which native code writes without reporting.
    static std::uint64_t generation(const void* data, std::size_t size) {
        const auto native = reinterpret_cast<std::uintptr_t>(data);
        const auto range = host_order_arrays().find(native);
        if (range.second && range.first < native + size) return 0;
        return PageGenerations::get().generation(data, size);
    }
    bool fail(std::uint32_t address, std::size_t size) {
        if (reported_++ < 8) std::fprintf(stderr, "starlet: gx: read of 0x%x bytes at 0x%08x outside game memory\n", unsigned(size), address);
        return false;
    }
    unsigned reported_ = 0;
    std::unordered_map<const void*, std::size_t> viewed_;
};

struct FrameBufferInfo {
    unsigned width = 0, height = 0;
    std::size_t stride = 0;
};

struct Job {
    std::vector<std::uint8_t> bytes;
    std::function<void()> call;
};

// Token values whose draw-sync callback read the EFB, and per token value the pixels it read
// last time (bit 56 valid, then y1, x1, y0, x0 in 10 bits each above the token). Later tokens
// of that value snapshot the EFB, reading back that rectangle widened by peek_margin.
std::atomic<std::uint64_t> peek_tokens[1024];
std::atomic<std::uint64_t> peek_bounds[4096];
constexpr int peek_margin = 32;

bool token_reads_efb(std::uint16_t token) { return peek_tokens[token >> 6].load(std::memory_order_relaxed) >> (token & 63) & 1; }

// The read-back region for a snapshot at token; empty (the whole EFB) before its callback read a pixel.
EfbRegion token_region(std::uint16_t token) {
    const std::uint64_t b = peek_bounds[token & 4095].load(std::memory_order_relaxed);
    EfbRegion region;
    if ((b >> 56) && (b & 0xffff) == token) {
        const int x0 = int(b >> 16 & 0x3ff), y0 = int(b >> 26 & 0x3ff), x1 = int(b >> 36 & 0x3ff), y1 = int(b >> 46 & 0x3ff);
        region = {x0 - peek_margin, y0 - peek_margin, x1 - x0 + 1 + 2 * peek_margin, y1 - y0 + 1 + 2 * peek_margin};
    }
    return region;
}

class Gpu final : public CommandSink {
public:
    explicit Gpu(GpuSetup setup)
        : setup_(std::move(setup)), renderer_(*setup_.renderer), diagnostics_(setup_.diagnostics),
          trace_draws_(diagnostics_.trace_draws) {
        thread_ = std::thread([this] { run(); });
    }
    ~Gpu() {
        post({{}, [this] { stopping_ = true; }});
        thread_.join();
    }

    void post(Job job) {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        ++posted_;
        wake_.notify_one();
    }
    void drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::uint64_t target = posted_;
        done_.wait(lock, [&] { return completed_ >= target; });
    }
    bool idle() {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_ >= posted_;
    }

    bool draw(const State& state, const Draw& draw, MemoryReader& memory) override {
        try {
            draw_checked(state, draw, memory);
        } catch (const std::exception& error) {
            // A draw the renderer cannot take is reported and skipped: the console's GPU would draw
            // something, and the stream after it stays valid. Each kind of failure is reported once,
            // so a frequent one cannot hide the others.
            ++draw_failures_;
            if (reported_failures_.size() < 64 && reported_failures_.insert(error.what()).second) {
                auto f = [&](std::uint16_t a) { float v; const auto w = state.xf(a); std::memcpy(&v, &w, 4); return v; };
                std::fprintf(stderr, "starlet: gx: draw skipped at retrace %u: %s (vat %u, %u vertices, cp a0=%08x a1=%08x a2=%08x b0=%08x vcd %08x/%08x, "
                             "xf viewport scale %g %g %g offset %g %g %g)\n",
                             retraces_, error.what(), draw.vat, draw.total_vertices(), state.cp(0xa0), state.cp(0xa1), state.cp(0xa2),
                             state.cp(0xb0), state.cp(0x50), state.cp(0x60), f(0x101a), f(0x101b), f(0x101c), f(0x101d), f(0x101e), f(0x101f));
            }
        }
        return true;
    }

    void draw_checked(const State& state, const Draw& draw, MemoryReader& memory) {
        if (trace_draws_ > 0 && retraces_ >= diagnostics_.trace_from) {
            --trace_draws_;
            trace(state, draw);
        }
        renderer_.draw(state, draw, memory);
    }
    bool observes_bp(std::uint8_t address) const override {
        return address == 0x45 || address == 0x47 || address == 0x48 || address == 0x52;
    }
    bool bp_write(const State& state, std::uint8_t address, std::uint32_t value) override {
        switch (address) {
        case 0x45:  // Draw done.
            renderer_.wait_until_idle();
            frame_event('D');
            deliver({true, 0, -1});
            break;
        case 0x47:
            token_.store(std::uint16_t(value), std::memory_order_release);
            break;
        case 0x48:
            token_.store(std::uint16_t(value), std::memory_order_release);
            renderer_.wait_until_idle();
            deliver({false, std::uint16_t(value), events_.token ? token_snapshot(std::uint16_t(value)) : -1});
            break;
        case 0x52:
            copy(state, value);
            break;
        default:
            break;
        }
        return true;
    }
    bool invalidate_vertices() override { return true; }

    std::uint32_t peek_depth(int x, int y) { return renderer_.peek_depth(x, y); }
    std::uint32_t peek_color(int x, int y) { return renderer_.peek_color(x, y); }
    std::uint16_t token() const { return token_.load(std::memory_order_acquire); }
    void set_events(GpuEvents events) { events_ = events; }
    bool peek_snapshot(int index, bool depth, int x, int y, std::uint32_t& value) const {
        return depth ? renderer_.peek_depth_snapshot(index, x, y, value) : renderer_.peek_color_snapshot(index, x, y, value);
    }
    // GPU thread: the snapshot's value once read back; a pixel outside its read-back region
    // comes from the retained snapshot after the queued GL work.
    std::uint32_t peek_snapshot_wait(int index, bool depth, int x, int y) {
        renderer_.wait_snapshot(index);
        std::uint32_t value = 0;
        if (peek_snapshot(index, depth, x, y, value)) return value;
        renderer_.select_snapshot(index);
        value = depth ? renderer_.peek_depth(x, y) : renderer_.peek_color(x, y);
        renderer_.select_snapshot(-1);
        return value;
    }
    void release_snapshot(int index) { renderer_.release_snapshot(index); }
    int request_depth_readback() { return renderer_.request_depth_readback(); }
    bool depth_readback_value(int handle, int x, int y, std::uint32_t& value) const {
        return renderer_.depth_readback_value(handle, x, y, value);
    }
    void wait_depth_readback(int handle) { renderer_.wait_depth_readback(handle); }
    void release_depth_readback(int handle) { renderer_.release_depth_readback(handle); }
    int token_snapshot(std::uint16_t token) { return token_reads_efb(token) ? renderer_.snapshot_efb(token_region(token)) : -1; }

    void present(const void* frame_buffer, bool black) {
        frame_event('P', std::uint32_t(reinterpret_cast<std::uintptr_t>(frame_buffer)));
        int width = 0, height = 0;
        if (setup_.drawable_size) setup_.drawable_size(width, height);
        FramebufferView view;
        const auto found = frame_buffers_.find(reinterpret_cast<std::uintptr_t>(frame_buffer));
        if (!black && found != frame_buffers_.end()) {
            const auto& info = found->second;
            view.bytes = {static_cast<const std::uint8_t*>(frame_buffer), info.stride * info.height};
            view.width = info.width;
            view.height = info.height;
            view.stride = info.stride;
        }
        const auto& dumps = diagnostics_.efb_dumps;
        if (std::find(dumps.begin(), dumps.end(), retraces_ + 1) != dumps.end()) dump_pending_ = retraces_ + 1;
        const bool presented = renderer_.present(view, width, height, view.bytes.data == nullptr);
        ++retraces_;
        if (diagnostics_.stats && retraces_ % 300 == 0) {
            GpuCounters counters;
            counters.retraces = retraces_;
            counters.presented = presented;
            counters.frame_buffer = frame_buffer;
            counters.known_frame_buffer = found != frame_buffers_.end();
            counters.decoded_bytes = decoded_;
            counters.skipped_draws = draw_failures_;
            renderer_.print_stats(counters);
            print_decoder_counts();
        }
    }

    std::string failure() {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_;
    }

    bool on_thread() const { return std::this_thread::get_id() == thread_.get_id(); }
    bool resident_in(const void* data, std::size_t bytes) const { return renderer_.resident_in(data, bytes); }
    // A transfer's memory as the console has it once the GX has run the commands before it.
    void before_dma(const void* data, std::size_t bytes, bool write) {
        try {
            if (write) renderer_.before_dma_write(data, bytes);
            else renderer_.before_dma_read(data, bytes);
        } catch (const std::exception& error) {
            report(std::string("GX results around a DMA failed: ") + error.what());
        }
    }

private:
    static std::uint32_t word(std::uint8_t address, std::uint32_t value) { return std::uint32_t{address} << 24 | (value & 0xffffff); }

    void trace(const State& state, const Draw& draw) {
        auto f = [&](std::uint16_t a) { float v; const auto w = state.xf(a); std::memcpy(&v, &w, 4); return v; };
        std::printf("gx draw: prim 0x%02x vat %u verts %u viewport %g %g %g %g %g %g proj %g %g %g %g %g %g type %u "
                    "pos0 %g %g %g %g gen 0x%06x vtx %02x%02x%02x%02x mtxidx %u row %g %g %g %g | %g %g %g %g | %g %g %g %g "
                    "normal %u pnidx %u bprev %llu xfrev %llu xfmem %llu retrace %u bpstate %llu tevreg %06x %06x\n",
                    draw.primitive, draw.vat, draw.total_vertices(), f(0x101a), f(0x101b), f(0x101c), f(0x101d),
                    f(0x101e), f(0x101f), f(0x1020), f(0x1021), f(0x1022), f(0x1023), f(0x1024), f(0x1025),
                    state.xf(0x1026), f(0), f(1), f(2), f(3), state.bp(0), draw.vertices.data[0], draw.vertices.data[1],
                    draw.vertices.data[2], draw.vertices.data[3], state.matrix_index(0),
                    f(state.matrix_index(0) * 4u + 0), f(state.matrix_index(0) * 4u + 1), f(state.matrix_index(0) * 4u + 2),
                    f(state.matrix_index(0) * 4u + 3), f(state.matrix_index(0) * 4u + 4), f(state.matrix_index(0) * 4u + 5),
                    f(state.matrix_index(0) * 4u + 6), f(state.matrix_index(0) * 4u + 7), f(state.matrix_index(0) * 4u + 8),
                    f(state.matrix_index(0) * 4u + 9), f(state.matrix_index(0) * 4u + 10), f(state.matrix_index(0) * 4u + 11),
                    unsigned(draw.layout.normal.mode), unsigned(draw.layout.matrix_indices[0]),
                    (unsigned long long)state.bp_state_revision(), (unsigned long long)state.xf_register_revision(),
                    (unsigned long long)state.xf_memory_revision(), retraces_, (unsigned long long)state.bp_state_revision(),
                    state.bp(0xe2), state.bp(0xe3));
        // Which BP registers changed since the previous traced draw.
        std::printf("gx bp changes:");
        for (unsigned a = 0; a < 256; ++a)
            if (state.bp(std::uint8_t(a)) != traced_bp_[a]) {
                std::printf(" %02x=%06x", a, state.bp(std::uint8_t(a)));
                traced_bp_[a] = state.bp(std::uint8_t(a));
            }
        std::printf("\n");
    }

    void print_decoder_counts() {
        const auto& c = decoder_->counts();
        unsigned long long draws = 0, indexed = 0;
        for (unsigned op = 0x80; op <= 0xb8; op += 8) draws += c.general[op];
        for (unsigned op : {0x20u, 0x28u, 0x30u, 0x38u}) indexed += c.general[op];
        std::printf("gpu decode: fast bp %llu cp %llu xf %llu nop %llu; general bp %llu cp %llu xf %llu indexed xf %llu "
                    "draws %llu calls %llu; lists %llu (%llu bytes)\n",
                    (unsigned long long)c.fast_bp, (unsigned long long)c.fast_cp, (unsigned long long)c.fast_xf,
                    (unsigned long long)c.nops, (unsigned long long)c.general[0x61], (unsigned long long)c.general[0x08],
                    (unsigned long long)c.general[0x10], indexed, draws, (unsigned long long)c.general[0x40],
                    (unsigned long long)c.lists, (unsigned long long)c.list_bytes);
    }

    // Development: the EFB as RGBA rows, written as a PPM.
    void dump_efb(unsigned retrace) {
        const auto pixels = renderer_.readback();
        const std::string path = diagnostics_.capture_dir + "/efb-" + std::to_string(retrace) + ".ppm";
        if (pixels.empty()) {
            std::fprintf(stderr, "starlet: gx: the renderer read back no EFB for %s\n", path.c_str());
            return;
        }
        const int w = EfbLayout::width, h = int(pixels.size() / 4 / EfbLayout::width);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; --y)
                for (int x = 0; x < w; ++x) std::fwrite(&pixels[(std::size_t(y) * w + x) * 4], 1, 3, f);
            std::fclose(f);
            std::printf("gpu: EFB written to %s\n", path.c_str());
        } else {
            std::fprintf(stderr, "starlet: gx: cannot write %s\n", path.c_str());
        }
    }

    void copy(const State& state, std::uint32_t value) {
        const std::uint32_t address = ((state.bp(0x4b) & 0xffffff) << 5) & (os::gx_window_size - 1);
        auto* destination = static_cast<std::uint8_t*>(os::physical_to_native(address));
        const bool clear = (value >> 11) & 1;
        if ((value >> 14) & 1) {
            if (dump_pending_) { dump_efb(dump_pending_); dump_pending_ = 0; }
            const auto plan = plan_display_copy(
                state, {word(0x49, state.bp(0x49)), word(0x4a, state.bp(0x4a)), word(0x4d, state.bp(0x4d)),
                        // The copy template without its execute bits (clear, to-XFB).
                        value & 0xffffu & ~((1u << 11) | (1u << 14))},
                clear);
            const std::size_t bytes = display_copy_bytes(plan);
            if (!os::game_visible(destination, bytes)) {
                std::fprintf(stderr, "starlet: gx: display copy to 0x%08x outside game memory\n", address);
                return;
            }
            renderer_.copy_display(plan, destination, bytes);
            PageGenerations::get().wrote(destination, bytes);
            // VISetNextFrameBuffer names the XFB by its pointer.
            frame_buffers_[reinterpret_cast<std::uintptr_t>(destination)] = {unsigned(plan.width), unsigned(plan.height), plan.stride};
        } else {
            const bool depth = (state.bp(0x43) & 7) == 3;
            const auto plan = plan_texture_copy(
                state, {word(0x49, state.bp(0x49)), word(0x4a, state.bp(0x4a)), word(0x4d, state.bp(0x4d)), word(0x52, value), depth},
                clear);
            const std::size_t bytes = texture_copy_bytes(plan);
            if (!os::game_visible(destination, bytes)) {
                std::fprintf(stderr, "starlet: gx: texture copy to 0x%08x outside game memory\n", address);
                return;
            }
            renderer_.copy_texture(plan, destination, bytes, address);
            PageGenerations::get().wrote(destination, bytes);
        }
    }

    void new_decoder() {
        Limits limits;
        limits.merge_primitives = true;
        decoder_ = std::make_unique<Decoder>(memory_, *this, limits, setup_.efb);
    }

    void run() {
        os::name_thread("gx-gpu");
        os::set_guest_allocation(false);
        if (setup_.start) setup_.start();
        new_decoder();
        while (!stopping_) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // A token waiting for its snapshot needs later GL work to collect the readback; with
                // none queued (the guest may be waiting for that token) the GPU thread waits for it.
                while (!pending_.empty() && jobs_.empty()) {
                    if (wake_.wait_for(lock, std::chrono::milliseconds(1), [&] { return !jobs_.empty(); })) break;
                    lock.unlock();
                    if (!renderer_.snapshot_ready(pending_.front().snapshot)) renderer_.wait_snapshot(pending_.front().snapshot);
                    pump();
                    lock.lock();
                }
                wake_.wait(lock, [&] { return !jobs_.empty(); });
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            try {
                if (!job.bytes.empty()) feed(job.bytes);
                if (job.call) job.call();
            } catch (const std::exception& error) {
                const auto& st = decoder_->state();
                char detail[160];
                std::snprintf(detail, sizeof detail, " (bp 00=%06x 22=%06x 43=%06x 44=%06x 68=%06x)", st.bp(0x00), st.bp(0x22),
                              st.bp(0x43), st.bp(0x44), st.bp(0x68));
                report(std::string("GX command failed: ") + error.what() + detail);
                new_decoder();
            }
            pump();
            std::lock_guard<std::mutex> lock(mutex_);
            ++completed_;
            done_.notify_all();
        }
        if (setup_.stop) setup_.stop();
    }

    void feed(const std::vector<std::uint8_t>& bytes) {
        decoded_ += bytes.size();
        const auto status = decoder_->feed({bytes.data(), bytes.size()});
        if (!status) {
            report("GX command stream failed: " + status.message);
            new_decoder();
        }
    }

    void report(const std::string& message) {
        std::fprintf(stderr, "starlet: %s\n", message.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_.empty()) failure_ = message;
    }

    // A requested EFB dump waits for the next display copy, which may clear the EFB.
    unsigned dump_pending_ = 0;
    GpuSetup setup_;
    Renderer& renderer_;
    const Diagnostics& diagnostics_;
    GuestMemory memory_;
    std::unique_ptr<Decoder> decoder_;
    std::unordered_map<std::uintptr_t, FrameBufferInfo> frame_buffers_;
    GpuEvents events_;
    // Draw-done and token interrupts in command order; a token with a snapshot holds back
    // itself and everything after it until its snapshot is read back.
    struct Interrupt {
        bool draw_done;
        std::uint16_t token;
        int snapshot;
    };
    std::deque<Interrupt> pending_;
    void raise(const Interrupt& i) {
        if (i.draw_done) {
            if (events_.draw_done) events_.draw_done();
        } else if (events_.token) {
            events_.token(i.token, i.snapshot);
        } else if (i.snapshot >= 0) {
            renderer_.release_snapshot(i.snapshot);
        }
    }
    void deliver(const Interrupt& i) {
        if (pending_.empty() && i.snapshot < 0) return raise(i);
        pending_.push_back(i);
        pump();
    }
    void pump() {
        while (!pending_.empty() && (pending_.front().snapshot < 0 || renderer_.snapshot_ready(pending_.front().snapshot))) {
            const Interrupt i = pending_.front();
            pending_.pop_front();
            raise(i);
        }
    }
    std::atomic<std::uint16_t> token_{0};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::deque<Job> jobs_;
    std::uint64_t posted_ = 0, completed_ = 0;
    bool stopping_ = false;
    std::string failure_;
    unsigned retraces_ = 0;
    unsigned draw_failures_ = 0;
    std::set<std::string> reported_failures_;
    int trace_draws_;
    std::uint32_t traced_bp_[256] = {};
    std::uint64_t decoded_ = 0;
};

std::unique_ptr<Gpu> gpu;

void (*dirty_state)() = nullptr;
std::string last_failure;

// os::before_dma_read and before_dma_write, on any thread. Only a range that may hold resident
// results waits for the GPU thread: the commands queued before the transfer run first, then the
// renderer settles the range. A read waits for the GL thread's readback too. The GPU and GL
// threads never wait for a guest or DMA thread, so the wait ends.
void note_dma(const void* data, std::size_t bytes, bool write) {
    Gpu* g = gpu.get();
    if (!g || !bytes || !g->resident_in(data, bytes)) return;
    if (g->on_thread()) return g->before_dma(data, bytes, write);
    gpu_call([g, data, bytes, write] { g->before_dma(data, bytes, write); });
}

}

void start_gpu(GpuSetup setup) {
    if (!setup.renderer) throw std::invalid_argument("GpuSetup needs a renderer");
    if (!setup.flush_dirty_state) throw std::invalid_argument("GpuSetup needs flush_dirty_state");
    if (!setup.efb.valid()) throw std::invalid_argument("GpuSetup has an invalid EFB layout");
    arrays_host_order = setup.arrays_host_order;
    if (gpu) throw std::logic_error("the GPU thread is already running");
    host_allocation_scope host;
    dirty_state = setup.flush_dirty_state;
    gpu = std::make_unique<Gpu>(std::move(setup));
    os::set_dma_observer(note_dma);
}

void stop_gpu() {
    host_allocation_scope host;
    os::set_dma_observer(nullptr);
    gpu.reset();
}

void flush_dirty_state() {
    if (!dirty_state) {
        std::fprintf(stderr, "starlet: gx: GXFlush before start_gpu\n");
        std::abort();
    }
    dirty_state();
}

void gpu_submit(std::vector<std::uint8_t>&& bytes) {
    host_allocation_scope host;
    gpu->post({std::move(bytes), {}});
}

void gpu_drain() {
    host_allocation_scope host;
    gpu->drain();
}

bool gpu_idle() {
    host_allocation_scope host;
    return gpu->idle();
}

void gpu_post_function(std::function<void()> call) {
    host_allocation_scope host;
    gpu->post({{}, std::move(call)});
}

void gpu_call_function(const std::function<void()>& call) {
    host_allocation_scope host;
    gpu->post({{}, call});
    gpu->drain();
}

std::uint32_t gpu_peek_depth(int x, int y) { return gpu->peek_depth(x, y); }
std::uint32_t gpu_peek_color(int x, int y) { return gpu->peek_color(x, y); }

void set_gpu_events(GpuEvents events) {
    gpu_call([events] { gpu->set_events(events); });
}

std::uint16_t gpu_token() { return gpu->token(); }

void gpu_note_token_peek(std::uint16_t token, int x0, int y0, int x1, int y1) {
    peek_tokens[token >> 6].fetch_or(std::uint64_t{1} << (token & 63), std::memory_order_relaxed);
    peek_bounds[token & 4095].store(std::uint64_t{1} << 56 | std::uint64_t(y1) << 46 | std::uint64_t(x1) << 36 |
                                        std::uint64_t(y0) << 26 | std::uint64_t(x0) << 16 | token,
                                    std::memory_order_relaxed);
}

bool gpu_peek_snapshot(int index, bool depth, int x, int y, std::uint32_t& value) {
    return gpu->peek_snapshot(index, depth, x, y, value);
}

std::uint32_t gpu_peek_snapshot_wait(int index, bool depth, int x, int y) { return gpu->peek_snapshot_wait(index, depth, x, y); }

void gpu_release_snapshot(int index) { gpu->release_snapshot(index); }

int gpu_request_depth_readback() { return gpu->request_depth_readback(); }
bool gpu_depth_readback_value(int handle, int x, int y, std::uint32_t& value) {
    return gpu->depth_readback_value(handle, x, y, value);
}
void gpu_wait_depth_readback(int handle) { gpu->wait_depth_readback(handle); }
void gpu_release_depth_readback(int handle) { gpu->release_depth_readback(handle); }

void gpu_present(const void* frame_buffer, bool black) {
    gpu_post([frame_buffer, black] { gpu->present(frame_buffer, black); });
}

void gpu_host_order_array(std::uintptr_t base, std::size_t bytes) { host_order_arrays().add(base, bytes); }
void gpu_big_endian_array(std::uintptr_t base, std::size_t bytes) { big_endian_arrays().add(base, bytes); }
void gpu_release_array(std::uintptr_t base) {
    host_order_arrays().remove(base);
    big_endian_arrays().remove(base);
}

const char* gpu_failure() {
    host_allocation_scope host;
    last_failure = gpu ? gpu->failure() : std::string();
    return last_failure.c_str();
}

}

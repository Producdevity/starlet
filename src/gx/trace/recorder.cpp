#include "recorder.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unistd.h>

namespace starlet::gx::trace {
namespace {

struct Config {
    std::string path, trigger, source;
    std::vector<unsigned> starts; // Frame boundaries that start a recording, ascending.
    unsigned frames = 8;
    // Several recordings are numbered: name-1.trace, name-2.trace.
    bool numbered() const { return starts.size() > 1 || !trigger.empty(); }
};

Config read_config() {
    Config c;
    auto text = [](const char* name) { const char* v = std::getenv(name); return std::string(v ? v : ""); };
    c.path = text("STARLET_GX_TRACE");
    c.trigger = text("STARLET_GX_TRACE_TRIGGER");
    c.source = text("STARLET_GX_TRACE_NAME");
    for (const char* p = std::getenv("STARLET_GX_TRACE_START"); p && *p;) {
        char* end = nullptr;
        c.starts.push_back(unsigned(std::strtoul(p, &end, 10)));
        if (end == p) break;
        p = *end == ',' ? end + 1 : end;
    }
    std::sort(c.starts.begin(), c.starts.end());
    if (c.starts.empty() && c.trigger.empty()) c.starts.push_back(0);
    if (const auto s = text("STARLET_GX_TRACE_FRAMES"); !s.empty()) c.frames = std::max(1u, unsigned(std::strtoul(s.c_str(), nullptr, 10)));
    return c;
}

std::uint64_t hash_bytes(const std::uint8_t* p, std::size_t size, std::uint64_t seed) {
    std::uint64_t h = seed * 0x9e3779b97f4a7c15ull + size;
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

struct Interval {
    std::uintptr_t end;
    std::uint32_t address;
};

struct ArrayInfo {
    std::uint64_t size;
    std::uint8_t native_unit;
    bool host_order;
    bool operator!=(const ArrayInfo& o) const { return size != o.size || native_unit != o.native_unit || host_order != o.host_order; }
};

constexpr std::uint32_t synthetic_base = 0xc0000000u;

struct Global {
    std::mutex mutex;
    Config config = read_config();
    const GlesRenderer* renderer = nullptr;
    GlesLimits limits;
    std::atomic<DecoderTap*> main{nullptr};
    std::atomic<bool> recording{false};
    bool done = false, state_written = false;
    unsigned boundaries = 0, frames = 0, recordings = 0;
    std::string file_path;
    std::FILE* file = nullptr;
    std::unordered_map<std::uint64_t, std::uint64_t> chunks;
    std::map<std::uintptr_t, Interval> pointers;
    std::uint32_t next_synthetic = synthetic_base;
    std::unordered_map<std::uint32_t, ArrayInfo> arrays;
};

Global& global() {
    static Global* g = new Global;
    return *g;
}
bool enabled() { return !global().config.path.empty(); }

// The events of one outermost recorded call on a thread. Memory it read goes first.
struct Unit {
    int depth = 0;
    bool active = false;
    bool finishes = false; // The recording's last frame ended inside this unit.
    std::vector<std::uint8_t> memory, events;
};
thread_local Unit unit;
thread_local DecoderTap* current_tap = nullptr;   // Inside one of this tap's sink callbacks.
thread_local bool recorded_callback = false;      // That callback is recorded.
thread_local int renderer_depth = 0;

// The array element ranges the running recorded draw's indices reach, by array base address.
// Readers may expose an array only while they serve a draw (array_view is empty before), so
// the bytes are recorded when a consumer views the array.
struct DrawArrays {
    unsigned count = 0;
    std::uint32_t base[12];
    std::size_t begin[12], end[12];
};
thread_local const DrawArrays* draw_arrays = nullptr;

void write_file(const std::vector<std::uint8_t>& bytes) {
    auto& g = global();
    if (g.file && !bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), g.file);
}

void begin_unit() {
    if (unit.depth++ == 0) {
        unit.active = global().recording.load(std::memory_order_acquire);
        unit.memory.clear();
        unit.events.clear();
    }
}
void finalize_locked(const char* why);
void end_unit() {
    if (--unit.depth || !unit.active) return;
    auto& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.recording.load(std::memory_order_relaxed)) return;
    write_file(unit.memory);
    write_file(unit.events);
    if (unit.finishes) {
        unit.finishes = false;
        finalize_locked("done");
    }
}

// Where memory events go: the running unit, or straight to the file outside one.
template <class F> void memory_event(F&& f) {
    auto& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.recording.load(std::memory_order_relaxed)) return;
    if (unit.depth) {
        if (!unit.active) return;
        Writer w(unit.memory);
        f(w);
        return;
    }
    std::vector<std::uint8_t> bytes;
    Writer w(bytes);
    f(w);
    write_file(bytes);
}

// Caller holds the mutex. Emits the 64-byte chunk parts of [address, address + size) whose
// contents changed since they were last recorded.
void changed_memory(Writer& w, Space space, std::uint32_t address, const std::uint8_t* data, std::size_t size) {
    auto& g = global();
    std::size_t run = SIZE_MAX, i = 0;
    auto emit = [&](std::size_t end) {
        w.op(Op::mem); w.u8(static_cast<std::uint8_t>(space)); w.u32(address + std::uint32_t(run)); w.u32(std::uint32_t(end - run));
        w.raw(data + run, end - run);
        run = SIZE_MAX;
    };
    while (i < size) {
        const std::uint64_t at = std::uint64_t{address} + i;
        const std::size_t length = std::min<std::uint64_t>(size - i, 64 - (at & 63));
        const auto key = std::uint64_t(space) << 40 | at >> 6;
        const auto h = hash_bytes(data + i, length, (at & 63) | std::uint64_t(length) << 8);
        auto& slot = g.chunks[key];
        const bool changed = slot != h;
        slot = h;
        if (changed && run == SIZE_MAX) run = i;
        if (!changed && run != SIZE_MAX) emit(i);
        i += length;
    }
    if (run != SIZE_MAX) emit(size);
}

void record_memory(Space space, std::uint32_t address, const void* data, std::size_t size) {
    if (!size || !data || std::uint64_t{address} + size > (std::uint64_t{1} << 32)) return;
    memory_event([&](Writer& w) { changed_memory(w, space, address, static_cast<const std::uint8_t*>(data), size); });
}
void record_failure(Space space, std::uint32_t address, std::size_t size) {
    memory_event([&](Writer& w) { w.op(Op::fail); w.u8(static_cast<std::uint8_t>(space)); w.u32(address); w.u32(std::uint32_t(size)); });
}

// Caller holds the mutex.
void learn_locked(const void* data, std::size_t size, std::uint32_t address) {
    if (!data || !size) return;
    auto& g = global();
    const auto begin = reinterpret_cast<std::uintptr_t>(data);
    auto& slot = g.pointers[begin];
    if (slot.end < begin + size || slot.address != address) slot = {begin + size, address};
}
void learn(const void* data, std::size_t size, std::uint32_t address) {
    auto& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    learn_locked(data, size, address);
}
// Caller holds the mutex: the canonical address of native bytes, from what reads and copies
// showed of the reader's mapping; otherwise a private address above synthetic_base.
std::uint32_t map_locked(const void* data, std::size_t size) {
    auto& g = global();
    const auto at = reinterpret_cast<std::uintptr_t>(data);
    auto it = g.pointers.upper_bound(at);
    // A containing range among the nearest ones, else the nearest reader range's offset.
    std::uint64_t linear = ~std::uint64_t{0};
    for (unsigned n = 0; n < 64 && it != g.pointers.begin(); ++n) {
        --it;
        const std::uint64_t address = it->second.address + std::uint64_t(at - it->first);
        if (at < it->second.end) return std::uint32_t(address);
        if (linear == ~std::uint64_t{0} && it->second.address < synthetic_base && at - it->first < (std::uintptr_t{512} << 20) &&
            address + size <= synthetic_base)
            linear = address;
    }
    if (linear != ~std::uint64_t{0}) return std::uint32_t(linear);
    const auto address = g.next_synthetic + std::uint32_t(at & 4095);
    g.next_synthetic += std::uint32_t((((at & 4095) + size + 4095) & ~std::size_t{4095}) + 4096);
    learn_locked(data, size, address);
    return address;
}

void finalize_locked(const char* why) {
    auto& g = global();
    if (!g.file) return;
    std::vector<std::uint8_t> bytes;
    Writer w(bytes);
    w.op(Op::end);
    write_file(bytes);
    std::fclose(g.file);
    g.file = nullptr;
    g.recording.store(false, std::memory_order_release);
    g.done = g.config.trigger.empty() && g.recordings >= g.config.starts.size();
    std::fprintf(stderr, "starlet: gx trace: %u frames written to %s (%s)\n", g.frames, g.file_path.c_str(), why);
}

} // namespace

struct DecoderTap {
    DecoderTap(Decoder* d, MemoryReader& m, CommandSink& s, const Limits& l, EfbLayout e)
        : decoder(d), inner_memory(m), inner_sink(s), limits(l), efb(e), memory_(*this), sink_(*this) {
        for (unsigned a = 0; a < 256; ++a) observed[a] = s.observes_bp(std::uint8_t(a));
    }
    bool recording() const { return global().recording.load(std::memory_order_relaxed) && global().main.load(std::memory_order_relaxed) == this; }

    class Memory final : public MemoryReader {
    public:
        explicit Memory(DecoderTap& tap) : tap_(tap) {}
        bool copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
            const bool ok = tap_.inner_memory.copy(address, destination, size);
            if (tap_.recording()) { if (ok) record_memory(Space::canonical, address, destination, size); else record_failure(Space::canonical, address, size); }
            return ok;
        }
        bool copy_indexed_xf(std::uint8_t array, std::uint32_t address, std::uint8_t* destination, std::size_t size) override {
            const bool ok = tap_.inner_memory.copy_indexed_xf(array, address, destination, size);
            if (tap_.recording()) { if (ok) record_memory(Space::xf, address, destination, size); else record_failure(Space::xf, address, size); }
            return ok;
        }
        ByteView view(std::uint32_t address, std::size_t size) override {
            const auto v = tap_.inner_memory.view(address, size);
            if (v.data && tap_.recording()) { learn(v.data, v.size, address); record_memory(Space::canonical, address, v.data, v.size); }
            return v;
        }
        MemoryArrayView array_view(std::uint32_t address) override {
            auto v = tap_.inner_memory.array_view(address);
            if (!tap_.recording()) return v;
            if (v.bytes.data) {
                learn(v.bytes.data, v.bytes.size, address);
                if (draw_arrays)
                    for (unsigned i = 0; i < draw_arrays->count; ++i) {
                        if (draw_arrays->base[i] != address) continue;
                        const auto end = std::min(draw_arrays->end[i], v.bytes.size);
                        const auto begin = draw_arrays->begin[i];
                        if (begin < end) record_memory(Space::raw, address + std::uint32_t(begin), v.bytes.data + begin, end - begin);
                    }
                const ArrayInfo info{v.bytes.size, v.native_unit, v.host_order};
                memory_event([&](Writer& w) {
                    auto& arrays = global().arrays;
                    const auto found = arrays.find(address);
                    if (found != arrays.end() && !(found->second != info)) return;
                    arrays[address] = info;
                    w.op(Op::array); w.u32(address); w.u64(info.size); w.u8(info.native_unit); w.u8(info.host_order);
                });
            }
            v.generation = 0;
            return v;
        }
        std::uint64_t range_generation(const MemoryArrayView& view, std::uint32_t address, std::size_t size) override {
            return tap_.recording() ? 0 : tap_.inner_memory.range_generation(view, address, size);
        }
        std::uint64_t source_generation(const void* data) override {
            return tap_.recording() ? 0 : tap_.inner_memory.source_generation(data);
        }
        SourceIdentity source_identity(const void* bytes, std::size_t size) override {
            if (tap_.recording()) return {bytes, 0};
            return tap_.inner_memory.source_identity(bytes, size);
        }
        ByteView begin_borrow(std::uint32_t address, std::size_t size) override {
            const auto v = tap_.inner_memory.begin_borrow(address, size);
            if (v.data && tap_.recording()) { learn(v.data, v.size, address); record_memory(Space::canonical, address, v.data, v.size); }
            return v;
        }
        void end_borrow() override { tap_.inner_memory.end_borrow(); }

    private:
        DecoderTap& tap_;
    };

    // A sink callback: recorded inside a recorded decoder call, closed by sink_end.
    class Callback {
    public:
        template <class F> Callback(DecoderTap& tap, Op op, F&& arguments)
            : tap_(tap), saved_tap_(current_tap), saved_recorded_(recorded_callback), saved_depth_(tap.api_depth) {
            current_tap = &tap;
            tap.api_depth = 0;
            recorded_ = unit.depth && unit.active && tap.recording() && tap.call_recorded;
            recorded_callback = recorded_;
            if (recorded_) { Writer w(unit.events); w.op(op); arguments(w); }
        }
        template <class F> void end(bool result, F&& extra) {
            if (recorded_) { Writer w(unit.events); w.op(Op::sink_end); w.u8(result); extra(w); }
            ended_ = true;
        }
        ~Callback() {
            if (!ended_) end(false, [](Writer& w) { w.u64(0); });
            current_tap = saved_tap_;
            recorded_callback = saved_recorded_;
            tap_.api_depth = saved_depth_;
        }

    private:
        DecoderTap& tap_;
        DecoderTap* saved_tap_;
        bool saved_recorded_, recorded_ = false, ended_ = false;
        int saved_depth_;
    };

    class Sink final : public CommandSink {
    public:
        explicit Sink(DecoderTap& tap) : tap_(tap) {}
        bool draw(const State& state, const Draw& command, MemoryReader& memory) override {
            Callback call(tap_, Op::sink_draw, [&](Writer& w) { w.u8(command.primitive); w.u16(command.vertex_count); });
            DrawArrays arrays;
            const auto* saved = draw_arrays;
            if (recorded_callback) {
                index_ranges(state, command, arrays);
                draw_arrays = &arrays;
                for (unsigned i = 0; i < arrays.count; ++i) tap_.memory_.array_view(arrays.base[i]);
            }
            struct Restore { const DrawArrays* saved; ~Restore() { draw_arrays = saved; } } restore{saved};
            const bool result = tap_.inner_sink.draw(state, command, memory);
            call.end(result, [](Writer& w) { w.u64(0); });
            return result;
        }
        bool bp_write(const State& state, std::uint8_t address, std::uint32_t value) override {
            Callback call(tap_, Op::sink_bp, [&](Writer& w) { w.u8(address); w.u32(value); });
            const bool result = tap_.inner_sink.bp_write(state, address, value);
            call.end(result, [](Writer& w) { w.u64(0); });
            return result;
        }
        bool observes_bp(std::uint8_t address) const override { return tap_.observed[address]; }
        bool invalidate_vertices() override {
            Callback call(tap_, Op::sink_invalidate, [](Writer&) {});
            const bool result = tap_.inner_sink.invalidate_vertices();
            call.end(result, [](Writer& w) { w.u64(0); });
            return result;
        }
        bool host_command(const State& state, ByteView input, std::size_t& consumed) override {
            Callback call(tap_, Op::sink_host, [&](Writer& w) { w.u64(input.size); });
            const bool result = tap_.inner_sink.host_command(state, input, consumed);
            call.end(result, [&](Writer& w) { w.u64(consumed); });
            return result;
        }
        bool filter_bp(const State& state, std::uint8_t address, std::uint32_t& value) override {
            Callback call(tap_, Op::sink_filter, [&](Writer& w) { w.u8(address); w.u32(value); });
            const bool result = tap_.inner_sink.filter_bp(state, address, value);
            call.end(result, [&](Writer& w) { w.u64(value); });
            return result;
        }

    private:
        // Array elements the draw's indices reach: consumers read them through array_view's
        // pointer, which the reader never sees.
        static void index_ranges(const State& state, const Draw& draw, DrawArrays& out) {
            const auto& layout = draw.layout;
            const Attribute* attributes[12] = {&layout.position, &layout.normal, &layout.colors[0], &layout.colors[1]};
            for (unsigned t = 0; t < 8; ++t) attributes[4 + t] = &layout.texcoords[t];
            std::uint32_t low[12], high[12];
            bool any = false;
            for (unsigned a = 0; a < 12; ++a) {
                low[a] = ~0u; high[a] = 0;
                any |= attributes[a]->mode == AttributeMode::index8 || attributes[a]->mode == AttributeMode::index16;
            }
            if (!any || !layout.stride || !draw.vertices.data) return;
            unsigned matrices = 0;
            for (bool m : layout.matrix_indices) matrices += m;
            auto scan = [&](std::size_t offset, unsigned count) {
                for (unsigned v = 0; v < count; ++v) {
                    const std::size_t at = offset + std::size_t(v) * layout.stride;
                    if (at + layout.stride > draw.vertices.size) return;
                    const std::uint8_t* cursor = draw.vertices.data + at + matrices;
                    for (unsigned a = 0; a < 12; ++a) {
                        const auto& attribute = *attributes[a];
                        if (attribute.mode == AttributeMode::index8 || attribute.mode == AttributeMode::index16) {
                            for (unsigned k = 0; k < attribute.indices; ++k) {
                                const std::uint32_t index = attribute.mode == AttributeMode::index8 ? cursor[0] : std::uint32_t(cursor[0]) << 8 | cursor[1];
                                cursor += attribute.mode == AttributeMode::index8 ? 1 : 2;
                                low[a] = std::min(low[a], index);
                                high[a] = std::max(high[a], index);
                            }
                        } else {
                            cursor += attribute.fifo_bytes;
                        }
                    }
                }
            };
            if (draw.merged()) {
                for (unsigned r = 0; r < draw.run_count; ++r) scan(draw.runs[r].offset, draw.runs[r].vertex_count);
            } else {
                scan(0, draw.vertex_count);
            }
            for (unsigned a = 0; a < 12; ++a) {
                if (low[a] > high[a]) continue;
                const auto stride = state.cp(std::uint8_t(0xb0 + a)) & 255;
                out.base[out.count] = state.cp(std::uint8_t(0xa0 + a));
                out.begin[out.count] = std::size_t(low[a]) * stride;
                out.end[out.count] = std::size_t(high[a]) * stride + 36;
                ++out.count;
            }
        }
        DecoderTap& tap_;
    };

    Decoder* decoder;
    MemoryReader& inner_memory;
    CommandSink& inner_sink;
    Limits limits;
    EfbLayout efb;
    std::bitset<256> observed;
    Memory memory_;
    Sink sink_;
    int api_depth = 0;
    bool call_recorded = false; // The running outermost decoder call is recorded.
};

DecoderTap* attach(Decoder* decoder, MemoryReader& memory, CommandSink& sink, const Limits& limits, EfbLayout efb) {
    return new DecoderTap(decoder, memory, sink, limits, efb);
}
void detach(DecoderTap* tap) {
    auto& g = global();
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.main.load() == tap) {
            if (g.recording.load()) finalize_locked("decoder destroyed");
            g.main.store(nullptr);
        }
    }
    delete tap;
}
MemoryReader& memory(DecoderTap& tap) { return tap.memory_; }
CommandSink& sink(DecoderTap& tap) { return tap.sink_; }

DecoderCall::DecoderCall(DecoderTap* tap, const Decoder& decoder, Op op, ByteView bytes, std::uint32_t argument)
    : tap_(tap), decoder_(decoder) {
    if (!tap_ || !enabled()) { tap_ = nullptr; return; }
    begin_unit();
    if (tap_->api_depth++ || !unit.active || !tap_->recording()) return;
    auto& g = global();
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.state_written) {
            if (unit.depth != 1) return;
            Writer w(unit.events);
            w.op(Op::state);
            TraceAccess::save(decoder, w);
            g.state_written = true;
        }
    }
    recorded_ = true;
    if (unit.depth == 1) tap_->call_recorded = true;
    Writer w(unit.events);
    w.op(op);
    switch (op) {
    case Op::feed: case Op::extend: case Op::inject: w.bytes(bytes.data, bytes.size); break;
    case Op::call_list: w.u8(argument != 0); w.bytes(bytes.data, bytes.size); break;
    case Op::texture_extent: w.u32(argument); break;
    case Op::load_tlut: w.u32(argument); w.bytes(bytes.data, bytes.size); break;
    default: break;
    }
}
DecoderCall::~DecoderCall() {
    if (!tap_) return;
    if (recorded_) {
        Writer w(unit.events);
        w.op(Op::call_end);
        w.u8(static_cast<std::uint8_t>(decoder_.status().error));
        w.u8(result_);
    }
    if (--tap_->api_depth == 0 && unit.depth == 1) tap_->call_recorded = false;
    end_unit();
}

void Encoder::pointer(const void* data, std::size_t size) {
    w.u8(data != nullptr);
    if (!data) return;
    std::lock_guard<std::mutex> lock(global().mutex);
    w.u32(map_locked(data, size));
}
void Encoder::memory(const void* data, std::size_t size) {
    if (!data || !size) return;
    std::uint32_t address;
    {
        std::lock_guard<std::mutex> lock(global().mutex);
        address = map_locked(data, size);
    }
    record_memory(Space::canonical, address, data, size);
}

void Encoder::target(const void* data, std::size_t size, std::uint32_t address) {
    w.u8(data != nullptr);
    if (!data) return;
    std::lock_guard<std::mutex> lock(global().mutex);
    learn_locked(data, size, address);
    w.u32(address);
}

void renderer_created(const GlesRenderer* renderer, const GlesLimits& limits) {
    if (!enabled()) return;
    auto& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.renderer = renderer;
    g.limits = limits;
}
void renderer_destroyed(const GlesRenderer* renderer) {
    if (!enabled()) return;
    auto& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.renderer != renderer) return;
    if (g.recording.load()) finalize_locked("renderer destroyed");
    g.renderer = nullptr;
}

RendererCall::RendererCall(const GlesRenderer* renderer, Op op, const std::function<void(Encoder&)>& arguments) {
    if (!enabled() || renderer != global().renderer) return;
    auto& g = global();
    entered_ = true;
    outermost_ = renderer_depth++ == 0;
    begin_unit();
    if (!outermost_) return;
    boundary_ = op == Op::present_framebuffer || op == Op::present || op == Op::end_frame;
    const bool draw = op == Op::draw_direct || op == Op::draw;
    if (draw && current_tap && !g.main.load()) {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.main.load()) g.main.store(current_tap);
    }
    if (!unit.active || !g.recording.load(std::memory_order_relaxed)) return;
    // Draws replay from their sink callback's state; one outside a recorded callback cannot.
    if (draw && !recorded_callback) return;
    std::vector<std::uint8_t> encoded;
    Writer w(encoded);
    w.op(op);
    if (arguments) {
        Encoder e(w);
        arguments(e);
    }
    unit.events.insert(unit.events.end(), encoded.begin(), encoded.end());
    recorded_ = true;
}
void RendererCall::result(std::int32_t value) {
    if (!recorded_) return;
    Writer w(unit.events);
    w.i32(value);
}
RendererCall::~RendererCall() {
    if (!entered_) return;
    --renderer_depth;
    end_unit();
    if (!outermost_ || !boundary_) return;
    auto& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    ++g.boundaries;
    if (g.recording.load()) {
        if (recorded_ && ++g.frames >= g.config.frames) {
            if (unit.depth) unit.finishes = true;
            else finalize_locked("done");
        }
        return;
    }
    if (g.done || !g.main.load() || !g.renderer) return;
    if (!g.config.trigger.empty()) {
        // Each appearance of the trigger file starts one recording; it is removed for the next.
        if (access(g.config.trigger.c_str(), F_OK) != 0) return;
        unlink(g.config.trigger.c_str());
    } else if (g.recordings >= g.config.starts.size() || g.boundaries < g.config.starts[g.recordings]) {
        return;
    }
    ++g.recordings;
    g.file_path = g.config.path;
    if (g.config.numbered()) {
        const auto slash = g.file_path.rfind('/'), dot = g.file_path.rfind('.');
        const auto at = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : g.file_path.size();
        g.file_path.insert(at, "-" + std::to_string(g.recordings));
    }
    g.file = std::fopen(g.file_path.c_str(), "wb");
    if (!g.file) {
        std::fprintf(stderr, "starlet: gx trace: cannot write %s\n", g.file_path.c_str());
        g.done = true;
        return;
    }
    std::setvbuf(g.file, nullptr, _IOFBF, 4 << 20);
    std::vector<std::uint8_t> bytes;
    Writer w(bytes);
    w.raw(magic, sizeof magic);
    w.u32(version);
    Header h;
    h.renderer = g.limits;
    const auto* tap = g.main.load();
    h.decoder = tap->limits;
    h.efb = tap->efb;
    h.observed_bp = tap->observed;
    h.frames = g.config.frames;
    h.source = g.config.source;
    put(w, h);
    write_file(bytes);
    g.chunks.clear();
    g.arrays.clear();
    g.pointers.clear();
    g.next_synthetic = synthetic_base;
    g.frames = 0;
    g.state_written = false;
    g.recording.store(true, std::memory_order_release);
    std::fprintf(stderr, "starlet: gx trace: recording %u frames from boundary %u\n", g.config.frames, g.boundaries);
}

} // namespace starlet::gx::trace

#pragma once

#include "starlet/gx/state.hpp"

#include <array>
#include <bitset>
#include <string>
#include <vector>

namespace starlet::gx {

struct MemoryArrayView {
    ByteView bytes;
    // Zero means canonical GX bytes; 2/4 mean native-endian words of that width.
    std::uint8_t native_unit = 0;
    // The array was written by native code: every multi-byte number is in host byte order
    // at its own width, and byte-sized values read as stored. Overrides native_unit.
    bool host_order = false;
    // Advances whenever the CPU reports writing the source (cache store/flush);
    // zero when the reader cannot track writes.
    std::uint64_t generation = 0;
};

class MemoryReader {
public:
    virtual ~MemoryReader() = default;
    // GX addresses are tokens in a registered byte-addressable space, never native pointers.
    // Return exactly size canonical big-endian GX bytes, or false. Native arrays need
    // an explicit typed view/conversion. Do not call back into Decoder.
    virtual bool copy(std::uint32_t address, std::uint8_t* destination, std::size_t size) = 0;
    // An indexed XF load (CP opcodes 0x20, 0x28, 0x30, 0x38) reads matrices or lights from
    // array 12 + n. Games fill those arrays on the CPU each frame; a port whose CPU memory
    // is not canonical returns them converted here. The default reads through copy().
    virtual bool copy_indexed_xf(std::uint8_t array, std::uint32_t address, std::uint8_t* destination, std::size_t size) {
        (void)array;
        return copy(address, destination, size);
    }
    // Optional exact canonical span, borrowed only until the next reader call or
    // source mutation. Consumers must finish reading it before returning a snapshot.
    virtual ByteView view(std::uint32_t, std::size_t) { return {}; }
    // Optional array suffix, stable across reader calls until the synchronous draw
    // callback returns. No pointer from it may enter a retained snapshot.
    virtual MemoryArrayView array_view(std::uint32_t) { return {}; }
    // Write generation of the size bytes at address, inside view (array_view of the array
    // base): an unchanged nonzero value promises unchanged bytes. For readers that track
    // writes finer than whole arrays; the default is the array's generation.
    virtual std::uint64_t range_generation(const MemoryArrayView& view, std::uint32_t address, std::size_t size) {
        (void)address;
        (void)size;
        return view.generation;
    }
    // Write generation of the registered source containing native bytes, such as a
    // draw's display-list payload; zero when unknown. See MemoryArrayView::generation.
    virtual std::uint64_t source_generation(const void*) { return 0; }
    // A stable name for size native bytes a draw reads, with its write generation: the source
    // they were copied from when the host copies display lists into its FIFO, otherwise the
    // bytes themselves. Equal names and nonzero generations promise equal bytes.
    struct SourceIdentity {
        const void* key = nullptr;
        std::uint64_t generation = 0;
    };
    virtual SourceIdentity source_identity(const void* bytes, std::size_t) { return {bytes, source_generation(bytes)}; }
    // Optional exact canonical span that stays valid across reader calls until the
    // matching end_borrow; an empty result takes no borrow.
    virtual ByteView begin_borrow(std::uint32_t, std::size_t) { return {}; }
    virtual void end_borrow() {}
};

// One primitive command of a merged draw.
struct PrimitiveRun {
    std::uint8_t primitive = 0; // Opcode without its VAT bits.
    std::uint16_t vertex_count = 0;
    std::uint32_t offset = 0;   // Of the run's first vertex within Draw::vertices.
};

struct Draw {
    std::uint8_t primitive = 0;
    std::uint8_t vat = 0;
    std::uint16_t vertex_count = 0;
    VertexLayout layout;
    ByteView vertices;
    // Set only under Limits::merge_primitives: consecutive quad, triangle, strip and fan
    // commands of one VAT that no other command separates, so one state draws them all.
    // vertices spans every run, each later one after its own three-byte header; primitive
    // and vertex_count repeat the first run.
    const PrimitiveRun* runs = nullptr;
    std::uint16_t run_count = 0;
    bool merged() const { return run_count != 0; }
    std::uint32_t total_vertices() const;
};

class CommandSink {
public:
    virtual ~CommandSink() = default;
    // Synchronous callbacks: views expire on return. Retained work must own state and
    // vertex/array/texture snapshots. The decoder does not retire GPU resources.
    virtual bool draw(const State& state, const Draw& command, MemoryReader& memory) = 0;
    virtual bool bp_write(const State& state, std::uint8_t address, std::uint32_t value) = 0;
    // Fixed for a decoder's lifetime. False omits only the callback, never the register write or TLUT operation.
    virtual bool observes_bp(std::uint8_t) const { return true; }
    virtual bool invalidate_vertices() = 0;
    // A command whose opcode is in Limits::host_opcodes, starting at input: sets consumed to
    // its size, which must lie within input, and may apply the commands it stands for with
    // Decoder::inject. False rejects the stream.
    virtual bool host_command(const State&, ByteView, std::size_t&) { return false; }
    // A stream BP write to an address in Limits::filtered_bp, before it applies: may rewrite
    // value; false drops the write.
    virtual bool filter_bp(const State&, std::uint8_t, std::uint32_t&) { return true; }
};

enum class Error {
    none, invalid_input, unknown_opcode, unsupported_register, invalid_vertex_layout,
    truncated_command, invalid_xf_range, memory_read, display_list_depth, work_limit,
    command_size, sink_rejected, reentrant_call, processing_exception
};

struct Status {
    Error error = Error::none;
    std::string message;
    explicit operator bool() const { return error == Error::none; }
};

struct Limits {
    std::size_t max_command_bytes = 16 * 1024 * 1024;
    std::size_t max_display_list_bytes = 16 * 1024 * 1024;
    std::size_t max_expanded_bytes = 64 * 1024 * 1024;
    std::size_t max_commands = 1024 * 1024;
    unsigned max_display_list_depth = 16;
    // Hands back-to-back triangle primitives to the sink as one Draw (see Draw::runs).
    // Enable only for sinks that read runs.
    bool merge_primitives = false;
    // Opcodes of a host's own commands, each handed whole to CommandSink::host_command.
    std::bitset<256> host_opcodes;
    // BP addresses whose stream writes pass through CommandSink::filter_bp first.
    std::bitset<256> filtered_bp;
    // BP addresses whose stream writes are ignored, as filter_bp dropping them would.
    std::bitset<256> dropped_bp;
};

class Decoder {
public:
    Decoder(MemoryReader& memory, CommandSink& sink, Limits limits = {}, EfbLayout efb = {});
    const State& state() const { return state_; }
    const Status& status() const { return status_; }
    // Commands decoded so far: those fast_command applied, the general path's by opcode
    // (VAT bits cleared for primitives), and display lists run.
    struct Counts {
        std::uint64_t fast_bp = 0, fast_cp = 0, fast_xf = 0, nops = 0, lists = 0, list_bytes = 0;
        std::array<std::uint64_t, 256> general{};
    };
    const Counts& counts() const { return counts_; }
    // Feeds may end anywhere, including within a vertex. Whole commands commit in order.
    // Errors latch; discard this decoder after failure. Earlier commands remain applied.
    // Processing exceptions latch failure and propagate without changing their type.
    Status feed(ByteView bytes);
    // Appends bytes that only extend a buffered command which they cannot complete;
    // false leaves the decoder unchanged and the caller uses feed.
    bool extend_pending(ByteView bytes);
    Status finish();
    // Synchronous canonical span with the same outer-call, nesting and work
    // budgets as opcode 0x40. Owns a snapshot before invoking the first callback,
    // unless owned: the caller then keeps the bytes unchanged until it returns.
    Status call_display_list(ByteView bytes, bool owned = false);
    Status write_u8(std::uint8_t value);
    Status write_u16(std::uint16_t value);
    Status write_u32(std::uint32_t value);
    Status write_f32(float value);
    // Applies whole hardware commands at the current stream position: between feeds, or
    // from host_command and filter_bp. They skip host_opcodes and filter_bp; a display list
    // they call decodes as stream commands.
    Status inject(ByteView bytes);
    // Host extension: see TextureExtent. Up to 8192 texels per side; {} clears it. Counts
    // as a BP change, as the image register it widens is one.
    void set_texture_extent(std::uint8_t map, TextureExtent extent);

private:
    struct Budget { std::size_t commands = 0; std::size_t bytes = 0; };
    enum class Parse { complete, incomplete, failed };
    Parse command(ByteView input, std::size_t& consumed, unsigned depth, Budget& budget);
    // command() for a complete BP write; false leaves anything else to command().
    bool bp_command(const std::uint8_t* input, std::size_t size, unsigned depth, Budget& budget, Parse& result);
    Parse host_command(ByteView input, std::size_t& consumed, unsigned depth, Budget& budget);
    // apply_bp for a write from the stream: filtered addresses go through the sink first.
    bool stream_bp(std::uint32_t word);
    bool execute_list(std::uint32_t address, std::size_t size, unsigned depth, Budget& budget);
    bool charge_list(std::size_t size, unsigned depth, Budget& budget);
    bool parse_list(ByteView bytes, unsigned depth, Budget& budget);
    bool apply_bp(std::uint32_t word);
    // A transfer of count XF words from big-endian words, within State::xf_words.
    void apply_xf(std::size_t address, const std::uint8_t* words, std::size_t count);
    bool load_palette(std::uint32_t control);
    bool read_memory(std::uint32_t address, std::uint8_t* destination, std::size_t size);
    bool fail(Error error, const char* message);
    void latch_exception() noexcept;
    State state_;
    MemoryReader& memory_;
    CommandSink& sink_;
    Limits limits_;
    Status status_;
    // Layouts depend only on CP registers; reused while cp_revision is unchanged.
    struct CachedLayout { std::uint64_t revision = ~std::uint64_t{0}; VertexLayout layout; };
    std::array<CachedLayout, 8> layouts_{};
    std::vector<PrimitiveRun> runs_;
    std::vector<std::uint8_t> pending_;
    std::size_t pending_required_ = 0; // Lower bound on the buffered command size.
    bool feeding_ = false;
    bool injecting_ = false;
    bool host_callback_ = false; // Inside host_command or filter_bp, where inject may run.
    // The feed loop and display lists apply NOP runs, these BP and CP writes and XF
    // writes themselves (fast_command).
    enum class Fast { none, applied, failed };
    Fast fast_command(const std::uint8_t* p, std::size_t remaining, unsigned depth, Budget& budget, std::size_t& consumed);
    bool fast_nop_ = false, fast_xf_ = false;
    std::bitset<256> plain_bp_, dropped_bp_, plain_cp_, notify_bp_;
    Budget* budget_ = nullptr; // The running feed's budget, which injected commands share.
    Counts counts_;
};

} // namespace starlet::gx

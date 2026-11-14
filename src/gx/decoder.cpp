#include "starlet/gx/decoder.hpp"

#include <stdexcept>

#include <algorithm>
#include <cstring>
#include <limits>
#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#endif

namespace starlet::gx {
namespace {
std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | p[1]);
}
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | p[3];
}
bool supported_cp(std::uint8_t address) {
    return address == 0x20 || address == 0x30 || address == 0x40 || address == 0x50 || address == 0x60 ||
           (address >= 0x70 && address <= 0x97 && (address & 15) < 8) ||
           (address >= 0xa0 && address <= 0xbf);
}
bool triangles(std::uint8_t opcode) {
    switch (opcode & 0xf8) {
    case 0x80: case 0x90: case 0x98: case 0xa0: return true;
    default: return false;
    }
}
bool primitive(std::uint8_t opcode) {
    switch (opcode & 0xf8) {
    case 0x80: case 0x90: case 0x98: case 0xa0: case 0xa8: case 0xb0: case 0xb8: return true;
    default: return false;
    }
}
}

Decoder::Decoder(MemoryReader& memory, CommandSink& sink, Limits limits, EfbLayout efb)
    : state_(efb), memory_(memory), sink_(sink), limits_(limits) {
    if (limits_.max_command_bytes < 6 || limits_.max_expanded_bytes < limits_.max_command_bytes ||
        limits_.max_commands == 0 || limits_.max_display_list_depth > 64) {
        fail(Error::invalid_input, "invalid GX decoder limits");
    }
    fast_nop_ = !limits_.host_opcodes.test(0x00);
    fast_xf_ = !limits_.host_opcodes.test(0x10) && limits_.max_command_bytes >= 69;
    for (unsigned a = 0; a < 256; ++a) {
        notify_bp_[a] = sink.observes_bp(static_cast<std::uint8_t>(a));
        // TLUT loads, texture-cache invalidation and the mask need apply_bp.
        const bool special = a == 0x65 || a == 0x66 || a == 0xfe;
        const bool stream = !limits_.host_opcodes.test(0x61);
        plain_bp_[a] = stream && !limits_.filtered_bp.test(a) && !limits_.dropped_bp.test(a) && !special;
        dropped_bp_[a] = stream && limits_.dropped_bp.test(a);
        plain_cp_[a] = !limits_.host_opcodes.test(0x08) && supported_cp(static_cast<std::uint8_t>(a));
    }
}

bool Decoder::fail(Error error, const char* message) {
    if (status_) {
        status_ = {error, message};
    }
    return false;
}

void Decoder::latch_exception() noexcept {
    if (status_) {
        // Allocation failure must not trigger another allocation while unwinding.
        status_.error = Error::processing_exception;
        status_.message.clear();
    }
}

bool Decoder::read_memory(std::uint32_t address, std::uint8_t* destination, std::size_t size) {
    const std::uint64_t end = std::uint64_t{address} + size;
    if (end > (std::uint64_t{1} << 32) || !memory_.copy(address, destination, size)) {
        return fail(Error::memory_read, "GX memory range is unavailable");
    }
    return true;
}

bool Decoder::load_palette(std::uint32_t control) {
    const auto offset = std::size_t{control & 0x3ff} * 512;
    const auto size = std::size_t{(control >> 10) & 0x7ff} * 32;
    if (size > State::palette_bytes - offset || state_.palette_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail(Error::invalid_input, "GX TLUT load exceeds palette memory or generation range");
    }
    std::vector<std::uint8_t> bytes(size);
    // The source is 21 bits of 32-byte units; the console ignores register bits 21-23.
    if (size && !read_memory((state_.bp_[0x64] & 0x1fffff) << 5, bytes.data(), size)) {
        return false;
    }
    const auto generation = ++state_.palette_generation_;
    for (std::size_t copied = 0; copied < size;) {
        const auto block_index = (offset + copied) / 512;
        auto block = state_.palette_[block_index] ? std::make_shared<PaletteBlock>(*state_.palette_[block_index])
                                                : std::make_shared<PaletteBlock>();
        const auto count = std::min<std::size_t>(size - copied, 512);
        std::copy_n(bytes.data() + copied, count, block->bytes.data());
        block->valid_chunks |= static_cast<std::uint16_t>((1u << (count / 32)) - 1u);
        block->generation = generation;
        state_.palette_[block_index] = std::move(block);
        copied += count;
    }
    return true;
}

bool Decoder::apply_bp(std::uint32_t word) {
    const auto address = static_cast<std::uint8_t>(word >> 24);
    const auto value = word & 0xffffff;
    // Rewriting an unchanged register leaves the revisions alone; TLUT loads and
    // texture-cache invalidation are operations even when the value repeats.
    bool changed = address == 0x65 || address == 0x66;
    bool color_register = false;
    if (address == 0xfe) {
        changed |= state_.bp_mask_ != value;
        state_.bp_mask_ = value;
        state_.bp_[address] = value;
    } else {
        const auto merged = (state_.bp_[address] & ~state_.bp_mask_) | (value & state_.bp_mask_);
        if (address >= 0xe0 && address <= 0xe7 && state_.bp_mask_ != 0xffffff &&
            ((merged ^ state_.bp_[address]) & 0x800000)) {
            return fail(Error::unsupported_register, "partially masked TEV bank switch is unsupported");
        }
        if (address == 0x65 && !load_palette(merged)) {
            return false;
        }
        changed |= state_.bp_[address] != merged || state_.bp_mask_ != 0xffffff;
        state_.bp_[address] = merged;
        if (address >= 0xe0 && address <= 0xe7) {
            state_.tev_registers_[(merged >> 23) & 1][address - 0xe0] = merged & 0x7fffff;
            color_register = !(merged & 0x800000) && state_.bp_mask_ == 0xffffff;
        }
        state_.bp_mask_ = 0xffffff;
        state_.bp_[0xfe] = state_.bp_mask_;
    }
    if (changed) {
        state_.mark_bp(address);
        ++state_.revision_;
        ++state_.bp_revision_;
        if (!color_register) ++state_.bp_state_revision_;
    }
    if (notify_bp_[address] && !sink_.bp_write(state_, address, state_.bp_[address])) {
        return fail(Error::sink_rejected, "GX sink rejected BP operation");
    }
    return true;
}

bool Decoder::charge_list(std::size_t size, unsigned depth, Budget& budget) {
    if (depth > limits_.max_display_list_depth) {
        return fail(Error::display_list_depth, "GX display-list nesting limit exceeded");
    }
    if (size > limits_.max_display_list_bytes) {
        return fail(Error::command_size, "GX display-list size limit exceeded");
    }
    if (size > limits_.max_expanded_bytes - budget.bytes) {
        return fail(Error::work_limit, "GX expanded byte limit exceeded");
    }
    budget.bytes += size;
    return true;
}

bool Decoder::execute_list(std::uint32_t address, std::size_t size, unsigned depth, Budget& budget) {
    if (!charge_list(size, depth, budget)) return false;
    ++counts_.lists;
    counts_.list_bytes += size;
    // A list's commands are stream commands, also when an injected call runs it.
    struct Stream { bool& injecting; bool was = injecting; ~Stream() { injecting = was; } } stream{injecting_};
    injecting_ = false;
    // Canonical list memory is parsed in place, so draws name the source bytes.
    if (size) if (const auto borrowed = memory_.begin_borrow(address, size); borrowed.data) {
        struct End { MemoryReader& memory; ~End() { memory.end_borrow(); } } end{memory_};
        return parse_list(borrowed, depth, budget);
    }
    std::vector<std::uint8_t> bytes(size);
    if (size && !read_memory(address, bytes.data(), size)) {
        return false;
    }
    return parse_list({bytes.data(), bytes.size()}, depth, budget);
}

// NOP runs, plain BP and CP writes and XF matrix-memory writes, applied with the general
// path's effects. A stream command (depth 0) also charges its bytes; a list's bytes were
// charged when the list was called. Anything else, or a limit, is left to the general path.
Decoder::Fast Decoder::fast_command(const std::uint8_t* p, std::size_t remaining, unsigned depth, Budget& budget,
                                    std::size_t& consumed) {
    const bool stream = depth == 0;
    const bool room = budget.commands < limits_.max_commands && (!stream || limits_.max_expanded_bytes - budget.bytes >= 6);
    const auto charge = [&](std::size_t size) { ++budget.commands; if (stream) budget.bytes += size; };
    if (p[0] == 0x61 && room && remaining >= 5 && dropped_bp_[p[1]]) {
        ++counts_.fast_bp;
        charge(5);
        consumed = 5;
        return Fast::applied;
    }
    if (p[0] == 0x61 && room && remaining >= 5 && plain_bp_[p[1]] && state_.bp_mask_ == 0xffffff) {
        consumed = 0;
        do {
            const auto* command = p + consumed;
            const auto address = command[1];
            const auto value = std::uint32_t{command[2]} << 16 | std::uint32_t{command[3]} << 8 | command[4];
            charge(5);
            ++counts_.fast_bp;
            const bool tev = address >= 0xe0 && address <= 0xe7;
            if (tev) state_.tev_registers_[(value >> 23) & 1][address - 0xe0] = value & 0x7fffff;
            if (state_.bp_[address] != value) {
                state_.bp_[address] = value;
                state_.mark_bp(address);
                ++state_.revision_;
                ++state_.bp_revision_;
                if (!tev || (value & 0x800000)) ++state_.bp_state_revision_;
            }
            if (notify_bp_[address] && !sink_.bp_write(state_, address, value)) {
                fail(Error::sink_rejected, "GX sink rejected BP operation");
                return Fast::failed;
            }
            if (!status_) return Fast::failed;
            consumed += 5;
        } while (remaining - consumed >= 5 && p[consumed] == 0x61 && plain_bp_[p[consumed + 1]] &&
                 state_.bp_mask_ == 0xffffff && budget.commands < limits_.max_commands &&
                 (!stream || limits_.max_expanded_bytes - budget.bytes >= 5));
        return Fast::applied;
    }
    if (p[0] == 0 && fast_nop_) {
        std::size_t run = 1;
        while (run < remaining && p[run] == 0) ++run;
        run = std::min(run, limits_.max_commands - budget.commands);
        if (stream) run = std::min(run, limits_.max_expanded_bytes - budget.bytes);
        if (run) {
            counts_.nops += run;
            budget.commands += run;
            if (stream) budget.bytes += run;
            consumed = run;
            return Fast::applied;
        }
    }
    if (p[0] == 0x10 && room && fast_xf_ && remaining >= 5 && p[1] == 0 && p[2] < 16) {
        const std::size_t count = std::size_t{p[2]} + 1, size = 5 + count * 4;
        const auto address = be16(p + 3);
        if (remaining >= size && address + count <= State::xf_words && (!stream || limits_.max_expanded_bytes - budget.bytes >= size)) {
            apply_xf(address, p + 5, count);
            charge(size);
            ++counts_.fast_xf;
            consumed = size;
            return Fast::applied;
        }
    }
    if (p[0] == 0x08 && room && remaining >= 6 && plain_cp_[p[1]]) {
        consumed = 0;
        do {
            const auto* command = p + consumed;
            const auto address = command[1];
            const auto value = be32(command + 2);
            charge(6);
            ++counts_.fast_cp;
            if (state_.cp_[address] != value) {
                state_.cp_[address] = value;
                ++state_.revision_;
                ++state_.cp_revision_;
                if (address < 0xa0) ++state_.cp_layout_revision_;
            }
            consumed += 6;
        } while (remaining - consumed >= 6 && p[consumed] == 0x08 && plain_cp_[p[consumed + 1]] &&
                 budget.commands < limits_.max_commands && (!stream || limits_.max_expanded_bytes - budget.bytes >= 6));
        return Fast::applied;
    }
    return Fast::none;
}

void Decoder::apply_xf(std::size_t address, const std::uint8_t* words, std::size_t count) {
    bool memory_changed = false, register_changed = false;
    auto* xf = state_.xf_.data() + address;
    const std::size_t memory_words = address < 0x1000 ? std::min<std::size_t>(count, 0x1000 - address) : 0;
    std::size_t i = 0;
#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    auto changed = vdupq_n_u32(0);
    for (; i + 4 <= memory_words; i += 4) {
        const auto value = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(words + i * 4)));
        changed = vorrq_u32(changed, veorq_u32(value, vld1q_u32(xf + i)));
        vst1q_u32(xf + i, value);
    }
    memory_changed = vmaxvq_u32(changed) != 0;
#endif
    for (; i < memory_words; ++i) {
        const auto word = be32(words + i * 4);
        memory_changed |= xf[i] != word;
        xf[i] = word;
    }
    for (std::size_t i = memory_words; i < count; ++i) {
        const auto word = be32(words + i * 4);
        // Indexed draws latch their last vertex's matrix indices without touching
        // the register image, so a rewrite of the same value still restores them.
        if (address + i == 0x1018 || address + i == 0x1019) {
            const unsigned first = address + i == 0x1018 ? 0 : 5;
            const unsigned latch_count = first == 0 ? 5 : 4;
            bool latched = false;
            for (unsigned m = 0; m < latch_count; ++m) {
                const auto value = static_cast<std::uint8_t>((word >> (6 * m)) & 63);
                latched |= state_.matrix_indices_[first + m] != value;
                state_.matrix_indices_[first + m] = value;
            }
            if (latched) {
                ++state_.revision_;
                ++state_.matrix_index_revision_;
            }
        }
        register_changed |= xf[i] != word;
        state_.mark_xf(address + i);
        xf[i] = word;
    }
    // Programs key on the GX_PTIDENTITY rows, so writes there act on registers too.
    if (memory_changed && address < 0x600 && address + count > 0x5f4) register_changed = true;
    if (memory_changed || register_changed) ++state_.revision_;
    if (memory_changed) {
        ++state_.xf_memory_revision_;
        if (address + count > 0x500) ++state_.xf_post_light_revision_;
        state_.last_xf_memory_write_ = {state_.xf_memory_revision_, static_cast<std::uint16_t>(address),
            static_cast<std::uint16_t>(std::min<std::size_t>(address + count, 0x1000))};
    }
    if (register_changed) ++state_.xf_register_revision_;
}

bool Decoder::parse_list(ByteView bytes, unsigned depth, Budget& budget) {
    std::size_t offset = 0;
    while (offset < bytes.size) {
        // Material and shape lists are mostly the same plain writes as the stream.
        std::size_t fast = 0;
        const auto applied = fast_command(bytes.data + offset, bytes.size - offset, depth, budget, fast);
        if (applied == Fast::failed) return false;
        if (applied == Fast::applied) { offset += fast; continue; }
        std::size_t consumed = 5;
        Parse result;
        if (!bp_command(bytes.data + offset, bytes.size - offset, depth, budget, result))
            result = command({bytes.data + offset, bytes.size - offset}, consumed, depth, budget);
        if (result == Parse::incomplete) {
            return fail(Error::truncated_command, "GX display list ends inside a command");
        }
        if (result == Parse::failed) {
            return false;
        }
        offset += consumed;
    }
    return true;
}

bool Decoder::bp_command(const std::uint8_t* input, std::size_t size, unsigned depth, Budget& budget, Parse& result) {
    if (size < 5 || input[0] != 0x61 || limits_.max_command_bytes < 5) return false;
    if (++budget.commands > limits_.max_commands || (depth == 0 && 5 > limits_.max_expanded_bytes - budget.bytes)) {
        fail(Error::work_limit, "GX command work limit exceeded");
        result = Parse::failed;
        return true;
    }
    if (depth == 0) budget.bytes += 5;
    ++counts_.general[0x61];
    result = stream_bp(be32(input + 1)) && status_ ? Parse::complete : Parse::failed;
    return true;
}

namespace {
struct HostCallback {
    bool& active;
    bool was = active;
    explicit HostCallback(bool& flag) : active(flag) { active = true; }
    ~HostCallback() { active = was; }
};
} // namespace

bool Decoder::stream_bp(std::uint32_t word) {
    const auto address = static_cast<std::uint8_t>(word >> 24);
    if (!injecting_ && limits_.dropped_bp.test(address)) return true;
    if (!injecting_ && limits_.filtered_bp.test(address)) {
        std::uint32_t value = word & 0xffffff;
        bool keep;
        {
            const HostCallback callback{host_callback_};
            keep = sink_.filter_bp(state_, address, value);
        }
        if (!keep) return status_.error == Error::none;
        word = std::uint32_t{address} << 24 | (value & 0xffffff);
    }
    return apply_bp(word);
}

Decoder::Parse Decoder::host_command(ByteView input, std::size_t& consumed, unsigned depth, Budget& budget) {
    consumed = 0;
    bool handled;
    {
        const HostCallback callback{host_callback_};
        handled = sink_.host_command(state_, input, consumed);
    }
    if (!handled) {
        fail(Error::sink_rejected, "GX sink rejected a host command");
        return Parse::failed;
    }
    if (!status_) return Parse::failed;
    if (!consumed || consumed > input.size) {
        fail(Error::truncated_command, "GX host command extends past its feed");
        return Parse::failed;
    }
    if (++budget.commands > limits_.max_commands || (depth == 0 && consumed > limits_.max_expanded_bytes - budget.bytes)) {
        fail(Error::work_limit, "GX command work limit exceeded");
        return Parse::failed;
    }
    if (depth == 0) budget.bytes += consumed;
    return Parse::complete;
}

void Decoder::set_texture_extent(std::uint8_t map, TextureExtent extent) {
    if (map >= 8 || extent.width > 8192 || extent.height > 8192 || (!extent.width != !extent.height))
        throw std::invalid_argument("invalid host texture extent");
    auto& current = state_.texture_extents_[map];
    if (current == extent) return;
    current = extent;
    state_.draw_word(262 + map, std::uint32_t{extent.width} | (std::uint32_t{extent.height} << 16));
    ++state_.revision_;
    ++state_.bp_revision_;
    ++state_.bp_state_revision_;
}

std::uint32_t Draw::total_vertices() const {
    if (!runs) return vertex_count;
    std::uint32_t total = 0;
    for (std::size_t r = 0; r < run_count; ++r) total += runs[r].vertex_count;
    return total;
}

Decoder::Parse Decoder::command(ByteView input, std::size_t& consumed, unsigned depth, Budget& budget) {
    consumed = 1;
    runs_.clear();
    if (input.size == 0) {
        return Parse::incomplete;
    }
    const auto opcode = input.data[0];
    if (!injecting_ && limits_.host_opcodes.test(opcode)) return host_command(input, consumed, depth, budget);
    const VertexLayout* layout = nullptr;
    std::uint16_t vertex_count = 0;
    if (opcode == 0 || opcode == 0x48) {
        consumed = 1;
    } else if (opcode == 0x08) {
        consumed = 6;
    } else if (opcode == 0x61 || opcode == 0x20 || opcode == 0x28 || opcode == 0x30 || opcode == 0x38) {
        consumed = 5;
    } else if (opcode == 0x40) {
        consumed = 9;
    } else if (opcode == 0x10) {
        consumed = 5;
        if (input.size >= 5) {
            if (input.data[1] != 0 || (input.data[2] & 0xf0) != 0) {
                fail(Error::invalid_input, "GX XF transfer count exceeds sixteen words");
                return Parse::failed;
            }
            consumed += (std::size_t{input.data[2]} + 1) * 4;
        }
    } else if (primitive(opcode)) {
        consumed = 3;
        if (input.size >= 3) {
            vertex_count = be16(input.data + 1);
            auto& cached = layouts_[opcode & 7];
            if (cached.revision != state_.cp_layout_revision_) {
                std::string error;
                if (!state_.vertex_layout(opcode & 7, cached.layout, error)) {
                    cached.revision = ~std::uint64_t{0};
                    fail(Error::invalid_vertex_layout, error.c_str());
                    return Parse::failed;
                }
                cached.revision = state_.cp_layout_revision_;
            }
            layout = &cached.layout;
            consumed += layout->stride * vertex_count;
            if (limits_.merge_primitives && triangles(opcode) && vertex_count && input.size >= consumed) {
                std::uint32_t total = vertex_count;
                while (consumed + 3 <= input.size && runs_.size() < 0xfffe) {
                    const auto next = input.data[consumed];
                    const std::uint16_t count = be16(input.data + consumed + 1);
                    const std::size_t bytes = 3 + layout->stride * count;
                    if ((next & 7) != (opcode & 7) || !triangles(next) || !count || total + count > 0xffff ||
                        bytes > input.size - consumed || consumed + bytes > limits_.max_command_bytes)
                        break;
                    if (runs_.empty()) runs_.push_back({static_cast<std::uint8_t>(opcode & 0xf8), vertex_count, 0});
                    runs_.push_back({static_cast<std::uint8_t>(next & 0xf8), count, static_cast<std::uint32_t>(consumed)});
                    total += count;
                    consumed += bytes;
                }
            }
        }
    } else {
        fail(Error::unknown_opcode, "unsupported GX opcode");
        return Parse::failed;
    }
    if (consumed > limits_.max_command_bytes) {
        fail(Error::command_size, "GX command size limit exceeded");
        return Parse::failed;
    }
    if (input.size < consumed) {
        return Parse::incomplete;
    }
    budget.commands += runs_.empty() ? 1 : runs_.size();
    if (budget.commands > limits_.max_commands || (depth == 0 && consumed > limits_.max_expanded_bytes - budget.bytes)) {
        fail(Error::work_limit, "GX command work limit exceeded");
        return Parse::failed;
    }
    if (depth == 0) {
        budget.bytes += consumed;
    }
    ++counts_.general[primitive(opcode) ? opcode & 0xf8 : opcode];
    bool ok = true;
    if (opcode == 0x08) {
        const auto address = input.data[1];
        if (!supported_cp(address)) {
            ok = fail(Error::unsupported_register, "unsupported GX CP register");
        } else {
            const auto value = be32(input.data + 2);
            if (state_.cp_[address] != value) {
                state_.cp_[address] = value;
                ++state_.revision_;
                ++state_.cp_revision_;
                if (address < 0xa0) ++state_.cp_layout_revision_;
            }
        }
    } else if (opcode == 0x10) {
        const auto address = be16(input.data + 3);
        const auto count = (consumed - 5) / 4;
        if (address >= State::xf_words || count > State::xf_words - address) {
            ok = fail(Error::invalid_xf_range, "GX XF transfer exceeds state memory");
        } else {
            apply_xf(address, input.data + 5, count);
        }
    } else if (opcode == 0x20 || opcode == 0x28 || opcode == 0x30 || opcode == 0x38) {
        const auto word = be32(input.data + 1);
        const auto destination = word & 0xfff;
        const auto count = ((word >> 12) & 15) + 1;
        const auto array = 12 + (opcode - 0x20) / 8;
        const std::uint64_t source = std::uint64_t{state_.cp_[0xa0 + array]} +
                                     std::uint64_t{word >> 16} * (state_.cp_[0xb0 + array] & 0xff);
        std::array<std::uint8_t, 64> bytes{};
        if (destination + count > 0x1000) {
            ok = fail(Error::invalid_xf_range, "GX indexed XF transfer exceeds matrix memory");
        } else if (source > std::numeric_limits<std::uint32_t>::max()) {
            ok = fail(Error::memory_read, "GX indexed XF address overflow");
        } else if (!memory_.copy_indexed_xf(static_cast<std::uint8_t>(array), static_cast<std::uint32_t>(source), bytes.data(),
                                            count * 4)) {
            ok = fail(Error::memory_read, "GX memory range is unavailable");
        } else {
            bool changed = false;
            for (std::size_t i = 0; i < count; ++i) {
                const auto word = be32(bytes.data() + i * 4);
                changed |= state_.xf_[destination + i] != word;
                state_.xf_[destination + i] = word;
            }
            if (changed) {
                ++state_.revision_;
                ++state_.xf_memory_revision_;
                if (destination + count > 0x500) ++state_.xf_post_light_revision_;
                if (destination < 0x600 && destination + count > 0x5f4) ++state_.xf_register_revision_;
                state_.last_xf_memory_write_ = {state_.xf_memory_revision_, static_cast<std::uint16_t>(destination),
                                                static_cast<std::uint16_t>(destination + count)};
            }
        }
    } else if (opcode == 0x61) {
        ok = stream_bp(be32(input.data + 1));
    } else if (opcode == 0x40) {
        ok = execute_list(be32(input.data + 1), be32(input.data + 5), depth + 1, budget);
    } else if (opcode == 0x48) {
        if (!sink_.invalidate_vertices()) {
            ok = fail(Error::sink_rejected, "GX sink rejected vertex-cache invalidation");
        }
    } else if (primitive(opcode)) {
        Draw draw;
        draw.primitive = opcode & 0xf8;
        draw.vat = opcode & 7;
        draw.vertex_count = vertex_count;
        draw.layout = *layout;
        draw.vertices = {input.data + 3, consumed - 3};
        if (!runs_.empty()) {
            draw.runs = runs_.data();
            draw.run_count = static_cast<std::uint16_t>(runs_.size());
        }
        // The n-th vertex (from 1) across all runs.
        const auto vertex_at = [&](std::size_t n) {
            if (draw.runs) {
                for (std::size_t r = 0; r < draw.run_count; ++r) {
                    if (n <= draw.runs[r].vertex_count) return draw.vertices.data + draw.runs[r].offset + (n - 1) * draw.layout.stride;
                    n -= draw.runs[r].vertex_count;
                }
            }
            return draw.vertices.data + (n - 1) * draw.layout.stride;
        };
        if (!sink_.draw(state_, draw, memory_)) {
            ok = fail(Error::sink_rejected, "GX sink rejected draw");
        } else if (status_ && draw.vertex_count) {
            const auto matrix_bytes = static_cast<std::size_t>(std::count(
                draw.layout.matrix_indices.begin(), draw.layout.matrix_indices.end(), true));
            for (std::size_t n = draw.total_vertices(); matrix_bytes && n > 0; --n) {
                const auto* vertex = vertex_at(n);
                const auto mode = draw.layout.position.mode;
                if ((mode == AttributeMode::index8 && vertex[matrix_bytes] == 255) ||
                    (mode == AttributeMode::index16 && be16(vertex + matrix_bytes) == 65535)) {
                    continue;
                }
                bool changed = false;
                for (unsigned m = 0; m < draw.layout.matrix_indices.size(); ++m) {
                    if (draw.layout.matrix_indices[m]) {
                        const auto value = static_cast<std::uint8_t>(*vertex++ & 63);
                        changed |= state_.matrix_indices_[m] != value;
                        state_.matrix_indices_[m] = value;
                    }
                }
                if (changed) { ++state_.revision_; ++state_.matrix_index_revision_; }
                break;
            }
        }
    }
    return ok && status_ ? Parse::complete : Parse::failed;
}

Status Decoder::feed(ByteView bytes) try {
    if (!status_) {
        return status_;
    }
    if (feeding_) {
        fail(Error::reentrant_call, "GX decoder cannot be called from its callbacks");
        return status_;
    }
    if (bytes.size && !bytes.data) {
        fail(Error::invalid_input, "null GX command buffer");
        return status_;
    }
    feeding_ = true;
    Budget budget;
    budget_ = &budget;
    struct Guard { bool& active; Budget*& budget; ~Guard() { active = false; budget = nullptr; } } guard{feeding_, budget_};
    std::size_t offset = 0;
    while (offset < bytes.size || !pending_.empty()) {
        // A buffered command's size is fixed by bytes already received; parse once it can be complete.
        if (!pending_.empty() && pending_required_ > pending_.size() + (bytes.size - offset)) {
            pending_.insert(pending_.end(), bytes.data + offset, bytes.data + bytes.size);
            break;
        }
        // NOP runs, plain BP and CP writes and XF matrix-memory writes are most of a frame's
        // commands; fast_command applies them with the general path's effects.
        if (pending_.empty()) {
            std::size_t fast = 0;
            const auto applied = fast_command(bytes.data + offset, bytes.size - offset, 0, budget, fast);
            if (applied == Fast::failed) break;
            if (applied == Fast::applied) { offset += fast; continue; }
        }
        std::size_t consumed = 5;
        const bool buffered = !pending_.empty();
        const auto input = buffered ? ByteView{pending_.data(), pending_.size()}
                                    : ByteView{bytes.data + offset, bytes.size - offset};
        Parse result;
        if (!bp_command(input.data, input.size, 0, budget, result)) result = command(input, consumed, 0, budget);
        if (result == Parse::failed) {
            break;
        }
        if (result == Parse::complete) {
            if (buffered) {
                pending_.clear();
            } else {
                offset += consumed;
            }
        } else if (buffered) {
            pending_required_ = consumed;
            const auto count = std::min(consumed - pending_.size(), bytes.size - offset);
            if (!count) {
                break;
            }
            pending_.insert(pending_.end(), bytes.data + offset, bytes.data + offset + count);
            offset += count;
        } else {
            pending_required_ = consumed;
            pending_.assign(bytes.data + offset, bytes.data + bytes.size);
            break;
        }
    }
    return status_;
} catch (...) {
    latch_exception();
    throw;
}

Status Decoder::inject(ByteView bytes) try {
    if (!status_) return status_;
    if (feeding_ ? !host_callback_ : !pending_.empty()) {
        fail(Error::reentrant_call, feeding_ ? "GX commands can be injected only from host_command and filter_bp"
                                             : "GX commands cannot be injected inside a partial command");
        return status_;
    }
    if (bytes.size && !bytes.data) {
        fail(Error::invalid_input, "null GX command buffer");
        return status_;
    }
    Budget local;
    auto& budget = budget_ ? *budget_ : local;
    struct Guard {
        bool& feeding; bool& injecting; bool& callback; bool was_feeding, was_injecting, was_callback;
        ~Guard() { feeding = was_feeding; injecting = was_injecting; callback = was_callback; }
    } guard{feeding_, injecting_, host_callback_, feeding_, injecting_, host_callback_};
    feeding_ = injecting_ = true;
    host_callback_ = false;
    std::size_t offset = 0;
    while (offset < bytes.size) {
        std::size_t consumed = 5;
        Parse result;
        if (!bp_command(bytes.data + offset, bytes.size - offset, 0, budget, result))
            result = command({bytes.data + offset, bytes.size - offset}, consumed, 0, budget);
        if (result == Parse::failed) break;
        if (result == Parse::incomplete) {
            fail(Error::truncated_command, "injected GX command is incomplete");
            break;
        }
        offset += consumed;
    }
    return status_;
} catch (...) {
    latch_exception();
    throw;
}

bool Decoder::extend_pending(ByteView bytes) {
    if (!status_ || feeding_ || pending_.empty() || !bytes.data || pending_required_ <= pending_.size() + bytes.size) return false;
    pending_.insert(pending_.end(), bytes.data, bytes.data + bytes.size);
    return true;
}

Status Decoder::finish() try {
    if (feeding_) {
        fail(Error::reentrant_call, "GX decoder cannot be finished from its callbacks");
    } else if (!pending_.empty()) {
        fail(Error::truncated_command, "GX FIFO ends inside a command");
    }
    return status_;
} catch (...) {
    latch_exception();
    throw;
}

Status Decoder::call_display_list(ByteView bytes, bool owned) try {
    if (!status_) return status_;
    if (feeding_) {
        fail(Error::reentrant_call, "GX decoder cannot be called from its callbacks");
        return status_;
    }
    if (!pending_.empty()) {
        fail(Error::truncated_command, "GX display-list call is inside an unfinished command");
        return status_;
    }
    if (bytes.size && !bytes.data) {
        fail(Error::invalid_input, "null GX display list");
        return status_;
    }
    if (limits_.max_command_bytes < 9) {
        fail(Error::command_size, "GX command size limit exceeded");
        return status_;
    }
    if (limits_.max_expanded_bytes < 9) {
        fail(Error::work_limit, "GX command work limit exceeded");
        return status_;
    }
    feeding_ = true;
    struct Guard { bool& active; ~Guard() { active = false; } } guard{feeding_};
    Budget budget{1, 9};
    if (charge_list(bytes.size, 1, budget)) {
        if (owned) {
            parse_list(bytes, 1, budget);
        } else {
            std::vector<std::uint8_t> snapshot(bytes.size);
            if (bytes.size) std::memcpy(snapshot.data(), bytes.data, bytes.size);
            parse_list({snapshot.data(), snapshot.size()}, 1, budget);
        }
    }
    return status_;
} catch (...) {
    latch_exception();
    throw;
}

Status Decoder::write_u8(std::uint8_t value) { return feed({&value, 1}); }
Status Decoder::write_u16(std::uint16_t value) {
    const std::uint8_t bytes[] = {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return feed({bytes, sizeof(bytes)});
}
Status Decoder::write_u32(std::uint32_t value) {
    const std::uint8_t bytes[] = {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
                                static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return feed({bytes, sizeof(bytes)});
}
Status Decoder::write_f32(float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);
    std::uint32_t word;
    std::memcpy(&word, &value, sizeof(word));
    return write_u32(word);
}

} // namespace starlet::gx

#include "format.hpp"

namespace starlet::gx {

void TraceAccess::save(const Decoder& decoder, trace::Writer& w) {
    const State& s = decoder.state_;
    w.i32(s.efb_.height);
    w.raw(s.bp_.data(), sizeof s.bp_);
    w.raw(s.cp_.data(), sizeof s.cp_);
    w.raw(s.tev_registers_.data(), sizeof s.tev_registers_);
    w.raw(s.xf_.data(), sizeof s.xf_);
    std::uint32_t blocks = 0;
    for (const auto& b : s.palette_) blocks += b != nullptr;
    w.u32(blocks);
    for (std::size_t i = 0; i < s.palette_.size(); ++i) {
        if (!s.palette_[i]) continue;
        w.u32(std::uint32_t(i));
        w.raw(s.palette_[i]->bytes.data(), s.palette_[i]->bytes.size());
        w.u64(s.palette_[i]->generation);
        w.u16(s.palette_[i]->valid_chunks);
    }
    w.raw(s.matrix_indices_.data(), sizeof s.matrix_indices_);
    w.u32(s.bp_mask_);
    for (auto v : {s.palette_generation_, s.revision_, s.bp_revision_, s.bp_state_revision_, s.cp_revision_, s.xf_memory_revision_,
                   s.xf_register_revision_, s.cp_layout_revision_, s.matrix_index_revision_, s.xf_post_light_revision_})
        w.u64(v);
    w.u64(s.last_xf_memory_write_.revision);
    w.u16(s.last_xf_memory_write_.first);
    w.u16(s.last_xf_memory_write_.end);
    for (const auto& e : s.texture_extents_) { w.u16(e.width); w.u16(e.height); }
    w.bytes(decoder.pending_.data(), decoder.pending_.size());
    w.u64(decoder.pending_required_);
}

void TraceAccess::load(Decoder& decoder, trace::Reader& r) {
    State& s = decoder.state_;
    if (r.i32() != s.efb_.height) throw std::runtime_error("trace: decoder state for another EFB layout");
    r.raw(s.bp_.data(), sizeof s.bp_);
    r.raw(s.cp_.data(), sizeof s.cp_);
    r.raw(s.tev_registers_.data(), sizeof s.tev_registers_);
    r.raw(s.xf_.data(), sizeof s.xf_);
    for (auto& b : s.palette_) b.reset();
    for (auto blocks = r.u32(); blocks--;) {
        const auto index = r.u32();
        if (index >= s.palette_.size()) throw std::runtime_error("trace: palette block out of range");
        auto block = std::make_shared<PaletteBlock>();
        r.raw(block->bytes.data(), block->bytes.size());
        block->generation = r.u64();
        block->valid_chunks = r.u16();
        s.palette_[index] = std::move(block);
    }
    r.raw(s.matrix_indices_.data(), sizeof s.matrix_indices_);
    s.bp_mask_ = r.u32();
    for (auto* v : {&s.palette_generation_, &s.revision_, &s.bp_revision_, &s.bp_state_revision_, &s.cp_revision_, &s.xf_memory_revision_,
                    &s.xf_register_revision_, &s.cp_layout_revision_, &s.matrix_index_revision_, &s.xf_post_light_revision_})
        *v = r.u64();
    s.last_xf_memory_write_.revision = r.u64();
    s.last_xf_memory_write_.first = r.u16();
    s.last_xf_memory_write_.end = r.u16();
    for (auto& e : s.texture_extents_) { e.width = r.u16(); e.height = r.u16(); }
    s.bp_dirty_ = {};
    s.xf_dirty_ = {};
    s.rebuild_registers();
    decoder.pending_ = r.bytes();
    decoder.pending_required_ = r.u64();
    for (auto& layout : decoder.layouts_) layout.revision = ~std::uint64_t{0};
}

} // namespace starlet::gx

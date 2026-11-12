#include "starlet/gx_fifo/gles_renderer.hpp"

#include <cstdio>

namespace starlet::gx_fifo {

void GlesBackend::draw(const gx::State& state, const gx::Draw& draw, gx::MemoryReader& memory) {
    renderer_.draw_direct(state, draw, memory, cmpr_);
}

void GlesBackend::wait_until_idle() { renderer_.wait_until_idle(); }

void GlesBackend::copy_display(const gx::DisplayCopy& copy, std::uint8_t* destination, std::size_t bytes) {
    renderer_.copy_display(copy, destination, bytes);
}

void GlesBackend::copy_texture(const gx::TextureCopy& copy, std::uint8_t* destination, std::size_t bytes, std::uint32_t address) {
    renderer_.copy_texture_resident(copy, destination, bytes, address);
}

std::uint32_t GlesBackend::peek_depth(int x, int y) { return renderer_.peek_depth(x, y); }
std::uint32_t GlesBackend::peek_color(int x, int y) { return renderer_.peek_color(x, y); }

bool GlesBackend::present(const gx::FramebufferView& frame, int drawable_width, int drawable_height, bool black) {
    return renderer_.present_framebuffer(frame, drawable_width, drawable_height, black);
}

std::vector<std::uint8_t> GlesBackend::readback() { return renderer_.readback(); }

void GlesBackend::print_stats(const GpuCounters& gpu) {
    const auto s = renderer_.stats();
    std::printf("gpu: %u retraces, presented=%d xfb=%p known=%d, draws %llu, display copies %llu, texture copies %llu, "
                "presentations %llu, programs %llu, decoded %llu bytes; direct draws %llu zero-area %llu culled %llu unready %llu "
                "submissions %llu batched %llu instanced %llu skipped %u; program waits %llu/%llu/%llu/%llu/%llu "
                "(33/67/133/267 ms/longer) max %llu ms, GL-thread links %llu max %llu ms; resident copies %llu hits %llu "
                "crops %llu materialized %llu\n",
                gpu.retraces, gpu.presented, gpu.frame_buffer, gpu.known_frame_buffer, (unsigned long long)s.draws,
                (unsigned long long)s.display_copies, (unsigned long long)s.texture_copies,
                (unsigned long long)s.presentations, (unsigned long long)s.shader_compiles,
                (unsigned long long)gpu.decoded_bytes, (unsigned long long)s.direct.draws,
                (unsigned long long)s.direct.zero_area, (unsigned long long)s.direct.culled_draws,
                (unsigned long long)s.direct.unready_draws, (unsigned long long)s.direct.submissions,
                (unsigned long long)s.direct.batched_draws, (unsigned long long)s.direct.instanced_draws, gpu.skipped_draws,
                (unsigned long long)s.direct.program_waits[0], (unsigned long long)s.direct.program_waits[1],
                (unsigned long long)s.direct.program_waits[2], (unsigned long long)s.direct.program_waits[3],
                (unsigned long long)s.direct.program_waits[4], (unsigned long long)s.direct.program_wait_max_ms,
                (unsigned long long)s.direct.fallback_links, (unsigned long long)s.direct.fallback_link_max_ms,
                (unsigned long long)s.direct.resident_copies, (unsigned long long)s.direct.resident_hits,
                (unsigned long long)s.direct.resident_crops, (unsigned long long)s.direct.materialized);
}

}

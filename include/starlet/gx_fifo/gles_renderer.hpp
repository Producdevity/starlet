#pragma once

#include "starlet/gx_fifo/renderer.hpp"

#include "starlet/gx/gles/renderer.hpp"

namespace starlet::gx_fifo {

// Starlet's OpenGL ES renderer behind the GPU thread, on its direct path (draw_direct, resident
// texture copies). The port links starlet::gx_gles.
class GlesBackend final : public Renderer {
public:
    explicit GlesBackend(gx::GlesRenderer& renderer, gx::TextureCmprModel cmpr = gx::TextureCmprModel::gx_candidate_v1)
        : renderer_(renderer), cmpr_(cmpr) {}

    void draw(const gx::State& state, const gx::Draw& draw, gx::MemoryReader& memory) override;
    void wait_until_idle() override;
    void copy_display(const gx::DisplayCopy& copy, std::uint8_t* destination, std::size_t bytes) override;
    void copy_texture(const gx::TextureCopy& copy, std::uint8_t* destination, std::size_t bytes,
                      std::uint32_t address) override;
    std::uint32_t peek_depth(int x, int y) override;
    std::uint32_t peek_color(int x, int y) override;
    bool present(const gx::FramebufferView& frame, int drawable_width, int drawable_height, bool black) override;
    std::vector<std::uint8_t> readback() override;
    void print_stats(const GpuCounters& gpu) override;

private:
    gx::GlesRenderer& renderer_;
    gx::TextureCmprModel cmpr_;
};

}

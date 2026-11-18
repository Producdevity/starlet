#include "shaders.hpp"

#include "starlet/gx/decoder.hpp"
#include "starlet/gx/gles/program_builder.hpp"
#include "starlet/gx/gles/program_cache.hpp"
#include "starlet/gx/gles/renderer.hpp"

#include <GLES3/gl31.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <thread>
#include <tuple>

using namespace starlet::gx;

namespace {

struct NoMemory final : MemoryReader {
    bool copy(std::uint32_t, std::uint8_t*, std::size_t) override { return false; }
};
struct NoSink final : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return true; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};

// GX state whose pixel format (BP 0x43) is RGBA6_Z24 or RGB8_Z24.
State pixel_state(EfbLayout efb, bool rgba6) {
    NoMemory memory;
    NoSink sink;
    Decoder decoder(memory, sink, {}, efb);
    const std::uint8_t write[] = {0x61, 0x43, 0, 0, std::uint8_t(rgba6 ? 1 : 0)};
    decoder.feed({write, sizeof write});
    return decoder.state();
}

class Check {
public:
    Check() : memory_(static_cast<std::uint8_t*>(std::aligned_alloc(64, size_))) { std::memset(memory_.get(), 0, size_); }

    int run(const std::vector<std::string>& lists) {
        for (auto shading : {DirectShading::exact, DirectShading::fast}) {
            shading_ = shading;
            fresh();
            copies();
            efb_programs();
            renderer_.reset();
        }
        std::printf("shaders: %zu texture-copy define sets built from %llu copies (%llu refused before GL work, %llu unsupported), %llu EFB steps\n",
                    defines_.size(), (unsigned long long)copies_, (unsigned long long)refused_, (unsigned long long)unsupported_,
                    (unsigned long long)efb_steps_);
        for (const auto& list : lists) program_list(list);
        for (const auto& f : failures_) std::printf("FAILED %s\n", f.c_str());
        return failures_.empty() ? 0 : 1;
    }

private:
    struct Free { void operator()(std::uint8_t* p) const { std::free(p); } };
    static constexpr std::size_t size_ = 8 << 20;
    static constexpr std::uint32_t token_ = 0x00100000;

    const char* shading_name() const { return shading_ == DirectShading::fast ? "fast" : "exact"; }
    void fresh() {
        GlesLimits limits;
        limits.efb = efb_;
        limits.direct.shading = shading_;
        limits.direct.background_programs = false;
        renderer_ = std::make_unique<GlesRenderer>(limits);
        renderer_->program_builder(); // Starts the direct path.
    }
    // Runs one step; a step that fails after GL work began (the renderer then reports failed())
    // is a shader failure, and the renderer is rebuilt for the next step.
    bool step(const std::string& name, const std::function<void()>& f) {
        try {
            f();
            renderer_->wait_until_idle();
            if (!renderer_->failed()) return true;
            failures_.push_back(std::string(shading_name()) + " " + name + ": renderer failed");
        } catch (const std::exception& e) {
            if (!renderer_->failed()) { ++refused_; return false; }
            // Combinations the renderer does not support end it too; only GLSL errors count here.
            const std::string what = e.what();
            if (what.rfind("GLES shader", 0) == 0 || what.rfind("GLES link", 0) == 0)
                failures_.push_back(std::string(shading_name()) + " " + name + ": " + what);
            else ++unsupported_;
        }
        renderer_.reset();
        fresh();
        return false;
    }

    void copies() {
        const int top = efb_.height;
        for (int depth = 0; depth < 2; ++depth)
            for (unsigned format = 0; format <= 12; ++format)
                for (int luminance = 0; luminance < 2; ++luminance)
                    for (int half = 0; half < 2; ++half)
                        for (int rgba6 = 0; rgba6 < 2; ++rgba6)
                            for (int filter = 0; filter < 2; ++filter)
                                for (int region = 0; region < 3; ++region) {
                                    TextureCopy p;
                                    p.efb = efb_;
                                    p.format = format;
                                    p.depth = depth;
                                    p.luminance = luminance;
                                    p.half_scale = half;
                                    p.rgba6 = rgba6;
                                    p.filter = filter ? std::array<int, 3>{{8, 48, 8}} : std::array<int, 3>{{0, 64, 0}};
                                    p.width = 64;
                                    p.height = region == 2 ? 32 : 64;
                                    p.y = region == 0 ? 16 : region == 1 ? top - 32 : top;
                                    p.x = 16;
                                    p.split = region == 1;
                                    p.strip = region == 2;
                                    p.stride = 2048;
                                    std::size_t bytes;
                                    try { bytes = texture_copy_bytes(p); } catch (const std::invalid_argument&) { continue; }
                                    ++copies_;
                                    char name[160];
                                    std::snprintf(name, sizeof name, "texture copy FORMAT=%u HALF=%d RGBA6=%d LUMINANCE=%d DEPTH=%d FILTER=%d SPLIT=%d STRIP=%d",
                                                  format, half, rgba6, luminance, depth, filter, int(p.split), int(p.strip));
                                    const bool ok = step(name, [&] {
                                        if (rgba6 != rgba6_) clear(rgba6, true);
                                        renderer_->copy_texture_resident(p, memory_.get(), size_, token_);
                                        renderer_->before_dma_read(memory_.get(), bytes);
                                    });
                                    if (ok) defines_.insert(std::make_tuple(format, half, rgba6, luminance, depth, filter, int(p.split)));
                                }
    }
    // A whole-EFB clear in the given pixel format: a format change reinterprets the EFB.
    void clear(bool rgba6, bool constant) {
        const auto state = pixel_state(efb_, rgba6);
        auto plan = plan_efb_clear(state, 0, 0, EfbLayout::width, efb_.height, {40, 80, 120, 255});
        if (!constant) plan.tile[1] = {200, 10, 10, 255};
        renderer_->clear_efb(plan);
        rgba6_ = rgba6;
    }
    void efb_step(const std::string& name, const std::function<void()>& f) {
        ++efb_steps_;
        step(name, f);
    }
    void efb_programs() {
        rgba6_ = false;
        for (int rgba6 = 0; rgba6 < 2; ++rgba6)
            for (int constant = 0; constant < 2; ++constant)
                efb_step("EFB clear rgba6=" + std::to_string(rgba6) + " constant=" + std::to_string(constant), [&] {
                    clear(rgba6, constant);
                    renderer_->peek_color(10, 10);
                });
        efb_step("partial and strip clear", [&] {
            const auto state = pixel_state(efb_, false);
            renderer_->clear_efb(plan_efb_clear(state, 32, 32, 64, 64, {1, 2, 3, 4}));
            renderer_->clear_efb(plan_efb_clear(state, 0, efb_.height - 8, 64, 32, {5, 6, 7, 8}));
            renderer_->peek_depth(40, 40);
        });
        const std::array<std::array<unsigned, 3>, 3> filters = {{{0, 64, 0}, {21, 22, 21}, {8, 40, 16}}};
        for (int rgba6 = 0; rgba6 < 2; ++rgba6)
            for (const auto& f : filters)
                for (int full = 0; full < 2; ++full) {
                    const std::string name = "display copy rgba6=" + std::to_string(rgba6) + " filter=" + std::to_string(f[0]) + "," +
                                             std::to_string(f[1]) + "," + std::to_string(f[2]) + " full=" + std::to_string(full);
                    efb_step(name, [&] {
                        if (rgba6 != rgba6_) clear(rgba6, true);
                        DisplayCopy d;
                        d.efb = efb_;
                        d.width = EfbLayout::width;
                        d.height = full ? efb_.height : 64;
                        d.stride = EfbLayout::width * 2;
                        d.filter = f;
                        d.rgba6 = rgba6;
                        renderer_->copy_display(d, memory_.get(), size_);
                        FramebufferView view;
                        view.bytes = {memory_.get(), d.stride * std::size_t(d.height)};
                        view.width = EfbLayout::width;
                        view.height = unsigned(d.height);
                        view.stride = d.stride;
                        renderer_->present_framebuffer(view, 640, 480);
                    });
                }
        efb_step("EFB presentation", [&] { renderer_->present(640, 480); });
        efb_step("bloom", [&] {
            renderer_->begin_bloom();
            EfbBloom b;
            b.level1_weight = 40;
            b.level2_weight = 30;
            b.intensity = 100;
            renderer_->end_bloom(b);
        });
        efb_step("snapshots and depth readback", [&] {
            const int exact = renderer_->snapshot_efb_exact({0, 0, 64, 64});
            const int scaled = renderer_->snapshot_efb(EfbRegion{});
            std::uint32_t value;
            for (int index : {exact, scaled}) {
                if (index < 0) continue;
                renderer_->wait_snapshot(index);
                renderer_->peek_color_snapshot(index, 4, 4, value);
                renderer_->peek_depth_snapshot(index, 4, 4, value);
                renderer_->release_snapshot(index);
            }
            const int depth = renderer_->request_depth_readback();
            if (depth >= 0) {
                renderer_->wait_depth_readback(depth);
                renderer_->depth_readback_value(depth, 4, 4, value);
                renderer_->release_depth_readback(depth);
            }
        });
        efb_step("offscreen pass", [&] {
            renderer_->begin_offscreen();
            clear(rgba6_, true);
            renderer_->end_offscreen();
        });
        for (unsigned format : {0u, 1u, 3u, 4u, 5u, 6u, 8u}) {
            efb_step("write-back format " + std::to_string(format), [&] {
                TextureCopy p;
                p.efb = efb_;
                p.format = format;
                p.luminance = format <= 1;
                p.width = 64;
                p.height = 64;
                p.stride = 2048;
                const auto bytes = texture_copy_bytes(p);
                renderer_->write_back(token_ + 0x200000, bytes, true);
                renderer_->copy_texture_resident(p, memory_.get() + 0x200000, size_ - 0x200000, token_ + 0x200000);
                renderer_->land_write_backs(renderer_->write_backs_issued(), true);
                renderer_->write_back(token_ + 0x200000, bytes, false);
            });
        }
    }

    // Builds every program of a program list in a fresh cache through the renderer's own builder.
    void program_list(const std::string& list) {
        char directory[] = "/tmp/starlet-shaders-XXXXXX";
        if (!mkdtemp(directory)) { failures_.push_back("cannot make a cache directory"); return; }
        FramebufferFetch fetch = FramebufferFetch::none;
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; ++i) {
            const auto* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
            if (name && !std::strcmp(name, "GL_EXT_shader_framebuffer_fetch")) fetch = FramebufferFetch::ext;
        }
        GlesProgramBuilderStats s;
        {
            GlesProgramCache cache(directory);
            GlesProgramBuilder builder(cache, {list}, fetch, 4);
            if (!builder.running()) { failures_.push_back("program list " + list + ": the builder cannot start"); return; }
            const auto start = std::chrono::steady_clock::now();
            for (;;) {
                s = builder.stats();
                if (s.lists_done && s.finished >= s.to_build) break;
                if (std::chrono::steady_clock::now() - start > std::chrono::minutes(10)) { failures_.push_back("program list " + list + ": timeout"); break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::printf("shaders: %s: %zu listed, %zu built, %zu failed, %zu rejected entries\n", list.c_str(), s.listed, s.built, s.failed, s.rejected);
        if (s.failed) failures_.push_back("program list " + list + ": " + std::to_string(s.failed) + " programs failed to build");
    }

    EfbLayout efb_;
    DirectShading shading_ = DirectShading::exact;
    std::unique_ptr<GlesRenderer> renderer_;
    std::unique_ptr<std::uint8_t, Free> memory_;
    bool rgba6_ = false;
    std::set<std::tuple<unsigned, int, int, int, int, int, int>> defines_;
    std::uint64_t copies_ = 0, refused_ = 0, unsupported_ = 0, efb_steps_ = 0;
    std::vector<std::string> failures_;
};

} // namespace

int check_shaders(const std::vector<std::string>& lists) {
    try {
        Check check;
        return check.run(lists);
    } catch (const std::exception& e) {
        std::printf("FAILED shader check: %s\n", e.what());
        return 1;
    }
}

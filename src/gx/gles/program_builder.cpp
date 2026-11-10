#include "starlet/gx/gles/program_builder.hpp"
#include "starlet/gx/direct.hpp"
#include "starlet/gx/gles/program_list.hpp"

#include <GLES3/gl32.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <algorithm>
#include <set>
#include <type_traits>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace starlet::gx {
namespace {
// EGL 1.5 names and values; libEGL is the one SDL already loaded, found at run time.
using EGLDisplay = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLConfig = void*;
using EGLint = std::int32_t;
using EGLBoolean = unsigned;
constexpr EGLint egl_config_id = 0x3028, egl_surface_type = 0x3033, egl_pbuffer_bit = 0x0001, egl_renderable_type = 0x3040,
                 egl_opengl_es3_bit = 0x0040, egl_none = 0x3038, egl_extensions = 0x3055, egl_height = 0x3056, egl_width = 0x3057,
                 egl_context_major_version = 0x3098, egl_context_minor_version = 0x30fb;
} // namespace

struct GlesProgramBuilder::Context {
    EGLDisplay (*current_display)() = nullptr;
    EGLContext (*current_context)() = nullptr;
    EGLBoolean (*query_context)(EGLDisplay, EGLContext, EGLint, EGLint*) = nullptr;
    EGLBoolean (*choose_config)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLSurface (*create_pbuffer)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
    EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    EGLBoolean (*destroy_context)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (*destroy_surface)(EGLDisplay, EGLSurface) = nullptr;
    const char* (*query_string)(EGLDisplay, EGLint) = nullptr;
    EGLDisplay display = nullptr;
    EGLContext context = nullptr;
    EGLSurface surface = nullptr;

    // A context sharing objects with the current one: surfaceless where offered, else on a small pbuffer.
    bool create() {
        void* library = nullptr;
        for (const char* name : {"libEGL.so.1", "libEGL.so"})
            if ((library = dlopen(name, RTLD_NOW | RTLD_NOLOAD)) != nullptr) break;
        if (!library) return false;
        auto load = [library](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
            return function != nullptr;
        };
        if (!load(current_display, "eglGetCurrentDisplay") || !load(current_context, "eglGetCurrentContext") ||
            !load(query_context, "eglQueryContext") || !load(choose_config, "eglChooseConfig") ||
            !load(create_context, "eglCreateContext") || !load(create_pbuffer, "eglCreatePbufferSurface") ||
            !load(make_current, "eglMakeCurrent") || !load(destroy_context, "eglDestroyContext") ||
            !load(destroy_surface, "eglDestroySurface") || !load(query_string, "eglQueryString"))
            return false;
        display = current_display();
        const auto shared = current_context();
        if (!display || !shared) return false;
        const char* extensions = query_string(display, egl_extensions);
        const bool surfaceless = extensions && std::strstr(extensions, "EGL_KHR_surfaceless_context");
        EGLint id = 0, count = 0;
        EGLConfig config = nullptr;
        query_context(display, shared, egl_config_id, &id);
        const EGLint same[] = {egl_config_id, id, egl_none};
        const EGLint pbuffer[] = {egl_surface_type, egl_pbuffer_bit, egl_renderable_type, egl_opengl_es3_bit, egl_none};
        if (!choose_config(display, surfaceless ? same : pbuffer, &config, 1, &count) || count != 1) return false;
        const EGLint version[] = {egl_context_major_version, 3, egl_context_minor_version, 1, egl_none};
        context = create_context(display, config, shared, version);
        if (!context) return false;
        if (!surfaceless) {
            const EGLint size[] = {egl_width, 16, egl_height, 16, egl_none};
            if (!(surface = create_pbuffer(display, config, size))) return false;
        }
        return true;
    }
    ~Context() {
        if (surface) destroy_surface(display, surface);
        if (context) destroy_context(display, context);
    }
};

namespace {
std::uint64_t source_key(const std::string& vertex, const std::string& fragment) {
    std::uint64_t h = 14695981039346656037ull;
    for (const auto* text : {&vertex, &fragment})
        for (unsigned char ch : *text) { h ^= ch; h *= 1099511628211ull; }
    return h ^ (vertex.size() << 32);
}
void lower_priority() {
#ifdef __linux__
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}
} // namespace

GlesProgramBuilder::GlesProgramBuilder(GlesProgramCache& cache, std::vector<std::string> lists, FramebufferFetch fetch, unsigned workers,
                                       GlesProgramBuilderPolicy policy)
    : cache_(cache), lists_(std::move(lists)), fetch_(fetch), policy_(policy), start_(std::chrono::steady_clock::now()) {
    if (!cache_.enabled()) return;
    for (unsigned i = 0; i < std::max(workers, 1u); ++i) {
        auto context = std::make_unique<Context>();
        if (!context->create()) break;
        contexts_.push_back(std::move(context));
    }
    for (unsigned i = 0; i < contexts_.size(); ++i) threads_.emplace_back([this, i] { work(i); });
}

GlesProgramBuilder::~GlesProgramBuilder() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    for (auto& thread : threads_) thread.join();
}

void GlesProgramBuilder::background() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        background_ = true;
    }
    wake_.notify_all();
}

void GlesProgramBuilder::request(const std::string& vertex, const std::string& fragment) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || !building_.insert(source_key(vertex, fragment)).second) return;
        requests_.emplace_back(vertex, fragment);
    }
    wake_.notify_all();
}

bool GlesProgramBuilder::building(const std::string& vertex, const std::string& fragment) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return building_.count(source_key(vertex, fragment)) != 0;
}

GlesProgramBuilderStats GlesProgramBuilder::stats() const {
    GlesProgramBuilderStats s;
    s.running = running();
    s.planned = planned_;
    s.to_build = s.planned ? todo_.size() : 0;
    s.finished = s.planned ? std::min<std::size_t>(finished_, todo_.size()) : 0;
    s.lists_done = s.planned && s.finished == s.to_build;
    s.listed = listed_; s.built = built_; s.rejected = rejected_; s.failed = failed_; s.requested = requested_;
    return s;
}

void GlesProgramBuilder::build(const std::string& vertex, const std::string& fragment, std::atomic<std::size_t>& count) {
    if (cache_.contains(vertex, fragment)) return;
    try {
        glDeleteProgram(link_program(vertex, fragment, cache_));
        ++count;
    } catch (const std::exception& e) {
        if (++failed_ <= 4) std::fprintf(stderr, "Shader build failed: %s\n", e.what());
    }
}

// Builds one requested program; false when none is waiting.
bool GlesProgramBuilder::serve_request() {
    std::pair<std::string, std::string> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requests_.empty()) return false;
        job = take_request(requests_, policy_.newest_first);
    }
    build(job.first, job.second, requested_);
    std::lock_guard<std::mutex> lock(mutex_);
    building_.erase(source_key(job.first, job.second));
    return true;
}

// Worker 0: the listed programs whose sources the cache lacks, planned again when built.
void GlesProgramBuilder::plan() {
    std::set<std::vector<std::uint32_t>> seen;
    std::unordered_set<std::uint64_t> sources;
    for (const auto& list : lists_) {
        for (auto& words : read_program_list(list)) {
            if (stop_) return;
            if (!seen.insert(words).second) continue;
            ++listed_;
            const bool batched = words.size() == 102 && words.back() == batched_program_word;
            const bool instanced = words.size() == 102 && words.back() == instanced_program_word;
            if (batched || instanced) words.pop_back();
            // The device's own fetch mode keys the programs its draws will ask for.
            words[0] = (words[0] & ~0xf0000u) | (static_cast<std::uint32_t>(fetch_) << 16);
            try {
                State state;
                VertexLayout layout;
                std::uint8_t latched = 0;
                DirectProgramResult planned;
                // Batched variants plan without the latch, as the renderer plans them.
                if (!direct_program_state(words, state, layout, &latched) ||
                    !(planned = plan_direct_program(state, layout, fetch_, DirectShading::fast, batched, batched ? 0 : latched,
                                                    instanced))) {
                    ++rejected_;
                    continue;
                }
                const auto& p = *planned.program;
                if (sources.insert(source_key(p.vertex, p.fragment)).second && !cache_.contains(p.vertex, p.fragment))
                    todo_.push_back({std::move(words), batched, instanced});
            } catch (const std::exception&) {
                ++rejected_;
            }
        }
    }
}

void GlesProgramBuilder::work(unsigned index) {
    auto& c = *contexts_[index];
    const bool current = c.make_current(c.display, c.surface, c.surface, c.context);
    if (index == 0) {
        if (current) plan();
        if (current && todo_.empty() && !lists_.empty()) {
            std::printf("Shader prebuild: %zu listed, all cached\n", listed_.load());
            std::fflush(stdout);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            planned_ = true;
            // Without a current context nothing completes; the renderer links requests itself.
            if (!current) { stop_ = true; building_.clear(); }
        }
        wake_.notify_all();
    } else {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return planned_ || stop_; });
    }
    bool lowered = false;
    while (current && !stop_) {
        if (background_ && !lowered) {
            if (index >= std::max(policy_.request_workers, 1u)) break;
            if (!policy_.normal_priority) lower_priority();
            lowered = true;
        }
        if (serve_request()) continue;
        const auto at = next_++;
        if (at >= todo_.size()) break;
        try {
            State state;
            VertexLayout layout;
            std::uint8_t latched = 0;
            DirectProgramResult planned;
            if (direct_program_state(todo_[at].words, state, layout, &latched) &&
                (planned = plan_direct_program(state, layout, fetch_, DirectShading::fast, todo_[at].batched,
                                               todo_[at].batched ? 0 : latched, todo_[at].instanced)))
                build(planned.program->vertex, planned.program->fragment, built_);
        } catch (const std::exception&) {
            ++failed_;
        }
        if (++finished_ == todo_.size() && !lists_.empty()) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
            std::printf("Shader prebuild: %zu listed, %zu built, %zu unusable, %zu failed, %.0f s\n", listed_.load(), built_.load(),
                        rejected_.load(), failed_.load(), seconds);
            std::fflush(stdout);
        }
    }
    // Request workers stay for the renderer's requests.
    if (index < std::max(policy_.request_workers, 1u) && current) {
        if (!lowered && !policy_.normal_priority) lower_priority();
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !requests_.empty(); });
                if (stop_) break;
            }
            while (!stop_ && serve_request()) {}
        }
        if (index == 0 && requested_) {
            std::printf("Shaders built during play: %zu\n", requested_.load());
            std::fflush(stdout);
        }
    }
    if (current) c.make_current(c.display, nullptr, nullptr, nullptr);
}

} // namespace starlet::gx

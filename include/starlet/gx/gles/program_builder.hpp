#pragma once

#include "starlet/gx/gles/program_cache.hpp"
#include "starlet/gx/draw.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace starlet::gx {

struct GlesProgramBuilderStats {
    bool running = false;
    bool planned = false; // The lists are read; to_build counts their programs missing from the cache.
    bool lists_done = false;
    std::size_t listed = 0, to_build = 0, finished = 0, built = 0, rejected = 0, failed = 0, requested = 0;
};

// Builds programs into a program cache on threads with their own EGL contexts sharing
// the current one, so draws load binaries instead of compiling: first the fast programs
// of program lists (see ProgramList) missing from the cache, then whatever the renderer
// requests. Until background(), every worker builds at normal priority, for a start-up
// phase in which nothing else runs: beside a running game, builds stall its GL thread
// on Mali drivers. After it, one low-priority thread continues. Construct with the
// renderer's context current; without EGL or a shareable context, running() is false.
class GlesProgramBuilder {
public:
    GlesProgramBuilder(GlesProgramCache&, std::vector<std::string> lists, FramebufferFetch, unsigned workers = 2);
    // Stops after the programs in progress.
    ~GlesProgramBuilder();
    GlesProgramBuilder(const GlesProgramBuilder&) = delete;
    GlesProgramBuilder& operator=(const GlesProgramBuilder&) = delete;
    bool running() const { return !threads_.empty() && !stop_; }
    void background();
    void request(const std::string& vertex, const std::string& fragment);
    // Requested and neither stored nor failed yet.
    bool building(const std::string& vertex, const std::string& fragment) const;
    GlesProgramBuilderStats stats() const;

private:
    struct Context;
    struct Entry { std::vector<std::uint32_t> words; bool batched = false, instanced = false; };
    void work(unsigned index);
    void plan();
    bool serve_request();
    void build(const std::string& vertex, const std::string& fragment, std::atomic<std::size_t>& count);
    GlesProgramCache& cache_;
    std::vector<std::string> lists_;
    FramebufferFetch fetch_;
    std::vector<std::unique_ptr<Context>> contexts_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<std::string, std::string>> requests_;
    std::unordered_set<std::uint64_t> building_;
    std::vector<Entry> todo_; // Written before planned_, then read only.
    std::atomic<std::size_t> next_{0}, finished_{0};
    std::atomic<bool> stop_{false}, planned_{false}, background_{false};
    std::atomic<std::size_t> listed_{0}, built_{0}, rejected_{0}, failed_{0}, requested_{0};
    std::chrono::steady_clock::time_point start_;
    std::vector<std::thread> threads_;
};

} // namespace starlet::gx

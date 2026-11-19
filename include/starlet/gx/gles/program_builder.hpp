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

// How requested programs are built once background() ends the start-up phase.
struct GlesProgramBuilderPolicy {
    // Workers that keep serving the renderer's requests (at most the start-up workers):
    // more build at once, each beside the running game.
    unsigned request_workers = 1;
    // Request workers keep normal priority instead of dropping to nice 10.
    bool normal_priority = false;
    // The newest request first: what the game draws now before what it drew earlier.
    bool newest_first = false;
};
// The request a worker takes next from the queue's front (oldest) or back (newest).
template <typename Queue>
typename Queue::value_type take_request(Queue& queue, bool newest_first) {
    auto job = std::move(newest_first ? queue.back() : queue.front());
    if (newest_first) queue.pop_back(); else queue.pop_front();
    return job;
}

// Requested programs by source key: queued until a worker takes one, then in flight until done.
class ProgramRequests {
public:
    struct Request { std::uint64_t key = 0; std::string vertex, fragment; };
    // False when the program is already queued or in flight.
    bool add(std::uint64_t key, std::string vertex, std::string fragment);
    // The next queued request, now in flight; false when none is queued.
    bool take(Request& request, bool newest_first);
    // Removes a queued request that no worker has taken; false when in flight or not requested.
    bool claim(std::uint64_t key);
    void done(std::uint64_t key) { pending_.erase(key); }
    bool pending(std::uint64_t key) const { return pending_.count(key) != 0; }
    bool queued() const { return !queue_.empty(); }
    void clear() { queue_.clear(); pending_.clear(); }

private:
    std::deque<Request> queue_;
    std::unordered_set<std::uint64_t> pending_;
};

// Builds programs into a program cache on threads with their own EGL contexts sharing
// the current one, so draws load binaries instead of compiling: first the fast programs
// of program lists (see ProgramList) missing from the cache, then whatever the renderer
// requests. Until background(), every worker builds at normal priority, for a start-up
// phase in which nothing else runs: beside a running game, builds stall its GL thread
// on Mali drivers. After it, request_workers threads continue (see GlesProgramBuilderPolicy).
// Construct with the renderer's context current; without EGL or a shareable context,
// running() is false.
class GlesProgramBuilder {
public:
    GlesProgramBuilder(GlesProgramCache&, std::vector<std::string> lists, FramebufferFetch, unsigned workers = 2,
                       GlesProgramBuilderPolicy policy = {});
    // Stops after the programs in progress.
    ~GlesProgramBuilder();
    GlesProgramBuilder(const GlesProgramBuilder&) = delete;
    GlesProgramBuilder& operator=(const GlesProgramBuilder&) = delete;
    bool running() const { return !threads_.empty() && !stop_; }
    void background();
    void request(const std::string& vertex, const std::string& fragment);
    // Requested and neither stored nor failed yet.
    bool building(const std::string& vertex, const std::string& fragment) const;
    // Takes back a request no worker has started, for the caller to link itself; false while
    // a worker builds it or when it is not requested.
    bool claim(const std::string& vertex, const std::string& fragment);
    GlesProgramBuilderStats stats() const;

private:
    struct Context;
    struct Entry { std::vector<std::uint32_t> words; bool batched = false, instanced = false; std::uint32_t options = 0; };
    void work(unsigned index);
    void plan();
    bool serve_request();
    void build(const std::string& vertex, const std::string& fragment, std::atomic<std::size_t>& count);
    GlesProgramCache& cache_;
    std::vector<std::string> lists_;
    FramebufferFetch fetch_;
    GlesProgramBuilderPolicy policy_;
    std::vector<std::unique_ptr<Context>> contexts_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    ProgramRequests requests_;
    std::vector<Entry> todo_; // Written before planned_, then read only.
    std::atomic<std::size_t> next_{0}, finished_{0};
    std::atomic<bool> stop_{false}, planned_{false}, background_{false};
    std::atomic<std::size_t> listed_{0}, built_{0}, rejected_{0}, failed_{0}, requested_{0};
    std::chrono::steady_clock::time_point start_;
    std::vector<std::thread> threads_;
};

} // namespace starlet::gx

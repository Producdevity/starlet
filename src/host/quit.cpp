#include "starlet/host/quit.hpp"

#include <SDL.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#ifdef __linux__
#include <pthread.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace starlet::host {
namespace {

// 0 until the quit is requested, then the requesting signal, or -1 for code.
std::atomic<int> requester{0};
// The write end of the pipe that wakes waiters; -1 until the first waiter or install.
std::atomic<int> wake_fd{-1};
static_assert(std::atomic<int>::is_always_lock_free, "the signal handler needs a lock-free flag");

std::mutex flush_mutex;
std::vector<std::function<void()>> flushes;
std::atomic<bool> exiting{false};
thread_local bool exiting_here = false;
std::atomic<bool> watching{false};

// A byte in the pipe stays there, so every poll on it returns once the quit is requested.
int wake_read_fd() {
    static const int read_end = [] {
        int ends[2];
        if (pipe(ends) != 0) throw std::runtime_error("quit: cannot create its wake pipe");
        for (int fd : ends) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
        wake_fd.store(ends[1], std::memory_order_release);
        return ends[0];
    }();
    return read_end;
}

void request(int who) noexcept {
    int none = 0;
    if (!requester.compare_exchange_strong(none, who)) return;
    const int fd = wake_fd.load(std::memory_order_acquire);
    if (fd >= 0) {
        const ssize_t written = write(fd, "q", 1);
        (void)written;
    }
}

void handle_signal(int signal) {
    const int saved = errno;
    request(signal);
    errno = saved;
}

void name_thread(const char* name) {
#ifdef __linux__
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

void wait_on_pipe(int timeout_ms) {
    pollfd wake{wake_read_fd(), POLLIN, 0};
    poll(&wake, 1, timeout_ms);
}

}

void install_quit_signals() {
    wake_read_fd();
    struct sigaction action {};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (int signal : {SIGTERM, SIGINT})
        if (sigaction(signal, &action, nullptr) != 0) throw std::runtime_error("quit: cannot install its signal handlers");
}

void request_quit() noexcept { request(-1); }

bool quit_requested() noexcept { return requester.load(std::memory_order_acquire) != 0; }

int quit_signal() noexcept { return std::max(requester.load(std::memory_order_acquire), 0); }

void wait_for_quit() {
    // The pipe exists before the check, so a request after it always wakes the poll.
    wake_read_fd();
    while (!quit_requested()) wait_on_pipe(-1);
}

bool wait_for_quit(std::chrono::milliseconds timeout) {
    wake_read_fd();
    const auto deadline = std::chrono::steady_clock::now() + std::min(timeout, std::chrono::milliseconds(INT_MAX));
    while (!quit_requested()) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        wait_on_pipe(int(std::min<std::chrono::milliseconds::rep>(left.count(), INT_MAX)));
    }
    return true;
}

void poll_events(const std::function<void(const SDL_Event&)>& handler) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) request_quit();
        if (handler) handler(event);
    }
}

void on_quit(std::function<void()> flush) {
    std::lock_guard<std::mutex> lock(flush_mutex);
    flushes.push_back(std::move(flush));
}

void exit_now(int status) {
    if (exiting.exchange(true)) {
        // A flush that exits again ends the process; any other thread waits for the first.
        if (exiting_here) std::_Exit(status);
        for (;;) pause();
    }
    exiting_here = true;
    std::vector<std::function<void()>> run;
    {
        std::lock_guard<std::mutex> lock(flush_mutex);
        run.swap(flushes);
    }
    for (const auto& flush : run) {
        try {
            flush();
        } catch (...) {
        }
    }
    std::fflush(nullptr);
    std::_Exit(status);
}

void exit_after_quit(std::chrono::milliseconds grace) {
    if (watching.exchange(true)) return;
    wake_read_fd();
    std::thread([grace] {
        name_thread("starlet-quit");
        wait_for_quit();
        std::this_thread::sleep_for(grace);
        exit_now(0);
    }).detach();
}

}

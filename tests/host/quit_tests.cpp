#include "starlet/host/quit.hpp"

#include <SDL.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

using namespace starlet::host;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Each mode runs in its own process: the quit request happens once per process.
namespace {

void require(bool value, const std::string& why) {
    if (!value) {
        std::fprintf(stderr, "quit: %s\n", why.c_str());
        std::exit(1);
    }
}

double since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

// A signal wakes a timed and an untimed waiter, and only sets the flag.
int signal_mode(int number, bool sdl_first) {
    if (sdl_first) require(SDL_Init(SDL_INIT_EVENTS) == 0, std::string("SDL events: ") + SDL_GetError());
    install_quit_signals();
    if (!sdl_first) require(SDL_Init(SDL_INIT_EVENTS) == 0, std::string("SDL events: ") + SDL_GetError());
    require(!quit_requested() && quit_signal() == 0, "no quit yet");
    const auto started = Clock::now();
    require(!wait_for_quit(30ms) && since(started) >= 0.029, "a wait without a request times out");

    std::atomic<double> timed_woke{-1}, untimed_woke{-1};
    Clock::time_point raised;
    std::thread timed([&] {
        const bool requested = wait_for_quit(10s);
        timed_woke = requested ? since(raised) : -1;
    });
    std::thread untimed([&] {
        wait_for_quit();
        untimed_woke = since(raised);
    });
    std::this_thread::sleep_for(50ms);
    raised = Clock::now();
    raise(number);
    timed.join();
    untimed.join();
    require(quit_requested() && quit_signal() == number, "the signal requested the quit");
    require(timed_woke >= 0 && timed_woke < 0.5 && untimed_woke >= 0 && untimed_woke < 0.5, "both waiters woke at once");
    require(wait_for_quit(0ms), "a later wait returns at once");
    // SDL saw no default handler (or lost its own), so it queued no SDL_QUIT of its own.
    bool sdl_quit = false;
    poll_events([&](const SDL_Event& event) { sdl_quit |= event.type == SDL_QUIT; });
    require(!sdl_quit, "Starlet's handler, not SDL's, took the signal");
    SDL_Quit();
    std::printf("quit: signal %d %s SDL wakes waiters\n", number, sdl_first ? "after" : "before");
    return 0;
}

// SDL_QUIT from the event queue requests the quit; every event reaches the handler.
int event_mode() {
    require(SDL_Init(SDL_INIT_EVENTS) == 0, std::string("SDL events: ") + SDL_GetError());
    SDL_Event user{};
    user.type = SDL_USEREVENT;
    SDL_PushEvent(&user);
    poll_events();
    require(!quit_requested(), "other events leave the flag alone");
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&user);
    SDL_PushEvent(&quit);
    unsigned seen = 0;
    poll_events([&](const SDL_Event& event) { seen |= event.type == SDL_USEREVENT ? 1 : event.type == SDL_QUIT ? 2 : 0; });
    require(quit_requested() && quit_signal() == 0 && seen == 3, "SDL_QUIT requests the quit and both events reach the handler");
    SDL_Quit();
    std::puts("quit: SDL_QUIT requests the quit");
    return 0;
}

// Runs body in a child that reports on a pipe; returns its exit status and report.
template <class Body> int child(Body body, std::string& report, double& seconds) {
    int ends[2];
    require(pipe(ends) == 0, "a report pipe");
    std::fflush(nullptr);
    const auto started = Clock::now();
    const pid_t pid = fork();
    require(pid >= 0, "fork");
    if (pid == 0) {
        close(ends[0]);
        body(ends[1]);
        std::_Exit(99);
    }
    close(ends[1]);
    char buffer[256];
    for (ssize_t got; (got = read(ends[0], buffer, sizeof buffer)) > 0;) report.append(buffer, std::size_t(got));
    int status = 0;
    waitpid(pid, &status, 0);
    seconds = since(started);
    close(ends[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
}

void say(int fd, const char* text) {
    const ssize_t written = write(fd, text, std::strlen(text));
    (void)written;
}

// Two threads exit at once: the flushes run once, in order, past one that throws.
int exit_mode() {
    std::string report;
    double seconds = 0;
    const int status = child(
        [](int fd) {
            on_quit([fd] { say(fd, "1"); });
            on_quit([] { throw std::runtime_error("a failing flush"); });
            on_quit([fd] {
                std::this_thread::sleep_for(50ms);
                say(fd, "2");
            });
            std::thread other([] { exit_now(5); });
            exit_now(3);
        },
        report, seconds);
    require(report == "12" && (status == 3 || status == 5), "flushes ran once, in order: '" + report + "' status " + std::to_string(status));

    report.clear();
    const int again = child(
        [](int fd) {
            on_quit([fd] {
                say(fd, "a");
                exit_now(9);
            });
            on_quit([fd] { say(fd, "b"); });
            exit_now(0);
        },
        report, seconds);
    require(again == 9 && report == "a", "a flush that exits ends the process at once");
    std::puts("quit: flushes run once on the exiting thread");
    return 0;
}

// The frontend's SIGTERM reaches a process whose main loop is stalled: the watcher flushes
// and exits after the grace period plus the flush, well inside gptokeyb's 3 s.
int watchdog_mode() {
    int report[2], ready[2];
    require(pipe(report) == 0 && pipe(ready) == 0, "pipes");
    std::fflush(nullptr);
    const pid_t pid = fork();
    require(pid >= 0, "fork");
    if (pid == 0) {
        close(report[0]);
        close(ready[0]);
        install_quit_signals();
        const int fd = report[1];
        on_quit([fd] { say(fd, "saved"); });
        exit_after_quit(200ms);
        say(ready[1], "r");
        for (;;) std::this_thread::sleep_for(1h);
    }
    close(report[1]);
    close(ready[1]);
    char byte = 0;
    require(read(ready[0], &byte, 1) == 1, "the child is ready");
    const auto sent = Clock::now();
    kill(pid, SIGTERM);
    std::string saved;
    char buffer[64];
    double flushed = -1;
    for (ssize_t got; (got = read(report[0], buffer, sizeof buffer)) > 0;) {
        saved.append(buffer, std::size_t(got));
        if (flushed < 0) flushed = since(sent);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const double exited = since(sent);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0 && saved == "saved", "the stalled process flushed and exited normally");
    require(flushed >= 0.2 && flushed < 1, "it saved " + std::to_string(flushed) + " s after SIGTERM");
    require(exited < 3, "it left " + std::to_string(exited) + " s after SIGTERM, past gptokeyb's 3 s");
    std::printf("quit: a stalled process saved %.0f ms and left %.0f ms after SIGTERM\n", flushed * 1000, exited * 1000);
    return 0;
}

}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "sigterm") return signal_mode(SIGTERM, false);
    if (mode == "sigint-after-sdl") return signal_mode(SIGINT, true);
    if (mode == "event") return event_mode();
    if (mode == "exit") return exit_mode();
    if (mode == "watchdog") return watchdog_mode();
    std::fprintf(stderr, "usage: %s sigterm|sigint-after-sdl|event|exit|watchdog\n", argv[0]);
    return 2;
}

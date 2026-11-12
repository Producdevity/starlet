#include "starlet/nand/nand.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace starlet::sdk;

namespace {
void require(bool value, const char* why) { if (!value) { std::fprintf(stderr, "NAND completions: %s\n", why); std::abort(); } }
struct Completion {
    NANDCommandBlock block{};
    std::mutex mutex;
    std::condition_variable changed;
    s32 result = 999;
    bool done = false, release = true, entered = false;
    std::thread::id caller = std::this_thread::get_id();
    Completion() { NANDSetUserData(&block, this); }
    static void receive(s32 result, NANDCommandBlock* block) {
        auto& self = *static_cast<Completion*>(NANDGetUserData(block));
        require(self.caller != std::this_thread::get_id(), "callback runs on worker");
        std::unique_lock<std::mutex> lock(self.mutex);
        self.entered = true; self.changed.notify_all();
        self.changed.wait(lock, [&] { return self.release; });
        self.result = result; self.done = true; self.changed.notify_all();
    }
    s32 wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return done; }), "completion timeout");
        return result;
    }
};
struct Chain {
    NANDCommandBlock block{};
    NANDFileInfo file{};
    std::array<char, 8> bytes{};
    Completion completion;
    unsigned stage = 0;
    Chain() { NANDSetUserData(&block, this); }
    static void next(s32 result, NANDCommandBlock* block) {
        auto& self = *static_cast<Chain*>(NANDGetUserData(block));
        switch (self.stage++) {
        case 0:
            require(result == 0, "chain open");
            require(NANDSeekAsync(&self.file, 2, 0, next, block) == 0, "callback reuses block for seek");
            break;
        case 1:
            require(result == 2, "absolute seek result");
            require(NANDReadAsync(&self.file, self.bytes.data(), self.bytes.size(), next, block) == 0, "callback read");
            break;
        case 2:
            require(result == 5 && !std::memcmp(self.bytes.data(), "cdefg", 5), "chain exact bytes and EOF");
            require(NANDSeek(&self.file, -2, 2) == 5 && NANDSeek(&self.file, -1, 1) == 4, "end and relative seek");
            require(NANDSeek(&self.file, -9, 0) == NAND_RESULT_INVALID && NANDSeek(&self.file, 0, 4) == NAND_RESULT_INVALID, "invalid seek rejected");
            require(NANDSafeCloseAsync(&self.file, next, block) == 0, "callback close");
            break;
        case 3: Completion::receive(result, &self.completion.block); break;
        default: require(false, "one callback per request");
        }
    }
};
}
int main() {
    char root[] = "/tmp/starlet-nand-async-XXXXXX";
    require(mkdtemp(root), "fixture directory");
    for (unsigned lifetime = 0; lifetime < 2; ++lifetime) {
        starlet::nand::start(root);
        if (!lifetime) {
            NANDFileInfo writer{};
            require(NANDCreate("save", 60, 0) == 0 && NANDOpen("save", &writer, NAND_ACCESS_WRITE) == 0, "seed writer");
            Completion write;
            require(NANDWriteAsync(&writer, "abcdefg", 7, Completion::receive, &write.block) == 0 && write.wait() == 7, "asynchronous write");
            require(NANDClose(&writer) == 0, "writer flush");
        }
        Completion missing;
        NANDFileInfo none{};
        std::array<unsigned char, 32> scratch{};
        require(NANDPrivateSafeOpenAsync("/shared2/menu/FaceLib/RFL_DB.dat", &none, 1, scratch.data(), scratch.size(), Completion::receive, &missing.block) == 0, "missing private database submitted");
        require(missing.wait() == NAND_RESULT_NOEXISTS, "private path absent, not host access");
        require(NANDPrivateSafeOpenAsync("save", &none, 2, scratch.data(), scratch.size(), Completion::receive, &missing.block) == NAND_RESULT_ACCESS, "write-safe transaction cannot silently degrade");
        Completion escape;
        require(NANDPrivateOpenAsync("/etc/passwd", &none, 1, Completion::receive, &escape.block) == 0 && escape.wait() == NAND_RESULT_ACCESS, "async host namespace rejected");
        Chain chain;
        require(NANDPrivateOpenAsync("save", &chain.file, 1, Chain::next, &chain.block) == 0, "chain submitted");
        require(chain.completion.wait() == 0 && chain.stage == 4, "open/seek/read/close callback chain");
        Completion held;
        held.release = false;
        require(NANDOpenAsync("save", &none, 1, Completion::receive, &held.block) == 0, "held callback submission");
        {
            std::unique_lock<std::mutex> lock(held.mutex);
            require(held.changed.wait_for(lock, std::chrono::seconds(5), [&] { return held.entered; }), "held callback entered");
        }
        Completion queued;
        u32 length = 0;
        require(NANDGetLengthAsync(&none, &length, Completion::receive, &queued.block) == 0, "queued request");
        require(NANDGetLengthAsync(&none, &length, Completion::receive, &queued.block) == NAND_RESULT_BUSY, "pending block cannot be overwritten");
        std::array<Completion, 32> capacity;
        for (unsigned i = 0; i < 31; ++i)
            require(NANDGetLengthAsync(&none, &length, Completion::receive, &capacity[i].block) == 0, "fill bounded queue");
        require(NANDGetLengthAsync(&none, &length, Completion::receive, &capacity[31].block) == NAND_RESULT_BUSY, "full queue rejected without replacing a request");
        {
            std::lock_guard<std::mutex> lock(held.mutex); held.release = true; held.changed.notify_all();
        }
        require(held.wait() == 0 && queued.wait() == 0, "ordered queued completion");
        for (unsigned i = 0; i < 31; ++i) require(capacity[i].wait() == 0, "full queue drained");
        require(length == 7, "length output after drain");
        Completion closing;
        require(NANDCloseAsync(&none, Completion::receive, &closing.block) == 0, "close queued before shutdown");
        starlet::nand::stop();
        require(closing.wait() == 0, "shutdown drains completion before caller storage release");
        require(NANDReadAsync(&none, scratch.data(), 1, Completion::receive, &closing.block) == NAND_RESULT_FATAL_ERROR, "no queue after shutdown");
    }
    char path[128];
    for (const auto* leaf : {"data/save", "data", "tmp"}) {
        std::snprintf(path, sizeof(path), "%s/%s", root, leaf);
        if (std::strchr(leaf, '/')) require(unlink(path) == 0, "remove owned fixture file");
        else require(rmdir(path) == 0, "remove owned fixture subdir");
    }
    require(rmdir(root) == 0, "remove fixture");
    std::puts("NAND completions: callback chaining, seek/EOF, worker ownership, queue bounds, shutdown and isolated private paths passed");
}

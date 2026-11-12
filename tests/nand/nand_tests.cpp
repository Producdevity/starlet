#include "starlet/nand/nand.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace starlet::sdk;

namespace {
void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "NAND: %s\n", message); std::abort(); } }
struct Fixture {
    char path[64] = "/tmp/starlet-nand-XXXXXX";
    Fixture() { require(mkdtemp(path), "fixture directory"); }
    std::string at(const char* file) const { return std::string(path) + "/" + file; }
    ~Fixture() {
        for (const auto* file : {"data/GameData.bin", "data/banner.bin", "data/link", "data/sub/jump", "tmp/banner.bin", "outside", "data/sub"}) {
            const auto entry = at(file); if (std::strchr(file, '.') || !std::strcmp(file,"outside") || !std::strcmp(file,"data/sub/jump")) unlink(entry.c_str());
            else rmdir(entry.c_str());
        }
        rmdir(at("data").c_str()); rmdir(at("tmp").c_str()); rmdir(path);
    }
};
}
int main() {
    using namespace starlet::nand;
    require(NANDInit() == NAND_RESULT_FATAL_ERROR, "unmounted service fails");
    char home[64]{}; require(NANDGetHomeDir(home) == NAND_RESULT_FATAL_ERROR, "unmounted home fails");
    Fixture fixture;
    for (unsigned lifetime = 0; lifetime < 2; ++lifetime) {
        start(fixture.path);
        require(NANDInit() == NAND_RESULT_OK && NANDGetHomeDir(home) == 0 && !std::strcmp(home,"/home"), "mounted namespace");
        bool duplicate = false;
        try { start(fixture.path); } catch (const std::logic_error&) { duplicate = true; }
        require(duplicate, "second owner rejected");
        NANDFileInfo info{}; u32 length = 99;
        if (lifetime == 0) {
            require(NANDOpen("GameData.bin", &info, NAND_ACCESS_READ) == NAND_RESULT_NOEXISTS, "first run missing save");
            require(NANDCreate("GameData.bin", 60, 0) == 0, "create save");
            require(NANDCreate("GameData.bin", 60, 0) == NAND_RESULT_EXISTS, "create preserves existing data");
            require(NANDOpen("GameData.bin", &info, NAND_ACCESS_WRITE) == 0, "open writer");
            require(NANDOpen("GameData.bin", &info, NAND_ACCESS_WRITE) == NAND_RESULT_OPENFD, "no handle overwrite");
            require(NANDRead(&info, home, 1) == NAND_RESULT_ACCESS, "write-only handle");
            const std::array<unsigned char,7> data{0x80,0,0x34,0x12,0xff,0xfe,0x7f};
            require(NANDWrite(&info, data.data(), data.size()) == s32(data.size()), "exact binary write");
            require(NANDWrite(&info, nullptr, 0) == 0 && NANDWrite(&info, data.data(), UINT32_MAX) == NAND_RESULT_INVALID, "bounded writes");
            require(NANDDelete("GameData.bin") == NAND_RESULT_OPENFD, "open save protected");
            require(NANDClose(&info) == 0 && NANDClose(&info) == NAND_RESULT_INVALID, "durable close and stale handle rejection");
            NANDFileInfo fake{}; fake.fileDescriptor = STDOUT_FILENO;
            require(NANDClose(&fake) == NAND_RESULT_INVALID, "foreign descriptors cannot be closed");
        }
        require(NANDOpen("/home/GameData.bin", &info, NAND_ACCESS_READ) == 0, "relative and absolute home agree");
        require(NANDGetLength(&info, &length) == 0 && length == 7, "persistent length across service lifetime");
        std::array<unsigned char,12> read{};
        require(NANDRead(&info,read.data(),2) == 2 && NANDRead(&info,read.data()+2,10) == 5 && NANDRead(&info,read.data()+7,1) == 0, "sequential read and EOF");
        const std::array<unsigned char,7> expected{0x80,0,0x34,0x12,0xff,0xfe,0x7f};
        require(!std::memcmp(read.data(),expected.data(),7), "exact persisted bytes");
        require(NANDWrite(&info,read.data(),1) == NAND_RESULT_ACCESS && NANDClose(&info) == 0, "read-only access");
        std::array<std::thread,4> workers;
        for (auto& worker : workers) worker = std::thread([] {
            NANDFileInfo file{}; std::array<unsigned char,7> data{};
            require(NANDOpen("GameData.bin",&file,NAND_ACCESS_READ) == 0, "worker open");
            require(NANDRead(&file,data.data(),7) == 7 && data[0] == 0x80 && data[6] == 0x7f, "independent worker position");
            require(NANDClose(&file) == 0, "worker close");
        });
        for (auto& worker : workers) worker.join();
        std::array<NANDFileInfo,17> files{};
        for (unsigned i=0;i<16;++i) require(NANDOpen("GameData.bin",&files[i],NAND_ACCESS_READ) == 0, "bounded handle slots");
        require(NANDOpen("GameData.bin",&files[16],NAND_ACCESS_READ) == NAND_RESULT_MAXFD, "handle limit");
        for (unsigned i=0;i<16;++i) require(NANDClose(&files[i]) == 0, "handle slot release");
        u32 answer = 99; require(NANDCheck(5,2,&answer) == 0 && answer == 0, "real filesystem availability");
        require(NANDCheck(0,0,nullptr) == NAND_RESULT_INVALID, "invalid check output");
        for (const auto* path : {"../outside", "a/../outside", "./GameData.bin", "/home/../outside", "/home//GameData.bin", "/tmp/", ""})
            require(NANDCreate(path,60,0) == NAND_RESULT_INVALID, "traversal or malformed path rejected");
        require(NANDOpen("/etc/passwd",&info,NAND_ACCESS_READ) == NAND_RESULT_ACCESS, "absolute host path rejected");
        if (!lifetime) {
            const auto outside = fixture.at("outside");
            int fd=open(outside.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600); require(fd>=0 && write(fd,"keep",4)==4 && close(fd)==0,"outside fixture");
            require(!symlink("../outside",fixture.at("data/link").c_str()), "symlink fixture");
            require(NANDOpen("link",&info,NAND_ACCESS_READ) == NAND_RESULT_ACCESS && NANDDelete("link") == NAND_RESULT_ACCESS, "leaf symlink blocked");
            require(!mkdir(fixture.at("data/sub").c_str(),0700) && !symlink("../../tmp",fixture.at("data/sub/jump").c_str()), "intermediate symlink fixture");
            require(NANDCreate("sub/jump/escape",60,0) < 0,"intermediate symlink blocked");
            require(NANDCreate("/tmp/banner.bin",60,0) == 0 && NANDOpen("/tmp/banner.bin",&info,NAND_ACCESS_WRITE) == 0,"temporary banner");
            require(NANDWrite(&info,"banner",6) == 6 && NANDClose(&info) == 0,"banner bytes");
            require(NANDMove("/tmp/banner.bin",home) == 0,"banner move to home directory");
            require(NANDOpen("banner.bin",&info,NAND_ACCESS_READ) == 0 && NANDGetLength(&info,&length) == 0 && length == 6 && NANDClose(&info) == 0,"moved banner persisted");
            fd=open(outside.c_str(),O_RDONLY); char guard[4]{}; require(fd>=0 && ::read(fd,guard,4)==4 && close(fd)==0 && !std::memcmp(guard,"keep",4),"outside file unchanged");
        }
        // Shutdown owns descriptors without retaining or dereferencing caller storage.
        require(NANDOpen("GameData.bin",&info,NAND_ACCESS_READ) == 0,"open at shutdown");
        stop();
        require(NANDGetLength(&info,&length) == NAND_RESULT_FATAL_ERROR,"unmounted handles fail");
    }
    require(!unlink(fixture.at("data/link").c_str()) && !rmdir(fixture.at("tmp").c_str()),"rollback setup");
    require(!symlink("data",fixture.at("tmp").c_str()),"bad storage symlink");
    bool rejected=false;
    try { start(fixture.path); } catch(const std::runtime_error&) { rejected=true; }
    require(rejected && NANDInit()==NAND_RESULT_FATAL_ERROR,"partial start rollback");
    require(!unlink(fixture.at("tmp").c_str()),"remove bad fixture symlink");
    start(fixture.path); stop();
    std::puts("NAND: persisted binary I/O, move, access, worker handles, bounds, namespace and rollback passed");
}

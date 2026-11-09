#include "starlet/gx/gles/program_cache.hpp"
#include "starlet/gx/gles/program_list.hpp"
#include <GLES3/gl32.h>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fake {
extern GLint program_binary_formats;
extern GLboolean binary_links;
extern GLenum binary_format;
extern std::vector<unsigned char> program_binary, loaded_binary;
extern unsigned retrievable_hints;
extern const char* renderer_name;
} // namespace fake
using starlet::gx::GlesProgramCache;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::string> entries(const std::string& directory) {
    std::vector<std::string> names;
    if (auto* dir = opendir(directory.c_str())) {
        while (const auto* e = readdir(dir)) if (e->d_name[0] != '.') names.push_back(directory + '/' + e->d_name);
        closedir(dir);
    }
    return names;
}
void clear(const std::string& directory) {
    for (const auto& name : entries(directory)) std::remove(name.c_str());
    rmdir(directory.c_str());
}
std::string temporary_directory() {
    char base[] = "/tmp/starlet-program-cache-XXXXXX";
    if (!mkdtemp(base)) throw std::runtime_error("mkdtemp failed");
    return std::string(base) + "/programs";
}
void reset() {
    fake::program_binary_formats = 1;
    fake::binary_links = GL_TRUE;
    fake::binary_format = 0x1234;
    fake::program_binary = {1, 2, 3, 4, 5};
    fake::loaded_binary.clear();
    fake::retrievable_hints = 0;
    fake::renderer_name = "private dispatch fixture";
}

void stores_and_loads(const std::string& directory) {
    reset();
    {
        GlesProgramCache cache(directory);
        check(cache.enabled(), "cache enabled with binary formats and a writable parent");
        check(!cache.load("vs", "fs"), "first lookup misses");
        cache.prepare(7);
        check(fake::retrievable_hints == 1, "binary retrievable hint set before linking");
        cache.store(7, "vs", "fs");
        const auto s = cache.stats();
        check(s.misses == 1 && s.stored == 1 && s.store_failures == 0, "linked binary stored");
        check(entries(directory).size() == 1, "one entry file written, no temporary left");
    }
    GlesProgramCache cache(directory);
    check(cache.load("vs", "fs") != 0, "next run loads the stored binary");
    check(fake::loaded_binary == fake::program_binary, "driver receives the stored bytes");
    check(!cache.load("vs", "fs2"), "different fragment source misses");
    check(!cache.load("vs2", "fs"), "different vertex source misses");
    const auto s = cache.stats();
    check(s.hits == 1 && s.misses == 2 && s.rejected == 0, "hit and miss counts");
}

void rejects_driver_refusal(const std::string& directory) {
    reset();
    { GlesProgramCache cache(directory); cache.store(3, "a", "b"); }
    fake::binary_links = GL_FALSE;
    GlesProgramCache cache(directory);
    check(!cache.load("a", "b"), "binary the driver refuses is not used");
    check(cache.stats().rejected == 1, "refusal counted");
    fake::binary_links = GL_TRUE;
    check(!cache.load("a", "b") && cache.stats().misses == 1, "refused entry deleted");
}

void rejects_corruption(const std::string& directory) {
    reset();
    { GlesProgramCache cache(directory); cache.store(3, "c", "d"); }
    const auto files = entries(directory);
    check(files.size() == 1, "single entry to corrupt");
    if (auto* file = std::fopen(files[0].c_str(), "r+b")) {
        std::fseek(file, -10, SEEK_END);
        std::fputc(0x5a, file);
        std::fclose(file);
    }
    GlesProgramCache cache(directory);
    check(!cache.load("c", "d") && fake::loaded_binary.empty(), "corrupt entry never reaches the driver");
    check(cache.stats().rejected == 1 && entries(directory).empty(), "corrupt entry rejected and deleted");
}

void keys_on_driver_identity(const std::string& directory) {
    reset();
    { GlesProgramCache cache(directory); cache.store(3, "e", "f"); }
    fake::renderer_name = "another driver";
    GlesProgramCache cache(directory);
    check(!cache.load("e", "f") && fake::loaded_binary.empty(), "binary from another driver is not loaded");
}

void removes_stale_temporaries(const std::string& directory) {
    reset();
    { GlesProgramCache cache(directory); }
    const auto stale = directory + "/0123456789abcdef.bin.tmp";
    const auto foreign = directory + "/notes.txt";
    for (const auto& name : {stale, foreign}) if (auto* file = std::fopen(name.c_str(), "wb")) std::fclose(file);
    GlesProgramCache cache(directory);
    check(access(stale.c_str(), F_OK) != 0, "interrupted write removed");
    check(access(foreign.c_str(), F_OK) == 0, "files the cache did not name are kept");
    std::remove(foreign.c_str());
}

void disabled_without_support(const std::string& directory) {
    reset();
    fake::program_binary_formats = 0;
    GlesProgramCache unsupported(directory);
    unsupported.prepare(1);
    unsupported.store(1, "g", "h");
    check(!unsupported.enabled() && fake::retrievable_hints == 0 && entries(directory).empty(),
          "driver without binary formats disables the cache");
    reset();
    GlesProgramCache unset("");
    check(!unset.enabled() && !unset.load("g", "h"), "empty directory disables the cache");
    GlesProgramCache orphan(directory + "/missing/programs");
    check(!orphan.enabled(), "missing parent directory disables the cache");
}
void trims_oldest_entries(const std::string& directory) {
    reset();
    { GlesProgramCache cache(directory); check(cache.enabled(), "cache enabled for trimming"); }
    const auto total = GlesProgramCache::max_entries + 8;
    for (std::size_t i = 0; i < total; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "/%016zx.bin", i);
        const auto path = directory + name;
        std::fclose(std::fopen(path.c_str(), "wb"));
        const timeval times[2] = {{static_cast<time_t>(1000 + i), 0}, {static_cast<time_t>(1000 + i), 0}};
        utimes(path.c_str(), times);
    }
    GlesProgramCache cache(directory);
    const auto kept = entries(directory);
    check(kept.size() == GlesProgramCache::max_entries * 3 / 4, "trimmed to three quarters of the limit");
    char newest[64];
    std::snprintf(newest, sizeof newest, "/%016zx.bin", total - 1);
    check(access((directory + newest).c_str(), F_OK) == 0, "newest entry kept");
    check(access((directory + "/0000000000000000.bin").c_str(), F_OK) != 0, "oldest entry removed");
    check(!cache.contains("x", "y"), "missing sources are not contained");
}

void records_program_lists(const std::string& directory) {
    mkdir(directory.c_str(), 0755);
    const auto path = directory + "/programs.list";
    {
        starlet::gx::ProgramList list(path);
        list.record({1, 0xba7c4ed1u, 0});
        list.record({1, 0xba7c4ed1u, 0});
        list.record({2, 3});
    }
    auto read = starlet::gx::read_program_list(path);
    check(read.size() == 2 && read[0] == std::vector<std::uint32_t>{1, 0xba7c4ed1u, 0} && read[1] == std::vector<std::uint32_t>{2, 3},
          "recorded words read back once each");
    auto* file = std::fopen(path.c_str(), "a");
    std::fputs("4 5", file);
    std::fclose(file);
    {
        starlet::gx::ProgramList list(path);
        list.record({2, 3});
        list.record({6});
    }
    read = starlet::gx::read_program_list(path);
    check(read.size() == 4 && read[2] == std::vector<std::uint32_t>{4, 5} && read[3] == std::vector<std::uint32_t>{6},
          "a cut line stays separate from later records");
    check(starlet::gx::read_program_list(directory + "/missing.list").empty(), "missing list reads empty");
}
} // namespace

int main() {
    const auto directory = temporary_directory();
    try {
        for (auto test : {stores_and_loads, rejects_driver_refusal, rejects_corruption, keys_on_driver_identity,
                          removes_stale_temporaries, disabled_without_support, trims_oldest_entries,
                          records_program_lists}) {
            clear(directory);
            test(directory);
        }
        clear(directory);
        rmdir(directory.substr(0, directory.rfind('/')).c_str());
        std::cout << "GLES program cache tests passed (fake driver, not GPU proof)\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}

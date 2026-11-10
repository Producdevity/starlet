#include "disc_fixture.hpp"
#include "starlet/disc/fst.hpp"

using namespace starlet::disc;
using namespace starlet::disc::fixture;

namespace {
// Root, Dir/{Hello.bin, Sub/Child}, root.bin, with a Wii word offset past 4 GiB.
Bytes wii_table() {
    constexpr char names[] = "\0Dir\0Hello.bin\0Sub\0Child\0root.bin\0";
    Bytes data(72 + sizeof names);
    std::memcpy(data.data() + 72, names, sizeof names);
    put32(data, 0, 0x01000000); put32(data, 8, 6);
    put32(data, 12, 0x01000001); put32(data, 20, 5);
    put32(data, 24, 5); put32(data, 28, 0xf0000000); put32(data, 32, 5);
    put32(data, 36, 0x0100000f); put32(data, 40, 1); put32(data, 44, 5);
    put32(data, 48, 19); put32(data, 52, 0x100); put32(data, 56, 3);
    put32(data, 60, 25); put32(data, 64, 0x200); put32(data, 68, 0);
    return data;
}

void wii_lookup() {
    const auto bytes = wii_table();
    const FileTable table(bytes, 2);
    require(table.size() == 6 && table.entry(2).offset == 0x3c0000000ull && table.entry(4).offset == 0x400,
            "Wii word offsets become 64-bit byte offsets");
    require(table.entry(1).directory && table.entry(1).next == 5 && table.entry(4).parent == 3, "directory extents and parents");
    require(table.path(4) == "/Dir/Sub/Child" && table.path(0) == "/", "paths from stored names");
    require(table.resolve("/dir/HELLO.BIN") == 2u, "SDK case folding");
    require(table.resolve("../Hello.bin", 3) == 2u && table.resolve("../../..", 3) == 0u, "relative parent lookup");
    require(table.resolve(".", 3) == 3u && table.resolve("") == 0u, "current directory lookup");
    require(table.resolve("/Dir//root.bin") == 5u, "a repeated slash returns to the root");
    require(!table.resolve("/Dir/Sub/Child/") && !table.resolve("/missing"), "a file with a trailing slash, and a missing name");
    require(table.resolve("/Dir/") == 1u, "a directory with a trailing slash");
    require(!failure([&] { table.resolve("a", 2); }).empty(), "a file is not a working directory");
    require(!failure([&] { table.resolve(std::string_view("a\0b", 3)); }).empty(), "an embedded NUL is rejected");
    require(failure([&] { table.entry(6); }) == "game file table entry 6 does not exist", "entry range");

    const auto rejects = [&](Bytes copy, const char* what) {
        require(!failure([&] { FileTable invalid(copy, 2); }).empty(), what);
    };
    const auto mutation = [&](std::size_t at, std::uint32_t value, const char* what) {
        auto copy = bytes;
        put32(copy, at, value);
        rejects(copy, what);
    };
    mutation(8, 0, "no entries");
    mutation(8, 65537, "too many entries");
    mutation(0, 0, "a root that is a file");
    mutation(40, 0, "a directory naming the wrong parent");
    mutation(44, 6, "a directory reaching past its parent");
    mutation(44, 3, "a directory ending before itself");
    mutation(24, 0x02000005, "an unknown entry type");
    mutation(24, 0x00ffffff, "a name outside the table");
    auto unterminated = bytes;
    unterminated.resize(74);
    unterminated[73] = 'x';
    rejects(unterminated, "an unterminated name");
    auto parent = bytes;
    std::memcpy(parent.data() + 73, "..", 3);
    rejects(parent, "a parent-directory name");
    auto duplicate = bytes;
    put32(duplicate, 60, 1);
    require(failure([&] { FileTable invalid(duplicate, 2); }) == "game file table repeats the name Dir",
            "siblings that differ only in case are rejected");
}

void gamecube_table() {
    const GameCubeDisc disc;
    auto fst = disc.span(GameCubeDisc::fst_at, disc.fst_size);
    const FileTable table(fst, 0);
    require(table.size() == 9, "entry count");
    require(table.entry(4).offset == 0x10000 && table.entry(4).length == 0x9000 && table.path(4) == "/Data/b.arc",
            "GameCube byte offsets are not shifted");
    require(table.entry(3).length == 0 && table.host_path(3) == "/Data/Sub/zero.bin", "an empty file inside nested folders");
    require(table.entry(5).directory && table.entry(5).next == 6 && table.path(5) == "/Empty", "an empty folder");
    require(table.resolve("/data/sub/ZERO.BIN") == 3u && table.resolve("Data/../d.bin") == 8u, "lookup");

    auto renamed = fst;
    const auto names = std::size_t{9} * 12;
    std::memcpy(renamed.data() + names + 36, "\x83\x5c" "bin", 5);
    const FileTable japanese(renamed, 0);
    require(japanese.path(7) == "/\x83\x5c" "bin" && japanese.host_path(7) == "/%83%5Cbin", "a Shift-JIS name is escaped on the host");

    auto unsafe = fst;
    std::memcpy(unsafe.data() + names + 42, "..\0\0\0", 5);
    require(failure([&] { FileTable invalid(unsafe, 0); }) == "unsafe name in the game file table", "a parent-directory name");
    auto folded = fst;
    std::memcpy(folded.data() + names + 24, "A.bin", 5);
    require(failure([&] { FileTable invalid(folded, 0); }) == "game file table repeats the name a.bin",
            "names that differ only in case are rejected");
}

void host_names() {
    require(host_name("gameConfig.ini") == "gameConfig.ini", "ASCII names are kept");
    require(host_name("\x83v\x83\x8d\x83_\x83N\x83ggameConfig.ini") == "%83v%83%8D%83_%83N%83ggameConfig.ini",
            "Shift-JIS bytes are escaped");
    require(host_name("100%.txt") == "100%25.txt" && host_name("a:b*c?\"<>|\\") == "a%3Ab%2Ac%3F%22%3C%3E%7C%5C",
            "the escape and characters FAT forbids are escaped");
    require(host_name("name.") == "name%2E" && host_name("name ") == "name%20" && host_name("a b.c") == "a b.c",
            "a final dot or space is escaped");
    require(host_name(std::string("\x01\x7f", 2)) == "%01%7F", "control bytes are escaped");
}
}

int main() {
    wii_lookup();
    gamecube_table();
    host_names();
    std::puts("FST: GameCube and Wii tables, SDK lookup and host names");
    return 0;
}

#include "starlet/card/card.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace starlet::card;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string work;

std::string fresh(const std::string& name) {
    const std::string path = work + "/" + name;
    std::system(("rm -rf '" + path + "'").c_str());
    return path;
}

std::vector<std::string> listing(const std::string& path) {
    std::vector<std::string> names;
    if (DIR* dir = opendir(path.c_str())) {
        while (const dirent* e = readdir(dir))
            if (e->d_name[0] != '.') names.emplace_back(e->d_name);
        closedir(dir);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }
// __CARDCheckSum stores 0xFFFF as 0.
std::uint16_t stored(std::uint32_t sum) { return std::uint16_t(sum) == 0xFFFF ? 0 : std::uint16_t(sum); }

// Pikmin 2's save file as Dolphin exports it: the entry the SDK wrote (GPVE, maker 01,
// CI8 banner, icon at 0, two CI8 icon frames at speed 3, public, first block 5, 27
// blocks, comment at 0x1C00).
const std::uint8_t pikmin_entry[entry_size] = {
    'G', 'P', 'V', 'E', '0', '1', 0xFF, 0x01, 'P', 'i', 'k', 'm', 'i', 'n', '2', '_',
    'S', 'a', 'v', 'e', 'D', 'a', 't', 'a', 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0x2E, 0x8C, 0x15, 0x2A, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x05, 0x00, 0x0F, 0x04, 0x00, 0x00, 0x05, 0x00, 0x1B, 0xFF, 0xFF, 0x00, 0x00, 0x1C, 0x00,
};

File sample(const char* game, const char* company, const char* name, int blocks, std::uint8_t fill) {
    File f;
    f.entry.set_owner(game, company);
    f.entry.set_name(name);
    f.entry.set_time(1234);
    f.entry.set_banner_format(0);
    f.entry.set_icon_address(no_address);
    f.entry.set_icon_format(0);
    f.entry.set_icon_speed(1);
    f.entry.set_permission(4);
    f.entry.set_copy_times(0);
    f.entry.set_first_block(0);
    f.entry.set_length(std::uint16_t(blocks));
    f.entry.set_comment_address(no_address);
    f.data.assign(std::size_t(blocks) * block_size, fill);
    for (std::size_t i = 0; i < f.data.size(); i += 4096) f.data[i] = std::uint8_t(i / 4096);
    return f;
}

void gci_fixture() {
    std::vector<std::uint8_t> bytes(pikmin_entry, pikmin_entry + entry_size);
    bytes.resize(entry_size + 27 * block_size, 0x5A);
    const auto file = read_gci(bytes.data(), bytes.size());
    check(bool(file), "Dolphin GCI reads");
    if (!file) return;
    const Entry& e = file->entry;
    check(e.game() == "GPVE" && e.company() == "01" && e.name() == "Pikmin2_SaveData", "GCI owner and name");
    check(e.length() == 27 && e.first_block() == 5 && e.comment_address() == 0x1C00 && e.icon_address() == 0, "GCI geometry");
    check(e.icon_format() == 0x0005 && e.icon_speed() == 0x000F && e.permission() == 4 && e.banner_format() == 1, "GCI icon fields");
    check(e.time() == 0x2E8C152A, "GCI time");
    check(gci_name(e) == "01-GPVE-Pikmin2_SaveData.gci", "Dolphin file name");
    check(write_gci(*file) == bytes, "GCI writes back byte for byte");
    bytes.pop_back();
    check(!read_gci(bytes.data(), bytes.size()), "a GCI whose size disagrees with its length is rejected");
    Entry odd;
    odd.set_owner("GABC", "0Q");
    odd.set_name("a/b:c");
    check(gci_name(odd) == "0Q-GABC-a__2f__b__3a__c.gci", "unsafe name bytes are escaped as Dolphin does");
}

void identity() {
    Identity id;
    for (int i = 0; i < 12; ++i) id.flash_id[i] = std::uint8_t(0x31 * i + 7);
    id.format_time = 0x0123456789ABCDEFull;
    id.counter_bias = 0x11223344;
    id.language = 2;
    id.vi_dtv = 1;
    const auto s = serial_bytes(id);
    check(flash_id_of(s.data()) == id.flash_id, "the flash ID is recovered from the serial");
    check(be16(s.data() + 12) == 0x0123 && s[19] == 0xEF && s[23] == 0x44 && s[27] == 2 && s[31] == 1, "serial layout");
    std::uint64_t expect = 0;
    for (int w = 0; w < 4; ++w) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = v << 8 | s[8 * w + i];
        expect ^= v;
    }
    check(serial_number(s) == expect, "CARDGetSerialNo exclusive-ors the serial's words");
    std::array<std::uint8_t, 12> flash{};
    flash.fill(1);
    check(flash_id_check(flash) == std::uint8_t(~12), "SRAM flash ID check byte");
    check(os_time(946684800, 0) == 0 && os_time(946684801, 3600) == 3601ull * 40500000u, "OSTime from host time");
}

void images() {
    Identity id;
    id.flash_id.fill(0x42);
    id.format_time = 987654321;
    auto empty = build_image(4, id, {});
    check(empty && empty->size() == 64 * block_size, "Memory Card 59 image size");
    if (!empty) return;
    const std::uint8_t* img = empty->data();
    std::uint16_t sum, inverse;
    checksum(img, 0x1FC, sum, inverse);
    check(be16(img + 0x1FC) == sum && be16(img + 0x1FE) == inverse && be16(img + 0x22) == 4 && be16(img + 0x20) == 0, "ID block");
    const std::uint8_t* fat = img + 3 * block_size;
    check(be16(fat + 6) == 59 && be16(fat + 8) == 4, "an empty card has 59 free blocks, last allocated 4");
    for (int copy = 0; copy < 2; ++copy) {
        const std::uint8_t* f = img + (3 + copy) * block_size;
        std::uint32_t s = 0, si = 0;  // __CARDCheckSum over the check code onwards.
        for (std::size_t i = 4; i < block_size; i += 2) {
            s += be16(f + i);
            si += std::uint16_t(~be16(f + i));
        }
        check(be16(f) == stored(s) && be16(f + 2) == stored(si) && be16(f + 4) == copy, "allocation table checksums");
        const std::uint8_t* d = img + (1 + copy) * block_size;
        s = si = 0;
        for (std::size_t i = 0; i < block_size - 4; i += 2) {
            s += be16(d + i);
            si += std::uint16_t(~be16(d + i));
        }
        check(be16(d + 0x1FFC) == stored(s) && be16(d + 0x1FFE) == stored(si), "directory checksums");
    }
    check(be16(img + block_size + 0x1FFA) == 0 && be16(img + 2 * block_size + 0x1FFA) == 1, "directory check codes");
    check(parse_image(img, empty->size(), id.flash_id, Encoding::ansi).result == ready, "a formatted card verifies");

    std::vector<File> files = {sample("GPVE", "01", "Pikmin2_SaveData", 27, 0x11), sample("GOTH", "8P", "other", 3, 0x22)};
    auto full = build_image(16, id, files);
    check(bool(full), "two files fit a Memory Card 251");
    if (!full) return;
    auto parsed = parse_image(full->data(), full->size(), id.flash_id, Encoding::ansi);
    check(parsed.result == ready && parsed.files.size() == 2, "files read back from an image");
    if (parsed.files.size() == 2) {
        check(parsed.files[0].data == files[0].data && parsed.files[1].data == files[1].data, "file data survives the image");
        check(parsed.files[0].entry.first_block() == 5 && parsed.files[1].entry.first_block() == 32, "contiguous allocation");
    }
    check(be16(full->data() + 3 * block_size + 6) == 251 - 30, "free blocks count the files");

    auto damaged = *full;
    damaged[block_size + 10] ^= 0xFF;  // Directory copy 0.
    check(parse_image(damaged.data(), damaged.size(), id.flash_id, Encoding::ansi).result == ready, "one damaged directory copy is repaired");
    damaged[2 * block_size + 10] ^= 0xFF;
    check(parse_image(damaged.data(), damaged.size(), id.flash_id, Encoding::ansi).result == broken, "both directory copies damaged is broken");
    auto other = id.flash_id;
    other[0] ^= 1;
    check(parse_image(full->data(), full->size(), other, Encoding::ansi).result == broken, "another console's card is broken");
    check(parse_image(full->data(), full->size(), id.flash_id, Encoding::shift_jis).result == encoding, "encoding mismatch");
    std::vector<std::uint8_t> blank(64 * block_size, 0xFF);
    check(parse_image(blank.data(), blank.size(), id.flash_id, Encoding::ansi).result == no_card, "an unformatted card");
    std::vector<File> too_many(128, sample("GPVE", "01", "x", 1, 0));
    check(!build_image(64, id, too_many), "more than 127 files do not fit");
}

void sdk() {
    const std::string path = fresh("sdk") + "/USA/Card A";
    Console console;
    console.flash_id[0].fill(7);
    auto folder = std::make_shared<Folder>(path, 0, console);
    check(folder->ok(), "the folder is created with its parents");
    Card card(folder, "GPVE", "01");
    check(card.mount() == ready && card.check() == ready, "a new card mounts formatted");
    std::int32_t bytes = 0, files = 0;
    check(card.free_blocks(bytes, files) == ready && bytes == 251 * 8192 && files == 127, "an empty Memory Card 251");
    Card::FileInfo info;
    check(card.open("Pikmin2_SaveData", info) == no_file, "no file yet");
    check(card.create("Pikmin2_SaveData", 27 * block_size, info) == ready && info.file_no == 0, "CARDCreate");
    check(card.create("Pikmin2_SaveData", block_size, info) == exist, "a second create of the same name");
    check(listing(path) == std::vector<std::string>({"01-GPVE-Pikmin2_SaveData.gci", "card.txt"}), "the folder holds Dolphin's file");
    std::vector<std::uint8_t> save(2 * block_size);
    for (std::size_t i = 0; i < save.size(); ++i) save[i] = std::uint8_t(i * 7);
    check(card.write(info, save.data(), std::int32_t(save.size()), std::int32_t(block_size)) == ready, "CARDWrite");
    check(card.write(info, save.data(), 512, 0) == fatal_error, "writes are whole blocks");
    Card::Stat stat{};
    check(card.get_status(0, stat) == ready && stat.length == 27 * block_size, "CARDGetStatus");
    stat.icon_address = 0;
    stat.icon_format = 5;
    stat.banner_format = 1;
    stat.comment_address = 0x1C00;
    check(card.set_status(0, stat) == ready, "CARDSetStatus");
    std::uint64_t serial = 0;
    check(card.serial(serial) == ready, "CARDGetSerialNo");

    Card again(folder, "GPVE", "01");
    check(again.mount() == ready, "remount");
    check(again.open("Pikmin2_SaveData", info) == ready, "the file persists");
    std::vector<std::uint8_t> back(save.size());
    check(again.read(info, back.data(), std::int32_t(back.size()), std::int32_t(block_size)) == ready && back == save, "data persists");
    std::uint64_t serial2 = 1;
    check(again.serial(serial2) == ready && serial2 == serial, "the serial number persists");
    check(again.get_status(0, stat) == ready && stat.comment_address == 0x1C00 && stat.offset_banner == 0 && stat.offset_icon[0] == 96 * 32 + 512,
          "status persists, offsets follow the banner");
    Card stranger(folder, "GXXE", "01");
    check(stranger.mount() == ready && stranger.open("Pikmin2_SaveData", info) == no_file, "other games do not open the file");
    check(stranger.fast_open(0, info) == ready && stranger.get_status(0, stat) == ready, "public files are readable to others");
    check(stranger.erase(0) == no_permission, "other games cannot delete it");

    check(again.rename("Pikmin2_SaveData", "Renamed") == ready, "CARDRename");
    check(listing(path) == std::vector<std::string>({"01-GPVE-Renamed.gci", "card.txt"}), "the renamed file replaces the old one");
    check(again.erase("Renamed") == ready, "CARDDelete");
    auto after = listing(path);
    check(after.size() == 2 && after[1].rfind("removed-", 0) == 0, "a deleted file is kept aside");

    check(again.create("Pikmin2_SaveData", block_size, info) == ready, "create again");
    check(again.format() == ready && again.open("Pikmin2_SaveData", info) == no_file, "CARDFormat empties the card");
    std::uint64_t serial3 = 0;
    again.serial(serial3);
    check(serial3 != serial, "a formatted card has a new serial");

    // A damaged identity and stray files.
    { std::ofstream(path + "/card.txt") << "junk"; }
    { std::ofstream(path + "/broken.gci") << "not a gci"; }
    Card damaged(folder, "GPVE", "01");
    check(damaged.mount() == ready, "a damaged card.txt gives a fresh identity, stray files are skipped");

    Console japan = console;
    japan.encoding = Encoding::shift_jis;
    Card sjis(std::make_shared<Folder>(path, 0, japan), "GPVJ", "01");
    check(sjis.mount() == encoding && sjis.check() == encoding, "a card in another encoding");
    check(sjis.create("x", block_size, info) == broken, "an unusable card refuses files");

    auto missing = std::make_shared<Folder>("/proc/no/such/dir", 0, console);
    Card none(missing, "GPVE", "01");
    check(!none.present() && none.mount() == no_card, "no folder, no card");
    check(folder_for("/s", region_of('E'), 0) == "/s/USA/Card A" && folder_for("/s", region_of('P'), 1) == "/s/EUR/Card B" &&
          folder_for("/s", region_of('J'), 0) == "/s/JAP/Card A", "Dolphin's folder layout");
}

void hardware() {
    const std::string path = fresh("hardware");
    Console console;
    console.flash_id[1].fill(9);
    auto folder = std::make_shared<Folder>(path, 1, console, 4);
    Card card(folder, "GMSE", "01");
    Card::FileInfo info;
    check(card.mount() == ready && card.create("super_mario_sunshine", 7 * block_size, info) == ready, "a save made at the SDK level");
    auto image = folder->image();
    auto parsed = parse_image(image.data(), image.size(), console.flash_id[1], Encoding::ansi);
    check(parsed.result == ready && parsed.files.size() == 1 && parsed.files[0].entry.name() == "super_mario_sunshine",
          "the device sees a formatted card holding the folder's file");

    // The game writes its file and creates a second one through the device.
    parsed.files[0].data[100] = 0xAB;
    parsed.files.push_back(sample("GMSE", "01", "second", 2, 0x33));
    Identity id;
    folder->load_identity(id);
    auto changed = build_image(4, id, parsed.files);
    check(changed && folder->store_image(changed->data(), changed->size()) == ready, "a changed image is stored");
    check(card.mount() == ready && card.open("second", info) == ready, "the new file is in the folder");
    std::vector<std::uint8_t> block(512);
    check(card.open("super_mario_sunshine", info) == ready && card.read(info, block.data(), 512, 0) == ready && block[100] == 0xAB,
          "the changed file is in the folder");

    // The game formats the card: a new format time, no files.
    Identity formatted = id;
    formatted.format_time += 4050;
    auto blank = build_image(4, formatted, {});
    check(folder->store_image(blank->data(), blank->size()) == ready, "a formatted image is stored");
    Identity now;
    folder->load_identity(now);
    check(now.format_time == formatted.format_time && card.mount() == ready && card.open("second", info) == no_file,
          "formatting through the device empties the folder and keeps the new identity");
    // A card formatted on another console (another flash ID) imports its files.
    Identity foreign;
    foreign.flash_id.fill(0x77);
    foreign.format_time = 123456789;
    auto raw = build_image(4, foreign, {sample("GMSE", "01", "imported", 3, 0x44)});
    check(folder->store_image(raw->data(), raw->size()) == broken, "another console's image is not stored");
    check(folder->import_image(raw->data(), raw->size()) == ready && card.mount() == ready && card.open("imported", info) == ready,
          "another console's image imports its files");
    std::vector<std::uint8_t> erased(image.size(), 0xFF);
    check(folder->store_image(erased.data(), erased.size()) == no_card, "an erased image leaves the folder alone");
}

}

int main(int argc, char** argv) {
    work = argc > 1 ? argv[1] : "/tmp/starlet_card_tests";
    std::system(("mkdir -p '" + work + "'").c_str());
    gci_fixture();
    identity();
    images();
    sdk();
    hardware();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("card tests passed");
    return 0;
}

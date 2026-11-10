#include "disc_fixture.hpp"
#include "starlet/disc/install.hpp"

using namespace starlet::disc;
using namespace starlet::disc::fixture;

namespace {
Game pikmin(const GameCubeDisc& disc, const Manifest* manifest = nullptr) {
    return {"Pikmin 2 (USA)", {{"GPVE01", 0, 0, disc.dol_digest, manifest}}};
}

void manifests() {
    const auto manifest = parse_manifest(std::string(40, 'a') + "  sys/main.dol\n\n" + std::string(40, '0') + "  files/a b.bin\n");
    require(manifest.size() == 2 && manifest.at("files/a b.bin") == std::string(40, '0'), "sha1sum lines parse");
    require(failure([&] { parse_manifest("not a manifest\n"); }) == "damaged file manifest", "manifest syntax checked");
    require(failure([&] { parse_manifest(std::string(40, 'A') + "  x\n"); }) == "damaged file manifest", "digests are lowercase");
    const auto twice = std::string(40, 'a') + "  x\n";
    require(failure([&] { parse_manifest(twice + twice); }) == "duplicate file manifest entry", "duplicates rejected");
}

void image_search(const Scratch& dir) {
    const auto folder = dir / "gamefiles";
    const auto touch = [&](const char* name) { std::ofstream(folder + "/" + name) << "x"; };
    const auto search = [&](unsigned formats) { return failure([&] { find_disc_image(folder, formats, "Pikmin 2 (USA)"); }); };
    require(search(formats::all) == folder + " is missing", "a missing folder is named");
    require(::mkdir(folder.c_str(), 0755) == 0, "gamefiles folder");
    touch("README.txt");
    touch("._game.rvz");
    const auto ask = "copy your Pikmin 2 (USA) disc image (RVZ, WIA, ISO or GCM) to " + folder;
    require(search(formats::all) == ask + ", then launch the game again", "the README and hidden files don't count");
    touch("Pikmin 2.7z");
    require(search(formats::all) == ask + "; Pikmin 2.7z is not a disc image", "an archive is named");
    require(::mkdir((folder + "/Pikmin 2").c_str(), 0755) == 0, "subfolder");
    require(search(formats::all) == ask + "; Pikmin 2.7z is not a disc image", "the first entry is reported");
    touch("Pikmin 2.nkit.iso");
    require(search(formats::all) == "Pikmin 2.nkit.iso: only RVZ, WIA, ISO or GCM disc images are supported", "NKit named");
    touch("Pikmin 2.GCM");
    require(find_disc_image(folder, formats::all, "Pikmin 2 (USA)") == folder + "/Pikmin 2.GCM",
            "extensions ignore case and win over other formats");
    require(search(formats::rvz | formats::wia) == "Pikmin 2.GCM: only RVZ or WIA disc images are supported",
            "an accepted container is named when the port takes fewer formats");
    touch("Pikmin 2.rvz");
    require(find_disc_image(folder, formats::rvz, "Pikmin 2 (USA)") == folder + "/Pikmin 2.rvz", "only accepted formats count");
    require(search(formats::all) == "put only one disc image in " + folder, "two images rejected");
    require(search(0) == "no disc image format is accepted", "a port names at least one format");
}

void identification(const Scratch& dir) {
    const GameCubeDisc disc;
    const auto game = pikmin(disc);
    const std::pair<GameCubeDisc, const char*> wrong[] = {
        {GameCubeDisc("GPVP01"), "this is not Pikmin 2 (USA): found GPVP01 revision 0"},
        {GameCubeDisc("GPVE01", 0, 1), "this is not Pikmin 2 (USA): found GPVE01 revision 1"},
        {GameCubeDisc("GPVE01", 1, 0), "this is not Pikmin 2 (USA): found GPVE01 disc 2 revision 0"},
    };
    for (const auto& [other, message] : wrong) {
        IsoImage image(other.iso(dir / "other.iso"));
        require(failure([&] { identify_disc(image, game); }) == message, message);
    }
    const WiiDisc wii;
    write_file(dir / "wii.iso", wii.disc);
    IsoImage wii_iso(dir / "wii.iso");
    require(failure([&] { identify_disc(wii_iso, game); }) == "this is not Pikmin 2 (USA): found RMGE01 revision 0",
            "a Wii disc is named by its ID");
    const Game galaxy{"Super Mario Galaxy (USA or Europe)", {{"RMGE01", 0, 0, wii.dol_digest}}};
    require(failure([&] { identify_disc(wii_iso, galaxy); }) ==
                "this Wii disc image is a plain copy, which keeps the game encrypted; convert it to RVZ with Dolphin",
            "a supported Wii disc as ISO gets a way forward");
    write_file(dir / "zeros.iso", Bytes(0x1000));
    IsoImage zeros(dir / "zeros.iso");
    require(failure([&] { identify_disc(zeros, game); }) == "not a GameCube or Wii disc image", "non-disc data rejected");
    IsoImage image(disc.iso(dir / "known.iso"));
    const Game unknown{"Pikmin 2 (USA)", {{"GPVE01", 0, 0, std::string(40, '0')}}};
    require(failure([&] { identify_disc(image, unknown); }) ==
                "main.dol of GPVE01 revision 0 is not a supported release (SHA-1 " + disc.dol_digest + ")",
            "an unknown main.dol is named");
    require(identify_disc(image, game).id == "GPVE01", "the supported disc is identified");
}

void gamecube_extract(const Scratch& dir) {
    const GameCubeDisc disc;
    const auto manifest = disc.manifest();
    const auto game = pikmin(disc, &manifest);
    WiaOptions rvz, packed_zstd, wia;
    packed_zstd.compression = 5;
    packed_zstd.chunk = 0x10000;
    packed_zstd.mixed = true;
    wia.rvz = false;
    wia.chunk = 0x200000;
    const std::string images[] = {disc.wia(dir / "small.rvz", rvz), disc.wia(dir / "zstd.rvz", packed_zstd),
                                  disc.wia(dir / "plain.wia", wia), disc.iso(dir / "game.iso")};
    unsigned n = 0;
    for (const auto& path : images) {
        const auto image = open_disc_image(path);
        const auto work = dir / ("work" + std::to_string(n++));
        std::uint64_t reported = 0, reported_total = 0;
        SetupOptions options;
        options.progress = [&](std::uint64_t done, std::uint64_t total) {
            reported = done;
            reported_total = total;
        };
        const auto result = extract(*image, work, game, options);
        require(result.disc.id == "GPVE01" && result.files == 5 && result.bytes == 0x9000 + 0x1234 + 0x100 + 0x2000,
                path + ": file count and size");
        require(reported == reported_total &&
                    reported == result.bytes + 0x440 + 0x2000 + GameCubeDisc::apploader_size + disc.fst_size + GameCubeDisc::dol_size,
                path + ": progress covers system and game files");
        for (const auto& file : GameCubeDisc::files)
            require(read_file(work + "/gamedata/" + file.path) == disc.span(file.offset, file.length), path + ": " + file.path);
        require(exists(work + "/gamedata/files/Data/Sub/zero.bin"), path + ": empty file written");
        require(is_directory(work + "/gamedata/files/Empty"), path + ": empty directory written");
        require(read_file(work + "/gamedata/sys/main.dol") == disc.dol(), path + ": main.dol written");
        require(read_file(work + "/gamedata/sys/apploader.img") == disc.span(0x2440, GameCubeDisc::apploader_size),
                path + ": apploader written");
        require(!exists(work + "/gamedata/.prepared"), path + ": extract leaves the marker to install");
    }

    IsoImage image(dir / "game.iso");
    require(failure([&] { extract(image, dir / "work0", game); }).rfind("cannot create " + (dir / "work0") + ": ", 0) == 0 &&
                exists(dir / "work0/gamedata/sys/main.dol"),
            "an existing work folder is refused and kept");

    auto wrong = manifest;
    wrong["files/a.bin"] = std::string(40, '0');
    require(failure([&] { extract(image, dir / "mismatch", pikmin(disc, &wrong)); }) ==
                "extracted file does not match the reference: files/a.bin",
            "manifest mismatch rejected");
    require(!exists(dir / "mismatch"), "a mismatch leaves no output");
    auto unlisted = manifest;
    unlisted.erase("files/c.bin");
    require(failure([&] { extract(image, dir / "unlisted", pikmin(disc, &unlisted)); }) ==
                "extracted file does not match the reference: files/c.bin",
            "an unlisted file rejected");
    auto missing = manifest;
    missing["files/missing.bin"] = std::string(40, '0');
    require(failure([&] { extract(image, dir / "missing", pikmin(disc, &missing)); }) ==
                "disc image lacks files listed in the reference",
            "a missing file rejected");
    require(!exists(dir / "unlisted") && !exists(dir / "missing"), "failed extractions leave no output");

    IsoImage cut(disc.iso(dir / "cut.iso", 0x1000));
    require(failure([&] { extract(cut, dir / "cut", game); }) == "the disc image is incomplete: files/d.bin lies past its end",
            "a trimmed image missing a file is rejected");
    require(!exists(dir / "cut"), "a short image is rejected before anything is written");

    GameCubeDisc japanese;
    std::memcpy(japanese.bytes.data() + GameCubeDisc::c_name, "\x83\x5c" "bin", 5);
    IsoImage sjis(japanese.iso(dir / "japanese.iso"));
    extract(sjis, dir / "japanese", pikmin(japanese));
    require(read_file(dir / "japanese/gamedata/files/%83%5Cbin") == japanese.span(0x30000, 0x100), "a Shift-JIS name is escaped");

    SetupOptions only_fst;
    only_fst.system_files = sys_files::fst;
    bool hooked = false;
    only_fst.hook = [&](DiscImage& hooked_image, const SupportedDisc& found, const std::string& work) {
        hooked = &hooked_image == &image && found.id == "GPVE01" && work == dir / "fst-only" &&
                 read_file(work + "/gamedata/files/d.bin") == disc.span(0x5a000, 0x2000);
        write_file(work + "/extra.bin", {1});
    };
    extract(image, dir / "fst-only", pikmin(disc), only_fst);
    require(hooked && exists(dir / "fst-only/extra.bin"), "the hook runs after the files are written");
    require(exists(dir / "fst-only/gamedata/sys/fst.bin") && !exists(dir / "fst-only/gamedata/sys/main.dol") &&
                !exists(dir / "fst-only/gamedata/sys/boot.bin"),
            "only the selected system files are written");
    SetupOptions failing;
    failing.hook = [](DiscImage&, const SupportedDisc&, const std::string&) { throw DiscError("hook failed"); };
    require(failure([&] { extract(image, dir / "hook-fails", pikmin(disc), failing); }) == "hook failed" &&
                !exists(dir / "hook-fails"),
            "a failing hook removes the output");
}

// Galaxy's setup: a Wii RVZ, only sys/fst.bin, a manifest for one of two discs, and
// embedded assets carved from main.dol by the port's hook.
void wii_install(const Scratch& dir) {
    const WiiDisc usa("RMGE01"), europe("RMGP01");
    const auto manifest = usa.manifest();
    const Game galaxy{"Super Mario Galaxy (USA or Europe)",
                      {{"RMGE01", 0, 0, usa.dol_digest, &manifest}, {"RMGP01", 0, 0, europe.dol_digest}}};
    SetupOptions options;
    options.system_files = sys_files::fst;
    options.outputs = {"embedded"};
    options.hook = [](DiscImage& image, const SupportedDisc& disc, const std::string& work) {
        GameVolume volume(image);
        const auto dol = volume.main_dol();
        require(::mkdir((work + "/embedded").c_str(), 0755) == 0, "embedded folder");
        write_file(work + "/embedded/" + disc.id + ".bin", Bytes(dol.begin() + 0x100, dol.begin() + 0x110));
    };
    WiaOptions rvz;
    rvz.compression = 5;
    rvz.exceptions = 1;
    const auto port = dir / "galaxy";
    require(::mkdir(port.c_str(), 0755) == 0, "port folder");
    require(!is_prepared(port, galaxy), "nothing is prepared yet");
    WiaImage image(usa.wia(dir / "usa.rvz", rvz));
    const auto result = install(image, port, galaxy, options);
    require(result.disc.id == "RMGE01" && result.files == 4 && result.bytes == 0x9000 + 0x1234 + 0x100 + 0x1000, "Wii files counted");
    require(read_file(port + "/gamedata/.prepared") == Bytes{'R', 'M', 'G', 'E', '0', '1', ' ', 'r', 'e', 'v', 'i', 's', 'i', 'o',
                                                             'n', ' ', '0', ',', ' ', 's', 'e', 't', 'u', 'p', ' ', '1', '\n'},
            "Galaxy's marker text is kept");
    require(is_prepared(port, galaxy), "the installed disc is prepared");
    require(read_file(port + "/gamedata/sys/fst.bin") == usa.fst() && !exists(port + "/gamedata/sys/boot.bin"),
            "only the FST among system files");
    require(read_file(port + "/gamedata/files/Data/b.arc") == usa.file(0) && read_file(port + "/gamedata/files/c.bin") == usa.file(2),
            "files across sectors and next to padding");
    require(read_file(port + "/embedded/RMGE01.bin") == slice(usa.dol(), 0x100, 0x10), "the hook's output is published");
    require(!exists(port + "/.setup-work") && !exists(port + "/.setup-old-embedded"), "staging removed");

    WiaImage pal(europe.wia(dir / "europe.rvz", rvz));
    install(pal, port, galaxy, options);
    const auto marker = prepared_marker(galaxy, galaxy.discs[1]);
    require(read_file(port + "/gamedata/.prepared") == Bytes(marker.begin(), marker.end()) &&
                marker == "RMGP01 revision 0, setup 1\n" && is_prepared(port, galaxy),
            "the second disc installs without the first disc's manifest");
    require(exists(port + "/embedded/RMGP01.bin") && !exists(port + "/embedded/RMGE01.bin"), "outputs are replaced as a whole");

    const Game japanese{"Super Mario Galaxy (USA or Europe)", {{"RMGJ01", 0, 0, usa.dol_digest}}};
    require(!is_prepared(port, japanese), "another disc's marker does not count");
    Game resetup = galaxy;
    resetup.setup = 2;
    require(!is_prepared(port, resetup), "a new setup version runs setup again");
}

void gamecube_install(const Scratch& dir) {
    const GameCubeDisc disc;
    const auto manifest = disc.manifest();
    const auto game = pikmin(disc, &manifest);
    const auto port = dir / "port";
    require(::mkdir(port.c_str(), 0755) == 0 && ::mkdir((port + "/gamedata").c_str(), 0755) == 0, "port folder");
    write_file(port + "/gamedata/stale.bin", {1, 2, 3});
    write_file(port + "/gamedata/.prepared", {'x'});
    require(!is_prepared(port, game), "a foreign marker is not prepared");
    IsoImage image(disc.iso(dir / "install.iso"));
    install(image, port, game);
    require(is_prepared(port, game) && prepared_marker(game, game.discs[0]) == "GPVE01 revision 0, setup 1\n",
            "installed data is marked as Pikmin 2 marks it");
    require(!exists(port + "/gamedata/stale.bin") && !exists(port + "/.setup-work") && !exists(port + "/.setup-old-gamedata"),
            "the old folder and staging are removed");
    require(read_file(port + "/gamedata/files/d.bin") == disc.span(0x5a000, 0x2000), "installed file");

    auto wrong = manifest;
    wrong["files/d.bin"] = std::string(40, '0');
    require(failure([&] { install(image, port, pikmin(disc, &wrong)); }) == "extracted file does not match the reference: files/d.bin",
            "failed install reported");
    require(!exists(port + "/.setup-work"), "a failed install removes its partial output");
    require(read_file(port + "/gamedata/files/d.bin") == disc.span(0x5a000, 0x2000) && is_prepared(port, game),
            "a failed install keeps the previous data");
    SetupOptions missing_output;
    missing_output.outputs = {"embedded"};
    require(failure([&] { install(image, port, game, missing_output); }).rfind("cannot publish " + port + "/embedded: ", 0) == 0 &&
                !exists(port + "/.setup-work") && is_prepared(port, game),
            "an output the hook did not write fails the install");
    missing_output.outputs = {"../escape"};
    require(failure([&] { install(image, port, game, missing_output); }) == "invalid setup output ../escape", "outputs stay in the port");
}
}

int main() {
    manifests();
    const Scratch dir("starlet-disc-install");
    image_search(dir);
    identification(dir);
    gamecube_extract(dir);
    wii_install(dir);
    gamecube_install(dir);
    std::puts("disc setup: search, identification, extraction with manifests and hooks, install and markers");
    return 0;
}

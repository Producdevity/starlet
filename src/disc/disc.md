# Disc images and first-launch setup

`starlet::disc` reads the user's own GameCube or Wii disc image. A port uses it
once, at first launch, to extract the game's files into `gamedata/`, or at run
time to serve the game's disc reads straight from the image.

## Images

`open_disc_image(path)` picks the reader by signature (`WIA\x01`, `RVZ\x01`,
then the GameCube magic word at 0x1C or the Wii one at 0x18) and falls back to
the extension (`.wia`, `.rvz`, `.iso`, `.gcm`). Every reader throws
`DiscError`, whose text is a sentence the user can act on.

`IsoImage` reads a plain copy. Its `size()` is the file size, so a trimmed copy
is shorter than the disc; it works while every byte the port reads lies inside
it, and a read past its end names the offset and asks whether the copy is
trimmed or incomplete.

`WiaImage` reads WIA 1.00 and RVZ as Dolphin's `docs/WiaAndRvz.md` describes
them, for GameCube and Wii discs:

- The header, disc-description and partition-table SHA-1s are checked, and
  `wia_file_size` must equal the file size, so a damaged or incomplete copy
  fails when it is opened.
- Groups are stored, Zstandard frames, or empty (all zeros). An RVZ group's
  top size bit selects Zstandard; in a WIA image every group uses the image's
  method. Dolphin writes Zstandard only to RVZ.
- bzip2, LZMA, LZMA2 and PURGE images are refused with an error that names the
  method and tells the user to convert the image with Dolphin to RVZ with
  Zstandard or no compression.
- Chunk sizes are powers of two from 32 KiB to 1 MiB (RVZ only) or multiples of
  2 MiB up to 64 MiB.
- RVZ packing regenerates disc padding with `JunkGenerator` from the stored
  seed and the offset within its 32 KiB block: the disc offset for raw data, the
  partition data offset (hashes not counted) for Wii partitions.
- Wii partition data is stored decrypted without hashes, so no Wii key is
  involved. `read_partition()` addresses it in 0x7C00 bytes per 0x8000-byte
  sector, across both `wia_part_data_t` segments; hash exception lists are
  skipped. `read()` serves the rest of the disc (header, partition table,
  partition headers) and refuses partition data. `data_partition()` finds the
  DATA partition (type 0) through the table at 0x40000 and the data offset at
  0x2B8 of its header.

`read()` and `read_partition()` may run on several threads at once. `IsoImage`
holds no shared state; `WiaImage` serializes reads with one mutex around its
single decoded-group cache and Zstandard decoder. Its buffers only grow: one
decoded chunk, plus the largest stored and decompressed group read so far.

`GameVolume` is the game's own file system: the whole disc on GameCube, the
DATA partition on Wii. Offsets count from `boot.bin`, and `shift()` is the
shift that `boot.bin` and the FST apply to their offsets (2 on Wii, 0 on
GameCube). `main_dol()` reads `main.dol` up to its furthest section, `fst()`
the FST. A Wii disc in an ISO is refused there, since its partitions are
encrypted; the error tells the user to convert it to RVZ.

## File system table

`FileTable` parses `fst.bin` with the volume's shift and rejects a malformed
table: a bad root, more than 65,536 entries, nesting deeper than 64, directory
extents outside their parent, unterminated, empty, `.`, `..` or slash-holding
names, and siblings that differ only in case after `host_name()`, which a FAT
card cannot hold side by side.

`host_name()` maps an FST name to a host file name: every byte outside
printable ASCII, each of `"*:<>?\|%`, and a final `.` or space becomes `%XX`
(uppercase hex). Names stay ASCII, valid on FAT, exFAT and APFS, and map back
uniquely; Shift-JIS developer files on Pikmin 2 are the case that needs it.
`host_path()` joins those names, `path()` the stored ones. `resolve()` is the
DVD SDK lookup: ASCII case-insensitive, `.` and `..`, and a `/` at the start of
any component returns to the root.

## First-launch setup

`install.hpp` is the setup every port runs before the game can boot:

```cpp
using namespace starlet::disc;
const auto manifest = parse_manifest(read_text(manifest_path));
const Game game{"Pikmin 2 (USA)", {{"GPVE01", 0, 0, GPVE01_DOL_SHA1, &manifest}}};
if (is_prepared(port, game)) return;
const auto image = open_disc_image(find_disc_image(gamefiles, formats::all, game.title));
SetupOptions options;
options.progress = [](std::uint64_t done, std::uint64_t total) { /* one stdout line per percent */ };
install(*image, port, game, options);
```

`find_disc_image(directory, accepted, title)` returns the one accepted image in
the folder. README.txt and dot files are skipped. NKit, CISO, GCZ, WBFS, WDF and
TGC images, and containers the port does not accept, are named in the error
("Pikmin 2.nkit.iso: only RVZ, WIA, ISO or GCM disc images are supported"). An
empty folder or one without an image gets "copy your <title> disc image (<formats>)
to <folder>", followed by the first other entry it holds, since the user reads
this on the device and cannot look.

`identify_disc` matches the disc header's ID, disc number and revision against
`Game::discs`, then the SHA-1 of `main.dol`, before anything is written. The
errors name what was found: "this is not Pikmin 2 (USA): found GPVP01 revision 0",
"main.dol of GPVE01 revision 0 is not a supported release (SHA-1 ...)".

`extract(image, work, game, options)` then checks the FST, that every output
lies inside the image ("the disc image is incomplete: files/d.bin lies past its
end") and the free space, and writes:

| Path under `work/gamedata` | Source in the game volume |
|--|--|
| `sys/boot.bin` | 0x440 bytes at 0 |
| `sys/bi2.bin` | 0x2000 bytes at 0x440 |
| `sys/apploader.img` | 0x2440: the 0x20-byte header plus the code and trailer sizes at 0x14 and 0x18 |
| `sys/fst.bin` | offset and size at 0x424 and 0x428, shifted |
| `sys/main.dol` | offset at 0x420, shifted; ends at its furthest section |
| `files/...` | the FST tree under `host_path()` names |

`SetupOptions::system_files` selects the `sys/` files (`sys_files::all` by
default). With a manifest on the matched `SupportedDisc` (SHA-1 per gamedata
path, as `sha1sum` writes it), each file must match as it is written and the
manifest must list nothing else. The hook runs last, with the image, the matched
disc and `work`, and writes the port's own outputs beside `gamedata/`. `work`
must not exist, and a failed extraction removes it.

`install(image, port, game, options)` extracts into `port/.setup-work`, writes
`gamedata/.prepared`, syncs, then renames each directory in
`SetupOptions::outputs` and finally `gamedata` into `port`, moving earlier
copies aside and deleting them once the new one is in place. A failed run
removes `.setup-work` and keeps the previous data. The marker is
`<ID> revision <n>, setup <n>` and a newline, `Game::setup` being 1 unless the
port raises it to make installed copies run setup again, so Galaxy's
`RMGE01 revision 0, setup 1` and Pikmin 2's `GPVE01 revision 0, setup 1`
carry over. `is_prepared()` accepts the marker of any of the game's discs.

## Zstandard

`third_party/zstd` is Zstandard v1.5.6 (BSD-3-Clause) as a submodule. Only
`lib/common` and `lib/decompress` are built, into the static `starlet_zstd`,
because some handheld firmware ships no libzstd. The tests also build
`lib/compress` to write their images.

## Tests

`tests/disc` builds small GameCube and Wii discs in memory and writes them as
ISO, WIA and RVZ with valid hashes (`disc_fixture.hpp`). They cover SHA-1
vectors; the padding generator against the spec's other formulation and across
skips; the same GameCube disc as ISO, stored RVZ (32 and 128 KiB groups, one and
two raw entries), Zstandard RVZ with stored groups, and stored and Zstandard
WIA, read back byte-identical over sequential and 800 random reads; a Wii disc
as stored and Zstandard RVZ and WIA with both partition segments, hash
exceptions, padded exception lists and packed padding; four threads reading one
image; damaged, incomplete and unsupported images; GameCube and Wii FST parsing,
SDK lookup and host names; and the setup flow: image search, identification,
extraction from every format, manifest mismatches and short images that leave no
output, the hook, selected system files, install's replacement, outputs and
cleanup on failure, and both ports' markers.

Real images read the same as their plain copies: Luigi's Mansion (USA) and
Super Mario Sunshine (USA) as stored RVZ match their ISOs over all 1,459,978,240
bytes, and the Pikmin 2 (USA) stored RVZ hashes to its Redump SHA-1
`63a68c656d654388e096d27a768e85823dcf3327`. `install()` of that RVZ wrote
Pikmin 2's gamedata exactly as its manifest lists it (2,768 files plus the five
system files, Shift-JIS names escaped) with the marker `GPVE01 revision 0,
setup 1`. No real Wii or Zstandard image has been read through this module yet.

# Discs and memory cards

`starlet::disc` reads the user's own GameCube or Wii disc image. A port uses it once, at first
launch, to extract the game's files into `gamedata/`, or at run time to serve the game's disc reads
straight from the image. `starlet::card` keeps GameCube memory cards as Dolphin GCI folders. For the
SDK calls that sit on top of both (DVD, CARD) see [SDK services](SDK-services.md).

# Discs

## Images

`open_disc_image(path)` picks the reader by signature (`WIA\x01`, `RVZ\x01`, then the GameCube
magic word at 0x1C or the Wii one at 0x18) and falls back to the extension (`.wia`, `.rvz`,
`.iso`, `.gcm`). Every reader throws `DiscError`, whose text is a sentence the user can act on.
`read()` and `read_partition()` may run on several threads at once.

`IsoImage` reads a plain copy. Its `size()` is the file size, so a trimmed copy is shorter than
the disc; it works while every byte the port reads lies inside it, and a read past its end names
the offset and asks whether the copy is trimmed or incomplete. It holds no shared state.

`WiaImage` reads WIA 1.00 and RVZ as Dolphin's `docs/WiaAndRvz.md` describes them, for GameCube
and Wii discs:

- The header, disc-description and partition-table SHA-1s are checked, and `wia_file_size` must
  equal the file size, so a damaged or incomplete copy fails when it is opened.
- Groups are stored, Zstandard frames, or empty (all zeros). An RVZ group's top size bit selects
  Zstandard; in a WIA image every group uses the image's method. Dolphin writes Zstandard only to
  RVZ.
- bzip2, LZMA, LZMA2 and PURGE images are refused with an error that names the method and tells
  the user to convert the image with Dolphin to RVZ with Zstandard or no compression.
- Chunk sizes are powers of two from 32 KiB to 1 MiB (RVZ only) or multiples of 2 MiB up to 64 MiB.
- The first raw-data entry starts at 0x80; groups begin on sector boundaries.
- RVZ packing regenerates disc padding with `JunkGenerator` from the stored seed and the offset
  within its 32 KiB block: the disc offset for raw data, the partition data offset (hashes not
  counted) for Wii partitions. The generator is a lagged Fibonacci (f = xor, j = 32, k = 521)
  seeded with 68 bytes whose output restarts at every 0x8000-byte boundary. The seeding step that
  replaces bits 16-23 of each word with bits 18-25 is easy to get wrong: the second byte of each
  word comes from bits 18-25.
- Wii partition data is stored decrypted without hashes, 0x7C00 bytes per 0x8000-byte sector, so
  no Wii key is involved. `read_partition()` addresses it across both `wia_part_data_t` segments;
  hash exception lists are skipped, since they only matter when re-creating encrypted sectors.
  `read()` serves the rest of the disc (header, partition table, partition headers) and refuses
  partition data. `data_partition()` finds the DATA partition (type 0) through the table at
  0x40000 and the data offset at 0x2B8 of its header.
- A Zstandard group is one complete frame; the decoder gets one spare output byte so a frame that
  fills the limit exactly finishes.

`WiaImage` serializes reads with one mutex around its single decoded-group cache and Zstandard
decoder. Its buffers only grow: one decoded chunk, plus the largest stored and decompressed group
read so far.

`GameVolume` is the game's own file system: the whole disc on GameCube, the DATA partition on Wii.
Offsets count from `boot.bin`, and `shift()` is the shift that `boot.bin` and the FST apply to
their offsets (2 on Wii, 0 on GameCube). `main_dol()` reads `main.dol` up to its furthest section
(seven text and eleven data sections: file offsets at 0x00, sizes at 0x90), `fst()` the FST. A Wii
disc in an ISO is refused there, since its partitions are encrypted; the error tells the user to
convert it to RVZ.

## File system table

`FileTable` parses `fst.bin`: big-endian 12-byte records, then the name table. Wii tables store
file offsets divided by 4, hence the volume's shift. It rejects a malformed table: a bad root, more
than 65,536 entries, nesting deeper than 64, directory extents outside their parent, unterminated,
empty, `.`, `..` or slash-holding names, and siblings that differ only in case after `host_name()`,
which a FAT card cannot hold side by side.

`host_name()` maps an FST name (bytes; some discs carry Shift-JIS) to a host file name: every byte
outside printable ASCII, each of `"*:<>?\|%`, and a final `.` or space becomes `%XX` (uppercase
hex). Names stay ASCII, valid on FAT, exFAT and APFS, and map back uniquely; Shift-JIS developer
files on Pikmin 2 are the case that needs it. `host_path()` joins those names, `path()` the stored
ones. `resolve()` is the DVD SDK lookup: ASCII case-insensitive, `.` and `..`, and a `/` at the
start of any component returns to the root. Relative paths start at the current directory.

## First-launch setup

`install.hpp` is the setup every port runs before the game can boot. The port describes its game
with a `Game`: a title (as in "this is not <title>"), the `SupportedDisc`s it accepts (game ID at
0x00, disc number at 0x06, revision at 0x07, the SHA-1 of `main.dol`, an optional manifest) and
`setup`, a number that is part of the prepared marker.

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

`find_disc_image(directory, accepted, title)` returns the one accepted image in the folder.
README.txt and dot files are skipped. NKit, CISO, GCZ, WBFS, WDF and TGC images, and containers the
port does not accept, are named in the error ("Pikmin 2.nkit.iso: only RVZ, WIA, ISO or GCM disc
images are supported"). An empty folder or one without an image gets "copy your <title> disc image
(<formats>) to <folder>", followed by the first other entry it holds, since the user reads this on
the device and cannot look.

`identify_disc` matches the disc header's ID, disc number and revision against `Game::discs`, then
the SHA-1 of `main.dol`, before anything is written. It only reads. The errors name what was found:
"this is not Pikmin 2 (USA): found GPVP01 revision 0", "main.dol of GPVE01 revision 0 is not a
supported release (SHA-1 ...)".

`extract(image, work, game, options)` then checks the FST, that every output lies inside the image
("the disc image is incomplete: files/d.bin lies past its end") and the free space, and writes:

| Path under `work/gamedata` | Source in the game volume |
|--|--|
| `sys/boot.bin` | 0x440 bytes at 0 |
| `sys/bi2.bin` | 0x2000 bytes at 0x440 |
| `sys/apploader.img` | 0x2440: the 0x20-byte header plus the code and trailer sizes at 0x14 and 0x18 |
| `sys/fst.bin` | offset and size at 0x424 and 0x428, shifted |
| `sys/main.dol` | offset at 0x420, shifted; ends at its furthest section |
| `files/...` | the FST tree under `host_path()` names |

`SetupOptions::system_files` selects the `sys/` files (`sys_files::all` by default). With a
manifest on the matched `SupportedDisc` (lowercase SHA-1 per gamedata path, as `sha1sum` writes it:
`<40 hex digits>  <path>`), each file must match as it is written and the manifest must list
nothing else. A file of an earlier installed disc's `gamedata/files` with the same path, size and
bytes is checked against the disc and not written again. The hook runs last, after the gamedata
files are written and checked, with the image, the matched disc and `work`, and writes the port's
own outputs beside `gamedata/`. `work` must not exist, and a failed extraction removes it.

`install(image, port, game, options)` extracts into `port/.setup-work`, writes
`gamedata/.prepared`, syncs, then renames each directory in `SetupOptions::outputs` and finally
`gamedata` into `port`, moving earlier copies aside and deleting them once the new one is in
place. A failed run removes `.setup-work` and keeps the previous data, and cleanup never replaces
the error being reported. The marker is `<ID> revision <n>, setup <n>` and a newline, `Game::setup`
being 1 unless the port raises it to make installed copies run setup again, so Galaxy's
`RMGE01 revision 0, setup 1` and Pikmin 2's `GPVE01 revision 0, setup 1` carry over.
`is_prepared()` accepts the marker of any of the game's discs.

`install_discs` handles a game on several discs (all of `game.discs`, any order): the first disc
goes to `port/gamedata` as `install()` puts it, each later disc N to `port/gamedata/disc<N>/{sys,files}`,
where `files` holds only what differs from the first disc's. They publish together with one marker
(one line per disc). `find_disc_images` returns every accepted image in a folder, sorted by name.

## Zstandard

`third_party/zstd` is Zstandard v1.5.6 (BSD-3-Clause) as a submodule. Only `lib/common` and
`lib/decompress` are built, into the static `starlet_zstd`, because some handheld firmware ships no
libzstd. The tests also build `lib/compress` to write their images.

## Verification status

`tests/disc` builds small GameCube and Wii discs in memory and writes them as ISO, WIA and RVZ with
valid hashes (`disc_fixture.hpp`).

Real images read the same as their plain copies: Luigi's Mansion (USA) and Super Mario Sunshine
(USA) as stored RVZ match their ISOs over all 1,459,978,240 bytes, and the Pikmin 2 (USA) stored
RVZ hashes to its Redump SHA-1 `63a68c656d654388e096d27a768e85823dcf3327`. `install()` of that RVZ
wrote Pikmin 2's gamedata exactly as its manifest lists it (2,768 files plus the five system files,
Shift-JIS names escaped) with the marker `GPVE01 revision 0, setup 1`. No real Wii or Zstandard
image has been read through this module yet.

# Memory cards

## Storage

One folder per card slot and region, Dolphin's layout: `<saves>/USA/Card A`, `<saves>/EUR/Card A`,
`<saves>/JAP/Card A` (`folder_for(saves, region_of(game_id[3]), slot)`; the region comes from the
fourth character of the game ID: E, P, J). Copy the files to and from Dolphin's
`GC/<region>/Card A` folder to move saves between them.

- `*.gci`: one file per card file, the card's 64-byte big-endian directory entry followed by the
  file's 8 KiB blocks. New files get Dolphin's names, `<company>-<game>-<file name>.gci`
  (`01-GPVE-Pikmin2_SaveData.gci`), numbered when taken, with bytes file systems reject written as
  `__xx__`. A GCI whose size disagrees with its entry's length is rejected.
- `card.txt`: the card's identity, `format_time <16 hex digits>` (OSTime ticks, 40.5 MHz since
  2000) and `encoding <0|1>` (0 ANSI, 1 Shift JIS). A missing or damaged `card.txt` is a freshly
  formatted card in the console's encoding, written at once. Sub-second ticks keep two formats in
  one second apart.

A load skips, with one log line each, files that are not GCIs (size and entry disagree), a second
copy of a file (same game, company and name), and files beyond 127 entries or the card's blocks.
The first block in a GCI's entry is whatever the card had; mounts and images assign their own.

Writes replace a file atomically: a temporary file, synced, renamed over the old one, then the
folder synced (file systems that refuse to sync a directory, such as exFAT, keep the rename). A
power cut leaves either file. A deleted file moves to `removed-<date>-<time>/`, a format moves
every GCI to `formatted-<date>-<time>/`; moving them back restores them.

## Directory entry and card contents

The 64-byte directory entry is kept as the card stores it (big-endian): game (4 characters),
company (2), name (up to 32, NUL-padded), length in blocks at 0x38. `size_mbits` counts a card in
megabits; user blocks are 16 per megabit less the 5 system blocks (ID, two directories, two
allocation tables). Memory Card 59 is 4 Mbit, 251 is 16, 1019 is 64.

A card is a Memory Card 251 (16 Mbit) unless the port asks for another size (4 to 128 Mbit). It is
always formatted: the SDK never sees an unformatted, broken or "other console" card. Its ID follows
CARDFormat:

- the 32-byte serial: 12 bytes from the slot's SRAM flash ID and a linear congruence seeded with
  the format time (a signed 64-bit congruence shifted right arithmetically, as CARDFormat and
  VerifyID compute OSTime), then the format time, SRAM counter bias, SRAM language and VI DTV
  status;
- device ID 0, the size in megabits, the encoding (CARD_ENCODE_*; the console's and the card's
  must agree);
- the checksums of `__CARDCheckSum` (16-bit big-endian sums and inverse sums, 0xFFFF stored as 0).

`Console` holds what the emulated console has: each slot's flash ID, counter bias, language, DTV
status and font encoding. The card's serial is derived from the flash ID, so VerifyID accepts the
card on a console whose SRAM holds that flash ID with its check byte (`flash_id_check`, the
complement of the flash ID's byte sum). `card_time_now()` is seconds since 2000-01-01 in local time.

## SDK level

`Card` implements the CARD calls on a `Folder` with the SDK's results and argument checks: mount,
unmount, check, format, free blocks, serial number, open, fast open, create, read, write, get and
set status, get and set attributes, delete (by name or number) and rename. The owner is the boot
disc's game and company codes (DVDDiskID): its files are writable (`__CARDAccess`; the SDK's
permission mask leaves the global and company attributes without effect), and other games' files
are readable when their public attribute is set (`__CARDIsReadable`). Calls are synchronous and not
thread-safe. A port serializes them and models the SDK's asynchronous completion, its control-block
rules (busy, attached) and callbacks itself; Pikmin 2's `src/platform/card.cpp` runs requests that
move data on one service thread and completes them as EXI interrupts.

- `mount` reads the folder: `ready`, or `encoding` for a card in the other font encoding (the game
  then offers to format it), or `io_error`. `check` reports `ready` or `encoding`.
- Reads and writes go to memory and, for writes, to the file before the call returns. Reads take
  512-byte multiples (`CARD_SEG_SIZE`), writes 8 KiB ones. The icon data must start below
  `CARD_READ_SIZE` (512).
- A new file reads as zeros (the SDK leaves whatever its blocks held). Its entry is the SDK's: no
  banner or icon, icon 0 at fast speed (`CARD_STAT_SPEED_FAST`, END for the rest), public, never
  copied, comment address 0xFFFFFFFF.
- `get_status` fills the banner, icon and data offsets as UpdateIconOffsets does (the SDK fills the
  offsets of the status it was given); `set_status` changes the banner, icons and comment address
  and stamps the time. `set_attributes` may change only the bits in 0x1C.
- After a read or write the position is `__CARDSeek` plus the SDK's transfer loop, which leaves it
  on the last sector it touched.
- `format` empties the card and gives it a new format time, hence a new serial.

### The SDK's CARD calls on starlet::os

`starlet::card_sdk` (`sdk_card.cpp`, Linux) is the CARD binding for ports on `starlet::os`: the
functions of `starlet/sdk/card.h` with C linkage, so a decompilation's calls bind by name; the port
`static_assert`s that its `CARDFileInfo` and `CARDStat` match `starlet::sdk`'s.
`initialize_cards(saves_dir)` names the saves folder before the game starts; `CARDInit` inserts slot
A, Dolphin's folder for the boot disc's region (`boot_info_block`), a card of the requested size,
formatted, created if missing. Slot B stays empty. An empty `saves_dir`, or a folder that cannot be
created, leaves slot A empty and the game plays without saving. Called again after `CARDInit`
(tests), it inserts the new folder at once.

It adds the SDK's control-block rules to `Card` (`__CARDGetControlBlock`/`__CARDPutControlBlock`:
`CARD_RESULT_NOCARD` when not mounted, `CARD_RESULT_BUSY` while a request is in flight) and timing:
requests that move data (mount, check, format, create, read, write, set status and attributes,
delete, rename) run in order on one service thread (`starlet-card`, the EXI channel) and complete
through `raise_interrupt`, with the callback on the interrupt thread; the synchronous calls sleep
the guest thread on the slot's queue as `__CARDSync` does. The channel's last result is
`CARD_RESULT_BUSY` while a request runs. Guest memory is read when a request is made
(`before_dma_read`) and written when it completes (`before_dma_write`, `report_cpu_write`), on
guest threads. Card state belongs to guest threads; the service thread never destroys it, since it
may still run at exit. `finish_card_writes(timeout)` is the exit path's wait for requests already
made; it returns false when some are still running. `tests/card/sdk_card_tests.cpp` runs the calls
on the guest CPU.

Pikmin 2 keeps its own copy of this binding (on its own OS layer) until it moves to `starlet::os`.

## Hardware level

`Folder::image()` builds a formatted card image of the folder's files: the ID, both directory
copies (check codes 0 and 1, as CARDFormat leaves them) and both block allocation tables, files in
contiguous blocks in name order. It returns nothing when the files do not fit (more than 127, or
more blocks than the card has), with one log line per file left out. An EXI model serves it and
sets the console SRAM's flash ID for the slot to `Console::flash_id[slot]` with its check byte.

`Folder::store_image()` writes a changed image back after the game wrote it: `parse_image` verifies
it as CARDCheck does (ID checksum and flash ID, then the directory and allocation table copies by
checksum and free count, the newer of two valid copies with check codes compared as s16, a damaged
copy replaced by the other), follows each file's block chain, saves the files that changed, moves
files the image no longer holds to `removed-<date>-<time>/`, and records a new format time and
encoding when the game formatted the card. The result is `ready`, `broken`, `encoding` or `no_card`
(unformatted: all bytes 0xFF). An image that does not verify (unformatted, broken, mid-erase) leaves
the folder unchanged; the model stores again after the game's next write. An encoding mismatch does
not stop the store: the files are what matter there.

`Folder::import_image()` brings in a raw card image formatted on any console (a model's earlier
`.raw` file, Dolphin's `.gcp`) once: `flash_id_of` recovers the flash ID it was formatted with
(VerifyID inverted), its files replace same-named ones in the folder, and the folder keeps its own
identity. A card formatted on the device carries a new format time with the same flash ID.

### Adopting it in an EXI card model (gc-recomp)

1. Per slot, a `Folder(folder_for(saves, region, slot), slot, console, size)`;
   `console.flash_id[slot]` and `flash_id_check` go into the IPL SRAM model.
2. The card model's image comes from `image()` instead of a raw file; an existing raw file goes
   through `import_image()` first and is then kept as a backup.
3. Where the model flushed its raw file, it calls `store_image()` with the current image.

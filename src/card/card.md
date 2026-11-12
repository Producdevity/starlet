# Memory cards

`starlet::card` (`starlet_card`) keeps GameCube memory cards as Dolphin GCI
folders and serves them two ways: the CARD calls of source ports (SDK level) and
whole card images for EXI card models (hardware level). C++17, no other
dependencies.

## Storage

One folder per card slot and region, Dolphin's layout:
`<saves>/USA/Card A`, `<saves>/EUR/Card A`, `<saves>/JAP/Card A`
(`folder_for(saves, region_of(game_id[3]), slot)`). Copy the files to and from
Dolphin's `GC/<region>/Card A` folder to move saves between them.

- `*.gci`: one file per card file, the card's 64-byte big-endian directory
  entry followed by the file's 8 KiB blocks. New files get Dolphin's names,
  `<company>-<game>-<file name>.gci` (`01-GPVE-Pikmin2_SaveData.gci`), with bytes
  file systems reject written as `__xx__`.
- `card.txt`: the card's identity, `format_time <16 hex digits>` (OSTime ticks)
  and `encoding <0|1>` (0 ANSI, 1 Shift JIS). A missing or damaged `card.txt` is a
  freshly formatted card in the console's encoding, written at once.

A load skips, with one log line each, files that are not GCIs (size and entry
disagree), a second copy of a file (same game, company and name), and files
beyond 127 entries or the card's blocks. The first block in a GCI's entry is
whatever the card had; mounts and images assign their own.

Writes replace a file atomically: a temporary file, synced, renamed over the old
one, then the folder synced (file systems that refuse to sync a directory, such
as exFAT, keep the rename). A deleted file moves to `removed-<date>-<time>/`, a
format moves every GCI to `formatted-<date>-<time>/`; moving them back restores
them.

## The card

A card is a Memory Card 251 (16 Mbit) unless the port asks for another size
(4 to 128 Mbit; user blocks are 16 per megabit less 5). It is always formatted:
the SDK never sees an unformatted, broken or "other console" card. Its ID follows
CARDFormat:

- the 32-byte serial: 12 bytes from the slot's SRAM flash ID and a linear
  congruence seeded with the format time, then the format time, SRAM counter
  bias, SRAM language and VI DTV status;
- device ID 0, the size in megabits, the encoding;
- the checksums of `__CARDCheckSum` (16-bit big-endian sums and inverse sums,
  0xFFFF stored as 0).

`Console` holds what the emulated console has: each slot's flash ID, counter
bias, language, DTV status and font encoding. The card's serial is derived from
the flash ID, so VerifyID accepts the card on a console whose SRAM holds that
flash ID with its check byte (`flash_id_check`).

## SDK level

`Card` implements the CARD calls on a `Folder` with the SDK's results and
argument checks: mount, unmount, check, format, free blocks, serial number,
open, fast open, create, read, write, get and set status, get and set
attributes, delete (by name or number) and rename. The boot disc's own files are
writable; other games' files are readable when their public attribute is set.
Calls are synchronous and not thread-safe. A port serializes them and models the
SDK's asynchronous completion, its control-block rules (busy, attached) and
callbacks itself; Pikmin 2's `src/platform/card.cpp` runs requests that move data
on one service thread and completes them as EXI interrupts.

- `mount` reads the folder: `ready`, or `encoding` for a card in the other font
  encoding (the game then offers to format it), or `io_error`.
- Reads and writes go to memory and, for writes, to the file before the call
  returns. Reads take 512-byte multiples, writes 8 KiB ones.
- A new file reads as zeros. Its entry is the SDK's: no banner or icon, icon 0
  at fast speed, public, never copied, comment address 0xFFFFFFFF.
- `get_status` fills the banner, icon and data offsets as UpdateIconOffsets
  does; `set_status` changes the banner, icons and comment address and stamps
  the time.
- `format` empties the card and gives it a new format time, hence a new serial.

### The SDK's CARD calls on starlet::os

`starlet::card_sdk` (`sdk_card.cpp`, Linux) is the CARD binding for ports on
`starlet::os`: the functions of `starlet/sdk/card.h` with C linkage, so a
decompilation's calls bind by name; the port static_asserts that its
`CARDFileInfo` and `CARDStat` match `starlet::sdk`'s. `initialize_cards(saves_dir)`
names the saves folder before the game starts; `CARDInit` inserts slot A, Dolphin's
folder for the boot disc's region (`boot_info_block`), slot B stays empty.

It adds the SDK's control-block rules (`CARD_RESULT_NOCARD` when not mounted,
`CARD_RESULT_BUSY` while a request is in flight) and timing to `Card`: requests
that move data (mount, check, format, create, read, write, set status and
attributes, delete, rename) run in order on one service thread (`starlet-card`)
and complete through `raise_interrupt`, with the callback on the interrupt thread;
the synchronous calls sleep the guest thread on the slot's queue as `__CARDSync`
does. Guest memory is read when a request is made and written when it completes
(`report_cpu_write`). `finish_card_writes(timeout)` is the exit path's wait for
requests already made. `tests/card/sdk_card_tests.cpp` (`starlet_card_sdk`) runs
the calls on the guest CPU: probe and mount rules, geometry, the created entry,
writes and reads across blocks, asynchronous completion on the interrupt thread,
persistence across re-initialization, safe writes, Dolphin GCI round trips,
other games' files, a full directory, encoding and format.

Pikmin 2 keeps its own copy of this binding (on its own OS layer) until it moves
to `starlet::os`.

## Hardware level

`Folder::image()` builds a formatted card image of the folder's files: the ID,
both directory copies (check codes 0 and 1) and both block allocation tables,
files in contiguous blocks in name order. An EXI model serves it and sets the
console SRAM's flash ID for the slot to `Console::flash_id[slot]` with its check
byte.

`Folder::store_image()` writes a changed image back after the game wrote it:
`parse_image` verifies it as CARDCheck does (ID checksum and flash ID, then the
directory and allocation table copies by checksum and free count, the newer of
two valid copies, a damaged copy replaced by the other), follows each file's
block chain, saves the files that changed, moves files the image no longer holds
to `removed-<date>-<time>/`, and records a new format time and encoding when the
game formatted the card. An image that does not verify (unformatted, broken,
mid-erase) leaves the folder unchanged; the model stores again after the game's
next write.

`Folder::import_image()` brings in a raw card image formatted on any console
(a model's earlier `.raw` file, Dolphin's `.gcp`) once: `flash_id_of` recovers the
flash ID it was formatted with, its files join the folder, and the folder keeps
its own identity.

### Adopting it in an EXI card model (gc-recomp)

1. Per slot, a `Folder(folder_for(saves, region, slot), slot, console, size)`;
   `console.flash_id[slot]` and `flash_id_check` go into the IPL SRAM model.
2. The card model's image comes from `image()` instead of a raw file; an
   existing raw file goes through `import_image()` first and is then kept as a
   backup.
3. Where the model flushed its raw file, it calls `store_image()` with the
   current image.

## Tests

`tests/card` (`starlet_card`): a Dolphin GCI fixture of Pikmin 2's save entry
(fields, name, byte-for-byte write-back, size checks, escaped names); the serial
derivation, its inversion and `CARDGetSerialNo`; images (sizes, ID, directory
and allocation table checksums checked independently, free counts, contiguous
allocation, one damaged copy repaired, both damaged broken, another console's
flash ID broken, an encoding mismatch, an unformatted card, 128 files); the SDK
level on a folder (create, write, status, reload, serial persistence, other
games' access, rename, delete kept aside, format, a damaged `card.txt`, stray
files, Shift JIS, a missing folder, Dolphin's folder names); the hardware level
(a folder's image read by the device, changed and new files stored back, a
format through the device, an erased image left alone).

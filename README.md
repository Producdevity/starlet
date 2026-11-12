# Starlet

Starlet is a source-level GameCube and Wii compatibility layer for decompilation
ports to Linux ARM handhelds: OpenGL ES 3.1 on Mali-class GPUs, the firmware's
SDL2, and about 1 GB of RAM.

It holds the GX graphics stack that the Super Mario Galaxy Linux ARM port runs on,
and the disc handling and SDL services its ports share:

- `starlet::gx` (`starlet_gx`) decodes the GX command stream (BP, CP and XF
  registers, display lists, vertex arrays) and models GX vertex processing,
  transforms, TEV, textures, rasterization, pixel operations and EFB copies.
  C++17, no other dependencies.
- `starlet::gx_gles` (`starlet_gx_gles`) draws that model with OpenGL ES 3.1.
- `starlet::disc` (`starlet_disc`) reads GameCube and Wii disc images (ISO/GCM,
  and WIA and RVZ stored or with Zstandard), parses the FST and runs the
  first-launch setup that extracts a supported disc into a port's `gamedata/`.
  C++17, with the pinned Zstandard decoder in `third_party/zstd`.
- `starlet::card` (`starlet_card`) keeps memory cards as Dolphin GCI folders and
  serves them as the SDK's CARD calls (source ports) or as formatted card images
  (EXI card models). C++17, no other dependencies. `starlet::card_sdk` binds the
  SDK's CARD functions on `starlet::os`.
- `starlet::host` (`starlet_host`) plays audio through the firmware's SDL2, reads
  its game controllers into a port's GameCube layout, and turns the frontend's
  SIGTERM into a prompt exit with the saves flushed. C++17 and SDL2.

A port feeds the command stream to a `Decoder` and gives it a `MemoryReader` for
the memory the commands refer to; [src/gx/decoder.md](src/gx/decoder.md) describes
that contract. [src/disc/disc.md](src/disc/disc.md) describes the disc formats,
their limits and the setup flow; [src/card/card.md](src/card/card.md) the card
folders, images and CARD calls; [src/host/host.md](src/host/host.md) the audio,
controller and quit contracts.

## Layout

```
include/starlet/gx/       public headers
include/starlet/gx/gles/  OpenGL ES renderer headers
include/starlet/disc/     disc image, FST and setup headers
include/starlet/card/     memory card headers
include/starlet/host/     audio, controller and quit headers
src/gx/                   GX model, with a note per module
src/gx/gles/              OpenGL ES renderer
src/disc/                 disc images and setup
src/card/                 memory cards
src/host/                 SDL audio, controllers and quit
tests/gx/                 host tests
tests/disc/               disc host tests
tests/card/               memory card tests
tests/host/               audio, controller and quit tests
third_party/zstd/         Zstandard v1.5.6 (submodule)
```

## Building

```sh
git submodule update --init third_party/zstd
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

The renderer builds when OpenGL ES 3.1 headers are found (`STARLET_GLES_INCLUDE_DIR`);
its tests run against a recording GL fake. A device build sets `-DSTARLET_GLES=ON`,
which also links the target's `libGLESv2`. `starlet::host` builds when SDL2 is
found (`STARLET_SDL2_INCLUDE_DIR`, `STARLET_SDL2_LIBRARY`); `-DSTARLET_HOST=OFF`
leaves it out.

## Using Starlet in a port

```cmake
set(STARLET_GLES ON)
add_subdirectory(path/to/starlet)
target_link_libraries(port PRIVATE starlet::gx_gles starlet::card starlet::host)
target_link_libraries(port-setup PRIVATE starlet::disc)
```

`starlet::disc` links Zstandard statically, so the setup tool runs on firmware
without libzstd. A port's setup describes its supported discs in a
`starlet::disc::Game` (title, game IDs, revisions, `main.dol` SHA-1s, optional
manifests) and calls `install()`; port-specific outputs, such as assets carved
from `main.dol`, go in the setup hook.

`starlet::host` gives a port its SDL services:

- `AudioOutput::open_pull` for an AI DMA model that the device's clock drives,
  `open_push` and `write()` for one that produces on its own clock;
- `Controllers` and `to_gamecube()` with the port's `GameCubeMapping` (face
  layout, Z and trigger sources, threshold, stick dead zones) for PAD or the SI
  model; a Wii port maps the `Gamepad` itself;
- `install_quit_signals()`, `poll_events()`, `quit_requested()`, `on_quit()`,
  `exit_now()` and `exit_after_quit()`: the port flushes its saves and exits
  within 1 s of SIGTERM.

An embedding project builds Starlet's tests only with `STARLET_TESTS` set.

## License

Starlet is licensed under the GNU General Public License, version 3. See
[LICENSE](LICENSE).

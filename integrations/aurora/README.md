# Starlet as the renderer of an Aurora project

Starlet draws Aurora's GX command stream with its own OpenGL ES 3.1 renderer. A small
bridge inside Aurora's source tree hands each drained FIFO buffer to Starlet, runs
Starlet's GL work on Aurora's render worker and presents Starlet's frame into Aurora's
present target. Aurora keeps its window, input, UI and audio. `AURORA_RENDERER=starlet`
selects Starlet at run time; without it Aurora renders as before.

**Which Aurora.** The bridge hooks Aurora's OpenGL ES backend (`lib/gl/`, `gl::gl`, the
EFB present path): the GLES port that Dusklight uses on PortMaster. Upstream
encounter/aurora renders through WebGPU/Dawn and has no GL context for Starlet to share,
so the bridge does not build there. The bridge was written against bmdhacks/aurora at
`3fbacee`, the GLES fork that Dusklight's PortMaster build pins, which sits on
encounter/aurora `f8573d3`.

## 1. What Starlet is, and when it helps

Starlet is a GameCube and Wii compatibility layer for Linux ARM handhelds, licensed
GPL-3.0. Its GX part decodes the hardware command stream (BP, CP and XF registers,
display lists, vertex arrays) and models vertex processing, TEV, textures, rasterization
and EFB copies. Its GLES part draws that model with OpenGL ES 3.1. It targets Mali-class
GPUs and about 1 GB of RAM: draws are batched, EFB copies stay on the GPU, and shader
programs are built in the background.

Measured with Dusklight (Twilight Princess) against Aurora's own GLES renderer, on an
Anbernic RG35XX H (Allwinner H700, Mali-G31 MP2) running muOS. Both renderers drew the
game's 608x448 frame (the fork at internal scale 0.95). Lockstep runs show the same
frames of the first attract cutscene on both; two runs each, 2026-11-10:

| Scene | Aurora GLES fork | Starlet |
|---|---|---|
| Cutscene frames 100-2400, ms per frame | 42.2, 42.3 | 38.2, 38.3 |
| Render-heavy frames 600-900, ms per frame | 53-58 | 38-41 |
| 95 s free-running, mean fps | 23.7 | 26.3 |
| 95 s free-running, worst 5 s window | 17.2 | 21.3 |
| Title scene, frames 1700-2400, ms per frame | about 44.5 | about 46 |

The title scene is bound by the game's own shadow code (about a third of the main thread
under both renderers), and Starlet is a few percent slower there. In a 13.7 minute play
session on the same device (2026-11-15), Starlet held 29.8 fps on average at 608x448; the
game caps at 30.

The same output was checked on an RG40XX H running ROCKNIX: it booted, played the attract
movies correctly and quit cleanly.

Not measured: PowerVR GPUs, desktop GPUs, any other Aurora game, and upstream Aurora on
Dawn. Treat Starlet as a candidate for a Mali handheld port and measure your game on the
device before relying on it.

## 2. Requirements

- An Aurora tree with the GLES backend, as described above.
- An OpenGL ES 3.1 context current on Aurora's render worker. Starlet checks the version
  and refuses to start below 3.1.
- A game frame at most 640 wide and 528 high, with an even height. The bridge logs
  `Starlet needs an EFB frame of at most 640x528; the game uses WxH` and draws nothing
  otherwise.
- A game that renders through Aurora's GX SDK (`lib/dolphin/gx`). Starlet draws what
  arrives in the GX FIFO.
- CMake 3.21, a C++17 compiler, the `GLES3/gl31.h` headers and `libGLESv2` in the target
  sysroot.

If Starlet fails to start, the bridge logs `Starlet renderer failed to start: <reason>`
and the run draws nothing. It does not fall back to Aurora's renderer.

## 3. Add Starlet to the build

Add Starlet as a submodule and pin it:

```sh
git submodule add https://github.com/Producdevity/starlet extern/starlet
git -C extern/starlet checkout <commit>
git submodule update --init --recursive extern/starlet
```

Pin a commit that contains this folder (`integrations/aurora/`). The bridge calls
Starlet's renderer API, so the two change together. The bridge was built and compiled
against Starlet `85f9836` plus the commit that added this folder (`git log -- integrations/aurora`).

Starlet configures all its modules, so its `third_party/zstd` submodule must be present
even though only the renderer is built. The bridge links `starlet::gx_gles`; the other
modules are excluded from the build. The SDL2 module configures only when SDL2 is found;
`-DSTARLET_HOST=OFF` leaves it out.

The patch adds this to `cmake/aurora_gx.cmake`:

```cmake
set(AURORA_STARLET_DIR "" CACHE PATH "Starlet source tree")
if (NOT EXISTS "${AURORA_STARLET_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set AURORA_STARLET_DIR to a Starlet checkout")
endif ()
set(STARLET_GLES ON)
add_subdirectory("${AURORA_STARLET_DIR}" "${CMAKE_BINARY_DIR}/starlet" EXCLUDE_FROM_ALL)
target_sources(aurora_gx PRIVATE
        lib/starlet_backend/aurora_stream.cpp
        lib/starlet_backend/backend.cpp
        lib/starlet_backend/host_memory.cpp
        lib/starlet_backend/issues.cpp
        lib/starlet_backend/texture_convert.cpp
)
target_link_libraries(aurora_gx PRIVATE starlet::gx_gles)
```

This makes Starlet a required dependency of `aurora_gx`. Configure with
`-DAURORA_STARLET_DIR=<path to extern/starlet>`. `STARLET_GLES=ON` makes Starlet link the
target's `libGLESv2` and stop configuring when the GLES 3.1 headers are missing
(`STARLET_GLES_INCLUDE_DIR`). Starlet's tests stay off when it is built as a subproject.

## 4. Add the bridge to Aurora

The bridge is in `starlet_backend/` next to this file. `aurora-hooks.patch` holds every
Aurora change below and applies to bmdhacks/aurora `3fbacee`. From your project root:

```sh
cp -r extern/starlet/integrations/aurora/starlet_backend extern/aurora/lib/starlet_backend
git -C extern/aurora apply "$PWD/extern/starlet/integrations/aurora/aurora-hooks.patch"
```

The bridge consists of:

| File | Role |
|---|---|
| `backend.cpp`, `backend.hpp` | Creates the renderer, feeds it, submits and presents frames |
| `aurora_stream.cpp`, `aurora_stream.hpp` | Reads Aurora's GX FIFO dialect (`GX_AURORA_*` commands) and writes the hardware registers Starlet's decoder expects |
| `host_memory.cpp`, `host_memory.hpp` | Gives host pointers the 32-bit GX addresses the decoder works in |
| `texture_convert.cpp`, `texture_convert.hpp` | Tiles Aurora's linear PC texture formats into GX layouts |
| `issues.cpp`, `issues.hpp` | The `[starlet-issue]` log |

The hooks below are what the patch contains. On another Aurora revision, apply them by
hand. Each is a few lines. Every hook sits behind `starlet_backend::enabled()`, which is
true only when `AURORA_RENDERER=starlet`.

### Drain, frame end, present, shutdown

In `lib/gx/fifo.cpp`, `drain()` decodes the buffer on the main thread. Starlet takes it
instead of Aurora's command processor:

```cpp
  if (starlet_backend::enabled()) {
    starlet_backend::process(detail::sBufferData, detail::sBufferSize);
  } else {
    process(detail::sBufferData, detail::sBufferSize, true);
  }
```

In `lib/aurora.cpp`, `end_frame()` submits the frame's GL work after the last drain:

```cpp
  gx::fifo::drain();
  if (starlet_backend::enabled()) {
    starlet_backend::end_frame();
  }
```

The end-of-frame callback runs on the render worker. Where it calls
`webgpu::present_frame`, present Starlet's frame first. The device branch passes the
slot's framebuffer, the desktop branch passes `0`:

```cpp
        if (!starlet_backend::enabled() ||
            !starlet_backend::present(targetFbo, webgpu::present_surface_width(), webgpu::present_surface_height())) {
          webgpu::present_frame(targetFbo);
        }
```

`present` draws into the target at the viewport Aurora calculates for the frame's aspect
ratio and resets Aurora's GL state cache (`gl::reset_state_cache()`), so RmlUi and ImGui
composite on top as before. Starlet draws at the game's frame size; the bridge does not read Aurora's
internal-resolution setting.

In `aurora::shutdown()`, before `gfx::shutdown()`:

```cpp
  starlet_backend::shutdown();
```

### Texture loads and display lists

Starlet decodes the frame when its FIFO drains, after the game may have rewritten memory a
texture load named, such as a font's glyph page. `texture_data` copies pages the game
wrote this frame, so each draw reads the texture as it was loaded. In
`emit_loaded_texobj_metadata` (`lib/dolphin/gx/GXTexture.cpp`):

```cpp
  const void* data = obj.data;
  if (aurora::starlet_backend::enabled()) {
    data = aurora::starlet_backend::texture_data(
        data, GXGetTexBufferSize(static_cast<u16>(obj.width()), static_cast<u16>(obj.height()), obj.format(), obj.has_mips(), 11));
  }
  GX_WRITE_AURORA(GX_AURORA_LOAD_TEXOBJ);
  GX_WRITE_U8(static_cast<u8>(id));
  GX_WRITE_U64(reinterpret_cast<u64>(data));
```

`GXCallDisplayList` copies the list into the FIFO. This hook names the list's source and
its write stamp, so Starlet recognises an unchanged list without hashing its bytes. It
affects speed, not the picture. In `lib/dolphin/gx/GXDispList.cpp`:

```cpp
  if (!aurora::gx::fifo::in_display_list()) {
    aurora::starlet_backend::note_display_list(data, nbytes);
  }

  // Write display list contents to the FIFO
  aurora::gx::fifo::write_data(data, nbytes);
```

### Copy filter and viewport origin

Starlet follows the hardware registers. Two things in Aurora's emitters differed from the
SDK and changed the picture, so fix them in the tree.

`GXSetCopyFilter` was empty. Starlet applies the vertical copy filter, and with BP `0x53`
and `0x54` left at zero it zeroed the frame copy (a black disc where Twilight Princess
draws the sun). Emit the registers as the SDK does (`lib/dolphin/gx/GXFrameBuffer.cpp`):

```cpp
void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {
  // BP 0x01-0x04: three sample locations each (x, y in 4 bits); 6 is the pixel centre.
  for (u32 reg = 0; reg < 4; ++reg) {
    u32 value = 0;
    for (u32 i = 0; i < 3; ++i) {
      const u32 x = aa ? sample_pattern[reg * 3 + i][0] : 6;
      const u32 y = aa ? sample_pattern[reg * 3 + i][1] : 6;
      SET_REG_FIELD(0, value, 4, i * 8, x);
      SET_REG_FIELD(0, value, 4, i * 8 + 4, y);
    }
    SET_REG_FIELD(0, value, 8, 24, 0x01 + reg);
    GX_WRITE_RAS_REG(value);
  }
  // BP 0x53/0x54: seven 6-bit vertical filter coefficients; without vf the hardware
  // filter is 0,0,21,22,21,0,0, which leaves every row unchanged.
  static constexpr u8 passthrough[7] = {0, 0, 21, 22, 21, 0, 0};
  const u8* coefficients = vf ? vfilter : passthrough;
  u32 coeff0 = 0, coeff1 = 0;
  for (u32 i = 0; i < 4; ++i) SET_REG_FIELD(0, coeff0, 6, i * 6, coefficients[i]);
  for (u32 i = 0; i < 3; ++i) SET_REG_FIELD(0, coeff1, 6, i * 6, coefficients[4 + i]);
  SET_REG_FIELD(0, coeff0, 8, 24, 0x53);
  SET_REG_FIELD(0, coeff1, 8, 24, 0x54);
  GX_WRITE_RAS_REG(coeff0);
  GX_WRITE_RAS_REG(coeff1);
  __gx->bpSent = 1;
}
```

The viewport origin bias is 342 on hardware and in the SDK. The Aurora tree used 340,
which put the picture 2 px off. Change the emitter, and the decoder in Aurora's own command
processor so both renderers agree:

- `GXSetViewportJitter` (`lib/dolphin/gx/GXTransform.cpp`): `ox = 342.0f + (left + width / 2.0f);` and the same for `oy`.
- `handle_xf` (`lib/gx/command_processor.cpp`): `.left = ox - 342.0f - width / 2.0f` and the same for `.top`.

### Game side

The game tells Starlet when its CPU writes memory the GPU reads. Starlet advances the
generation of cached vertex arrays and textures on these reports; a game that rewrites
texture or array data in place without them shows stale geometry and textures. Dusklight's
data-cache stubs (`src/dusk/stubs.cpp`) call:

```cpp
extern "C" void aurora_starlet_note_write(const void* pointer, unsigned size);

void DCStoreRange(void* addr, u32 nBytes) {
    aurora_starlet_note_write(addr, nBytes);
}
```

`DCFlushRange`, `DCFlushRangeNoSync`, `DCInvalidateRange` and `DCStoreRangeNoSync` do the
same.

Starlet decodes the game's original display lists. If the game runs Aurora's display list
optimizer (`aurora::gx::dl::optimize`), keep the original list when Starlet is selected.
Dusklight's `J3DShapeDraw` constructor does:

```cpp
    static const bool starlet = [] {
        const char* renderer = std::getenv("AURORA_RENDERER");
        return renderer && std::strcmp(renderer, "starlet") == 0;
    }();
    if (starlet) {
        set_display_list_copy(mDisplayList, mDisplayListSize, displayList, displayListSize);
        return;
    }
```

## 5. Select it and check it runs

```sh
AURORA_RENDERER=starlet ./yourgame
```

The log starts with `Starlet draws the GX stream: frame WxH`. Every 5 seconds the bridge
logs `[starlet] per frame:`, `[starlet] stream per frame:` and `[starlet] totals:` lines.
In the totals, `rejected`, `failed_copies` and `unmapped` should stay at 0 in a healthy
run; when `rejected` is nonzero, the line ends with each rejection's reason and count.

The bridge keeps a shader program cache (linked programs and a `programs.list`) in a
`starlet` folder under `AuroraConfig::cachePath`; `data_directory()` in `backend.cpp`
names it. On the first run, programs missing from the cache
are built in the background and their draws are skipped until they are ready, so the first
minutes show missing geometry on a handheld. Keep the cache between runs.

## 6. What the bridge handles

Aurora's FIFO carries hardware BP, CP and XF commands, draws, display lists inlined by
`GXCallDisplayList`, and `GX_AURORA_*` sub-commands from `GXAurora.h`. Starlet's decoder
takes the hardware part directly; the bridge translates the extensions:

| Aurora command | Bridge |
|---|---|
| `LOAD_ARRAYBASE` | Maps the array pointer to a 32-bit address and writes the CP array base. Honors the little-endian flag. Matrix arrays resolve when an indexed XF load uses them |
| `LOAD_TEXOBJ`, `LOAD_TLUT` | Maps the data and writes the image and TLUT registers. Textures over 1024 texels are supported |
| `LOAD_COPY_SRC`, `LOAD_COPY_DST`, `LOAD_COPY_DEST` | The BP copy trigger becomes an EFB copy into a GPU-resident texture named by the destination pointer |
| `BEGIN_OFFSCREEN`, `END_OFFSCREEN` | Starlet's offscreen pass |
| `DRAW_SIZED`, `DRAW_INDEXED` | Expanded into hardware-style vertex data |
| `DEBUG_GROUP_*`, `DESTROY_*` | Consumed with no effect |
| `LOAD_VIEWPORT_RENDER`, `LOAD_SCISSOR_RENDER`, `LOAD_PROJECTION_FULL` | Ignored and counted. The hardware viewport, scissor and projection registers drive the draw |
| `REQUEST_DEPTH_SNAPSHOT` | Ignored and logged: effects that read depth back see stale values |
| `GX2_SET_POLYGON_OFFSET` | Ignored and logged: coplanar decals may z-fight |
| A display list call that names a host pointer (nested lists) | Ignored |
| Any other command | Stream failure: `Starlet stopped following the GX stream: <reason>` is logged once and Starlet decodes nothing more in that run |

At the start of each frame the bridge clears the EFB colour, alpha and depth to the
copy-clear values, as Aurora's own renderer does.

### PC texture formats

Aurora adds four PC-only texture formats. Starlet reads the image register's low four
bits, so a PC format needs its data in the matching GX tiled layout.

| Format | Bridge |
|---|---|
| `GX_TF_R8_PC` | Handled. Linear bytes are tiled as I8 on every load (Dusklight's movie planes use it) |
| `GX_TF_RGBA8_PC` | Handled. Linear RGBA bytes are tiled as GX RGBA8 (4x4 tiles, the A,R pairs and then the G,B pairs) |
| `GX_TF_BC1_PC` | Handled. Linear BC1 blocks are rearranged into CMPR (8x8 tiles of four blocks, big-endian endpoints, index pairs reversed) |
| `GX_TF_RG8_PC` | Not supported: no GX format matches. The bridge logs `PC texture format has no GX equivalent` and the map keeps its previous texture. Store such textures in a GX format |
| Any PC format with mipmaps | Not supported. The bridge logs `mipmapped or invalid PC texture not loaded` and the map keeps its previous texture |

Each conversion repeats the last row, column or block into edge tiles. `tests/aurora/` in
Starlet checks the three conversions against Starlet's own texture decoder.

### EFB copies and readbacks

Aurora keeps each EFB copy as a texture of its own, named by the destination pointer, and
never writes the game's destination memory. The bridge does the same: each destination
gets a private address range and storage, and the copy stays on the GPU. A game that reads
a copy's bytes with the CPU therefore reads memory that was never written, under Aurora's
renderer and under the bridge alike.

## 7. Write-back: copies the CPU reads

On the console an EFB copy writes main memory, and some games read it back. Starlet can
write chosen copies back. The bridge does not enable this today and no Aurora project has
used it; the steps below use the renderer's public calls (`include/starlet/gx/gles/renderer.hpp`).

- `renderer.write_back(token, bytes, on)` names a range whose copies the CPU reads. `token`
  is the copy's address in the bridge's address space (`CopyTarget::address`, from
  `HostMemory::copy_target`). Call it on the thread that records draws, before the copy is
  issued. Starlet then keeps the copy resident and sampled on the GPU, reads it
  back without waiting, and stores it as the console would. `on = false` drops the range.
- `renderer.land_write_backs(renderer.write_backs_issued(), false)` writes the copies read
  back so far, in issue order, on the same thread. With `wait = true` it blocks until all
  are written. Call it once per drain, and with `wait = true` where the CPU is about to
  read the buffer, for example when the game calls `GXDrawDone`.
- Landed bytes go to the `destination` buffer passed to `copy_texture_resident`. The
  bridge passes the copy's private storage there. For the CPU to see the bytes, pass the
  game's own destination pointer instead (the buffer must hold the copy's `bytes`).

The changes in the bridge are small: add `write_back` and `land_write_backs` to its `Efb`
class (`backend.cpp`) and `EfbTarget` interface, call `write_back` from
`AuroraStream::copy_texture` for the destinations your game lists, and pass the game's
destination pointer to `efb_.copy_texture`. Choose the ranges in game code, where the
buffer is created.

Costs, from Starlet's notes: each readback takes about 1 ms of GL thread time on Mali
whatever its size. A copy of one byte per texel (I8, whole 8x4 tiles, natural stride) is
packed on the GPU first, so a quarter of the bytes come back, and copies issued together
share a readback. Starlet's own SDK layer uses this for Super Mario Sunshine's goop maps.

## 8. Logs and issue reports

The bridge logs through Aurora's logger under `aurora::starlet`.

`[starlet-issue] HH:MM:SS frame N: <kind> (<detail>)` marks anything that can change the
picture:

- a draw Starlet rejected (the kind is the reason);
- a texture, TLUT or vertex array outside mapped memory: draws sample the previous
  texture or palette, or read wrong vertices;
- an EFB copy refused, without a destination, or with a destination outside mapped memory;
- the ignored commands in section 6 that log: depth snapshots and polygon offset.

The first line of each kind is followed by the BP, CP and XF register state of the draw
involved. A kind logs again only after 300 quiet frames (about 5 seconds at 60 fps), at
most 40 lines per kind and 400 in total; later occurrences are counted only.

The issue log cannot see a draw Starlet renders wrongly without error. Compare those
against Aurora's own renderer at the same point in the game.

Debugging switches:

- `AURORA_STARLET_TRACE=1` prints extra detail for rejected draws and texture loads.
- `STARLET_MERGE=0` turns off draw merging, to tell a merging problem from a decoding one.

To report a problem, open an issue at https://github.com/Producdevity/starlet/issues with:

1. the full log of the run;
2. the Starlet, bridge and Aurora revisions, and the game;
3. the device, its GPU and driver, and the GL renderer and version strings from the log;
4. a screenshot with the time it was taken, so it matches a `[starlet-issue]` line;
5. a screenshot of the same scene under `AURORA_RENDERER` unset, to show which renderer
   differs.

## 9. Licensing

Starlet is GPL-3.0. Aurora is MIT. The bridge links Starlet, so it is distributed under
GPL-3.0 with it.

A binary that links Starlet is a combined work under GPL-3.0. When you distribute that
binary, you must offer the complete corresponding source of the whole binary (game, Aurora,
Starlet, the bridge and your build scripts) under GPL-3.0-compatible terms. Loading
Starlet dynamically does not change this. Aurora's MIT code can be included in a GPL-3.0
program; the combined program is then GPL-3.0.

Check your game project's license:

- GPL-3.0, GPL-2.0-or-later, Apache-2.0, MIT and BSD are compatible with GPL-3.0.
- GPL-2.0-only, proprietary licenses and licenses that forbid commercial use are not. You
  cannot distribute a binary that links Starlet under them.

If your project is not compatible, ship the build without Starlet. Distributing source that
only names Starlet as an optional submodule leaves the obligation with whoever distributes
a built binary.

Game assets and discs are never part of any of this. Do not distribute them.

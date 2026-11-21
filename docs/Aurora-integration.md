# Aurora integration

Starlet can replace the renderer of an Aurora port. Aurora keeps the window, input and audio.
Starlet decodes the GX FIFO and draws it with OpenGL ES 3.1. `AURORA_RENDERER=starlet` selects it
at run time.

Code: [`integrations/aurora/`](https://github.com/Producdevity/starlet/tree/master/integrations/aurora).
It has the bridge (`starlet_backend/`) and one Aurora patch per tree.

## Aurora trees

| Aurora | Patch | Overlays under Starlet |
|---|---|---|
| encounter/aurora `5794daa` (upstream, WebGPU) | `aurora-hooks-upstream.patch` | none |
| encounter/aurora `5179721` (upstream, older) | `aurora-hooks-upstream-5179721.patch` | none |
| bmdhacks/aurora `3fbacee` (OpenGL ES fork, Dusklight) | `aurora-hooks-gles-fork.patch` | RmlUi and ImGui keep working |

Other upstream revisions need the patch applied by hand; the changes are listed below.

The bridge detects the tree from `GXAurora.h`. `aurora_commands.hpp` has every command id and
payload size and `static_assert`s the ids against the header, so a renumbered command fails the
build. A stream command the bridge does not know stops decoding for the run.

## Requirements

| | |
|---|---|
| Starlet | the revision that has `integrations/aurora/` for your tree |
| GL | OpenGL ES 3.1 |
| Frame | EFB at most 640x528, even height |
| Build | CMake 3.21, C++17, `GLES3/gl31.h` and `libGLESv2` in the sysroot |

## Build: upstream Aurora

```sh
git submodule add https://github.com/Producdevity/starlet extern/starlet
git -C extern/starlet checkout <commit>
git submodule update --init --recursive extern/starlet
git -C extern/aurora am "$PWD/extern/starlet/integrations/aurora/aurora-hooks-upstream.patch"
cmake -S . -B build -DAURORA_STARLET_DIR="$PWD/extern/starlet" -DSTARLET_HOST=OFF
```

- Without `AURORA_STARLET_DIR` the hooks compile to inert stubs and Aurora builds as before.
- Starlet's `third_party/zstd` submodule must be checked out.
- The bridge calls Starlet's renderer API, so update both together.
- Dawn stays linked but is never initialised. A target with a libstdc++ older than the one Dawn's
  prebuilt library uses needs Dawn built from source.
- Shared game modules: do not link the executable with `-Wl,--exclude-libs,ALL`. It hides Aurora's
  symbols from the modules, and each module then carries its own copy of Aurora.

### Aurora hooks (upstream)

All of them are gated by `starlet_backend::enabled()`.

| Location | Change |
|---|---|
| `cmake/aurora_gx.cmake` | Adds the bridge sources and `starlet::gx_gles`, or the stubs in `lib/starlet_stub` |
| `lib/window.cpp` | Window with `SDL_WINDOW_OPENGL` and ES 3.1 attributes; no WebGPU surface |
| `lib/aurora.cpp`, init | Skips Dawn, `gfx::initialize`, the render worker, ImGui and RmlUi |
| `lib/gx/fifo.cpp`, `drain()` | `starlet_backend::process()` replaces Aurora's command processor; FIFO in drain mode |
| `lib/aurora.cpp`, `end_frame()` | Drain, `starlet_backend::end_frame()`, present, return |
| `lib/aurora.cpp`, shutdown | `starlet_backend::shutdown()` before the rest |
| `lib/imgui.cpp`, `process_event` | Returns when no ImGui context exists (`5179721`) |
| `GXTexture.cpp`, `emit_loaded_texobj_metadata` | `texture_data()` snapshots texture pages the game wrote this frame |
| `GXDispList.cpp`, `GXCallDisplayList` | `note_display_list()` lets Starlet skip hashing unchanged lists |
| `GXFrameBuffer.cpp`, `GXSetCopyFilter` | Emits BP `0x01`-`0x04` and `0x53`/`0x54` like the SDK. Aurora's version is empty, and zero filter coefficients black out copies. |
| `GXTransform.cpp` | Viewport origin bias 342 (hardware and SDK) instead of 340 |

The SDL window belongs to Aurora. The bridge creates the GL context on the main thread, then a
bridge thread owns it, runs the renderer and swaps. `AURORA_STARLET_VSYNC=1` sets swap interval 1;
the default is 0.

Aurora's `GXSetScissorBoxOffset` writes BP `0x59` with the wrong sign: field 853 where the console
writes 171. The bridge rewrites it. Without that, every draw falls outside the scissor and only the
frame-start clear colour reaches the screen.

Under Starlet the port has no launcher, ESC menu or overlays, and no texture replacement. A port that
requires RmlUi or ImGui at startup needs a change to start without them; pass the disc image on the
command line.

## Build: GLES fork

```sh
cp -r extern/starlet/integrations/aurora/starlet_backend extern/aurora/lib/starlet_backend
git -C extern/aurora apply "$PWD/extern/starlet/integrations/aurora/aurora-hooks-gles-fork.patch"
cmake -S . -B build -DAURORA_STARLET_DIR="$PWD/extern/starlet" -DSTARLET_HOST=OFF
```

`aurora_gx` then requires Starlet and links `starlet::gx_gles`. The patch hooks `fifo.cpp`,
`end_frame()`, the render worker's end-of-frame callback (`present(fbo, w, h)`), shutdown, and the
texture, display list, copy filter and viewport hooks above. Aurora's render worker owns the GL
context.

## Game changes

These are not in the patches.

Report CPU writes to memory the GPU reads. Without them, textures and vertex arrays the game
rewrites in place render stale:

```cpp
extern "C" void aurora_starlet_note_write(const void* pointer, unsigned size);

void DCStoreRange(void* addr, u32 nBytes) { aurora_starlet_note_write(addr, nBytes); }
// Same for DCFlushRange, DCFlushRangeNoSync, DCInvalidateRange, DCStoreRangeNoSync.
```

If the game calls `aurora::gx::dl::optimize`, skip it when `AURORA_RENDERER=starlet`, because
Starlet decodes the original display lists. Dusklight does this in the `J3DShapeDraw` constructor.
A code search found no call in Animal Crossing or Mario Party 4.

## Running

```sh
AURORA_RENDERER=starlet ./game
```

- The first line is `Starlet draws the GX stream: frame WxH`. On a startup failure the log
  says `Starlet renderer failed to start: <reason>` and nothing is drawn. There is no
  fallback.
- Stats are logged every 5 s (`[starlet] per frame:`, `stream per frame:`, `totals:`).
  `rejected`, `failed_copies` and `unmapped` are 0 in a clean run.
- Shader cache: `<AuroraConfig::cachePath>/starlet/`. Missing programs build in the
  background, and their draws are skipped until they are ready. Keep the cache between runs.
- Upstream Aurora's Dawn path has no surface on KMS/DRM or the muOS SDL shim, so ports built on it
  have no non-Starlet renderer on those handhelds.

### Testing on a host

A Linux container with Mesa's llvmpipe (GLES 3.2), SDL2, Xvfb and gdb runs an aarch64 build. Use a
640x480 X window; a larger window changes the frame size. Give the container a null ALSA device,
otherwise a game that waits for its audio driver never starts drawing. Dump frames with an
`LD_PRELOAD` wrapper on `SDL_GL_SwapWindow` and `glReadPixels`. A stripped binary hides crashes:
keep an unstripped build and use gdb.

### Overlay (upstream trees)

[sbc-port-overlay](https://github.com/Producdevity/sbc-port-overlay) adds a settings menu and an
FPS, load and memory readout. Build with `-DSTARLET_OVERLAY_DIR=<sbc-port-overlay checkout>`;
without it the hooks do nothing.

- `STARLET_OVERLAY_NAME` names the port in the menu. Settings live in `conf/overlay.ini`, relative
  to the working directory.
- The bridge draws the overlay on its GL thread after the frame and before `SDL_GL_SwapWindow`.
- Aurora's event loop offers each gamepad event to the overlay first. While its menu is open, the
  game does not see the event.
- Before `spo_frame` the bridge rebinds framebuffer 0, `GL_BACK` and texture unit 0, because the
  overlay expects the window's default state and Starlet's last pass leaves other bindings.

### Skipping frames

A port that runs two game ticks per presented frame calls `aurora_starlet_skip_frame()` after
`aurora_begin_frame` in the first tick. That frame's GX state still updates and its draws are
recorded, so textures, programs and geometry stay in step, but the GL thread does not render the
draws and the frame is not presented. A frame that copies or reads the EFB (texture copies,
offscreen passes, peeks) or fills a segment renders fully, because later frames may sample what it
took from the EFB; it is not presented either. The log's `[starlet] rates:` line gives ticks,
presented and skipped frames per second, and how many skipped frames rendered anyway.

The call changes nothing unless the port makes it.

### Menu options

`aurora_starlet_overlay_option` adds a toggle or a choice to the overlay menu (see
`backend.hpp`). `on_change` runs when the option registers, with the saved value, and on every
change, on the GL thread or the main thread; hand the value to the game thread there.

## Performance

Dusklight on an RG35XX H (H700, Mali-G31, muOS), 608x448, two lockstep runs each:

| | Aurora GLES | Starlet |
|---|---|---|
| Attract cutscene, ms/frame | 42.2 | 38.2 |
| Heaviest cutscene section, ms/frame | 53-58 | 38-41 |
| 95 s free run, mean fps | 23.7 | 26.3 |
| 95 s free run, worst 5 s, fps | 17.2 | 21.3 |
| Title scene, ms/frame | 44.5 | 46 |

The title scene is bound by the game's shadow code on the CPU. A 13.7 min play session
averaged 29.8 fps; the game caps at 30.

Upstream Aurora ports on an RG35XX SP (H700, Mali-G31, muOS 2601.0), 640x480, Starlet only:

| Game | Scene | fps |
|---|---|---|
| Animal Crossing (Nookery) | title and K.K. intro, cold shader cache | 60 (min 58.2); `gl_ms` about 16 |
| Mario Party 4 (partyboard) | logo scene | 40-41 |
| Mario Party 4 (partyboard) | file select | 60 |
| Mario Party 4 (partyboard) | title after returning, about 1300 draws per frame | 31.6; `gl_ms` about 19 |

Other GPUs and other Aurora games are untested.

## Stream coverage

| Aurora command | Handling |
|---|---|
| BP, CP, XF, draws, inlined display lists | Decoded directly |
| `LOAD_ARRAYBASE` | Mapped to a 32-bit address; honours the little-endian flag |
| `LOAD_TEXOBJ`, `LOAD_TLUT`, `SET_TEX_IMG_PTR` | Mapped; textures over 1024 texels supported. A palette above 64 MiB of GX address space, which BP `0x64` cannot hold, loads through `Decoder::load_tlut` |
| `LOAD_COPY_SRC`, `LOAD_COPY_DST`, `LOAD_COPY_DEST`, `LOAD_DISP_COPY_SRC` | EFB copy to a GPU texture keyed by the destination pointer; BP `0x52` with bit 14 is a display copy |
| `CLEAR` | Planned as a GX clear of the given rectangle |
| `BEGIN_OFFSCREEN`, `END_OFFSCREEN` | Offscreen pass |
| `DRAW_SIZED`, `DRAW_INDEXED` | Expanded to hardware vertex data |
| `SET_DRAWS_BLOCKING`, `DEBUG_GROUP_*`, `DESTROY_*` | Consumed |
| `LOAD_VIEWPORT_RENDER`, `LOAD_SCISSOR_RENDER`, `LOAD_PROJECTION_FULL` | Ignored; the hardware registers drive the draw |
| `REQUEST_DEPTH_SNAPSHOT` (fork) | Ignored and logged; depth readback effects see stale values |
| Readback commands | Not supported. Logged; the game sees no data |
| `GX2_SET_POLYGON_OFFSET` | Ignored and logged; coplanar decals can z-fight |
| Display list call with a host pointer | Ignored |
| Anything else | Logged once, then Starlet stops decoding for the run |

### Bridge internals

- Threads. Upstream trees: `process()` and `end_frame()` run on the main thread; the bridge's GL
  thread owns the context from `initialize()` on, runs the renderer and swaps, so Aurora's main
  thread issues no GL call. At most two frames wait for it, so the game cannot run ahead. GL
  objects die on that thread. GLES fork: Aurora's render worker owns the context and calls
  `present(fbo, w, h)`, which draws into the present target and leaves Aurora's GL state shadow
  invalid. Aurora's non-stencil UI passes can leave an invalid cached stencil enum pending, so
  ownership boundaries clear it and Starlet diagnoses only its own GL calls.
- Aurora subcommands. `aurora_commands.hpp` follows the tree it is built against. The fork
  (`bmdhacks/aurora 3fbacee`) uses `0x38` for `REQUEST_DEPTH_SNAPSHOT` (no payload). Upstream
  `5179721` has none of `0x38`, `0x3b` and `0x52`; `5794daa` has `0x38` as `SET_TEX_IMG_PTR`
  (u8 map, u64 pointer), `0x3b` and `0x52`. Each optional command compiles in when `GXAurora.h`
  defines its macro. `STARLET_AURORA_UPSTREAM` selects the host glue (the fork owns its GL
  context through its render worker; upstream trees do not). The bridge's own subcommand carries a
  display-list pointer, a u32 size and the list's u64 write stamp; the next size bytes of the FIFO
  are that copy.
- Addresses. Starlet's decoder works in 32-bit GX addresses. The game's MEM1 arena maps linearly
  into `[0, 512 MiB)` (addresses below the arena base stay invalid, so a null address reads
  nothing; the arena ends at the window base); other ranges get addresses from a second window,
  and every address keeps its pointer's alignment modulo 32, which texture and TLUT registers rely
  on. GX texture registers hold 29-bit addresses, hence the 512 MiB end. A palette address above
  64 MiB does not fit BP `0x64` (21 bits of 32-byte units) and loads from its bytes. A TLUT name
  maps to a TMEM line (512 bytes): `GX_TLUT0..15` hold up to 256 entries, `GX_BIGTLUT0..3` up to
  16384.
- Write tracking. `aurora_starlet_note_write` advances the write stamp of arena blocks (1 KiB)
  and the generation of every big-endian array and loaded texture the range touches. Host-order
  arrays, which PC code writes without reporting, are hashed once per feed (the game does not run
  while a FIFO drains) and their generation advances when the hash changes; arrays above a size
  limit keep generation zero. A texture rewritten during the frame is copied at load
  (`texture_data()`), so each draw reads it as it was loaded.
- EFB copies. Aurora keeps each copy as a texture named by its destination pointer, and PC code
  can copy the frame at full size into a buffer it still describes at half size, so a texture whose
  data pointer is a copy destination reads the copy as made. The bridge gives every destination a
  private address range and storage: copies never write game memory or overlap. A display copy
  (BP `0x52` bit 14) has no destination of its own and is not planned, since the frame is presented
  from the EFB. Aurora clears colour, alpha and depth to the copy-clear values when a frame begins,
  whatever the update masks, and the game relies on it. The bridge clears the EFB's full 640 columns,
  past a narrower frame: nothing draws or reads them, and a whole-EFB clear needs no scissor.
- Offscreen passes. `GXCreateFrameBuffer` stands in for `GXSetViewport(0, 0, w, h, 0, 1)` and
  `GXSetScissor(0, 0, w, h)`; `GXRestoreFrameBuffer` puts back the frame's viewport and scissor.
  Draws and copies between them use a separate cleared target and leave the EFB as it was.
- Scissor offset. The fork has no `GXSetScissorBoxOffset`, so the bridge writes BP `0x59` at the
  first feed: 171 (342 halved), what `GXSetScissor` and `GXSetViewport` add. Upstream trees encode each
  half as `(offset - 342) >> 1` in ten bits; the console field is `(offset + 342) >> 1`, which the
  bridge rebuilds from the signed offset.
- Texture state. J3D display lists write console TMEM offsets and truncated pointers for image and
  TLUT addresses; the bridge takes them from `LOAD_TEXOBJ`, as Aurora's renderer does. Aurora's
  textures may pass 1024 texels (a font's pages joined into one), while the image register keeps
  only the low ten bits of each size. A texture's negative LOD bias is sign-extended over bits
  17..23, since the PC build converts the float bias to an integer the console truncated to its
  8-bit field.

### PC texture formats

| Format | Handling |
|---|---|
| `GX_TF_R8_PC` | Tiled as I8 |
| `GX_TF_RGBA8_PC` | Tiled as GX RGBA8 |
| `GX_TF_BC1_PC` | Converted to CMPR |
| `GX_TF_RG8_PC` | Unsupported (no GX equivalent). Logged; the map keeps its previous texture. |
| Any PC format with mipmaps | Unsupported. Logged; the map keeps its previous texture. |

`tests/aurora/` checks the conversions against Starlet's texture decoder.

### EFB copies

Copies stay on the GPU and never write game memory, as in Aurora's renderer. If a game
reads a copy on the CPU, Starlet can write it back. Use `Renderer::write_back` and
`land_write_backs` in `include/starlet/gx/gles/renderer.hpp`. The bridge does not wire this
up. Each readback costs about 1 ms of GL thread time on Mali.

## Logs

- `[starlet-issue] HH:MM:SS frame N: <kind> (<detail>)` reports what can change the picture:
  - rejected draws;
  - unmapped textures, TLUTs or vertex arrays;
  - refused copies;
  - unsupported readbacks;
  - ignored depth snapshots and polygon offsets.

  The first line of each kind includes the BP, CP and XF state. Limits: at most 40 lines
  per kind and 400 in total; a kind logs again only after 300 quiet frames.
- `AURORA_STARLET_TRACE=1` adds detail for rejected draws and texture loads.
- `STARLET_MERGE=0` disables draw merging.
- Draws that Starlet accepts but renders wrong are not logged. Compare them with
  `AURORA_RENDERER` unset where a second renderer exists; a flat or single-colour frame with
  counters at 0 means the draws fall outside a scissor or viewport register, so dump the BP state
  at draw time.

Report issues at https://github.com/Producdevity/starlet/issues. Include:

- the full log;
- the Starlet, Aurora and game revisions;
- the device, driver, and GL strings;
- a timestamped screenshot;
- the same scene without Starlet, if the tree has another renderer.

## License

Starlet and the bridge are GPL-3.0. A binary that links Starlet is GPL-3.0 as a whole,
dynamically linked or not, and distributing it requires offering its complete source.
Compatible project licenses: GPL-3.0, GPL-2.0-or-later, Apache-2.0, MIT, BSD. Incompatible:
GPL-2.0-only, proprietary, non-commercial. Never distribute game data.

# Aurora integration

Starlet can replace the renderer of an Aurora GLES port. Aurora keeps the window, input, UI
and audio. Starlet decodes the GX FIFO and draws it with OpenGL ES 3.1.
`AURORA_RENDERER=starlet` selects it at run time.

Code: [`integrations/aurora/`](https://github.com/Producdevity/starlet/tree/master/integrations/aurora).
It has the bridge (`starlet_backend/`) and the Aurora changes (`aurora-hooks.patch`).

## Requirements

| | |
|---|---|
| Aurora | bmdhacks/aurora `3fbacee` (GLES backend). Upstream encounter/aurora renders through WebGPU and is not supported. |
| Starlet | `85f9836` or later, with `integrations/aurora/` |
| GL | OpenGL ES 3.1 context current on Aurora's render worker |
| Frame | EFB at most 640x528, even height |
| Build | CMake 3.21, C++17, `GLES3/gl31.h` and `libGLESv2` in the sysroot |

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
averaged 29.8 fps; the game caps at 30. Other GPUs and other Aurora games are untested.

## Build

```sh
git submodule add https://github.com/Producdevity/starlet extern/starlet
git -C extern/starlet checkout <commit>
git submodule update --init --recursive extern/starlet
cp -r extern/starlet/integrations/aurora/starlet_backend extern/aurora/lib/starlet_backend
git -C extern/aurora apply "$PWD/extern/starlet/integrations/aurora/aurora-hooks.patch"
cmake -S . -B build -DAURORA_STARLET_DIR="$PWD/extern/starlet" -DSTARLET_HOST=OFF
```

- `aurora_gx` then requires Starlet and links `starlet::gx_gles`.
- Starlet's `third_party/zstd` submodule must be checked out.
- The bridge calls Starlet's renderer API, so update both together.

## Aurora hooks

`aurora-hooks.patch` contains these changes. All of them are gated by
`starlet_backend::enabled()`. On another Aurora revision, apply them by hand.

| Location | Change |
|---|---|
| `cmake/aurora_gx.cmake` | Adds Starlet, the bridge sources and `starlet::gx_gles` |
| `lib/gx/fifo.cpp`, `drain()` | `starlet_backend::process()` replaces Aurora's command processor |
| `lib/aurora.cpp`, `end_frame()` | `starlet_backend::end_frame()` after the last drain |
| Render worker, end-of-frame callback | `starlet_backend::present(fbo, w, h)`; `webgpu::present_frame` runs only if it returns false. RmlUi and ImGui draw on top. |
| `aurora::shutdown()` | `starlet_backend::shutdown()` before `gfx::shutdown()` |
| `GXTexture.cpp`, `emit_loaded_texobj_metadata` | `texture_data()` snapshots texture pages the game wrote this frame |
| `GXDispList.cpp`, `GXCallDisplayList` | `note_display_list()` lets Starlet skip hashing unchanged lists |
| `GXFrameBuffer.cpp`, `GXSetCopyFilter` | Emits BP `0x01`-`0x04` and `0x53`/`0x54` like the SDK. Aurora's version is empty, and zero filter coefficients black out copies. |
| `GXTransform.cpp`, `command_processor.cpp` | Viewport origin bias 342 (hardware and SDK) instead of 340 |

## Game changes

These are not in the patch.

Report CPU writes to memory the GPU reads. Without them, textures and vertex arrays the game
rewrites in place render stale:

```cpp
extern "C" void aurora_starlet_note_write(const void* pointer, unsigned size);

void DCStoreRange(void* addr, u32 nBytes) { aurora_starlet_note_write(addr, nBytes); }
// Same for DCFlushRange, DCFlushRangeNoSync, DCInvalidateRange, DCStoreRangeNoSync.
```

Skip `aurora::gx::dl::optimize` when `AURORA_RENDERER=starlet`, because Starlet decodes the
original display lists. Dusklight does this in the `J3DShapeDraw` constructor.

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

## Stream coverage

| Aurora command | Handling |
|---|---|
| BP, CP, XF, draws, inlined display lists | Decoded directly |
| `LOAD_ARRAYBASE` | Mapped to a 32-bit address; honours the little-endian flag |
| `LOAD_TEXOBJ`, `LOAD_TLUT` | Mapped; textures over 1024 texels supported |
| `LOAD_COPY_SRC`, `LOAD_COPY_DST`, `LOAD_COPY_DEST` | EFB copy to a GPU texture keyed by the destination pointer |
| `BEGIN_OFFSCREEN`, `END_OFFSCREEN` | Offscreen pass |
| `DRAW_SIZED`, `DRAW_INDEXED` | Expanded to hardware vertex data |
| `DEBUG_GROUP_*`, `DESTROY_*` | Consumed |
| `LOAD_VIEWPORT_RENDER`, `LOAD_SCISSOR_RENDER`, `LOAD_PROJECTION_FULL` | Ignored; the hardware registers drive the draw |
| `REQUEST_DEPTH_SNAPSHOT` | Ignored and logged; depth readback effects see stale values |
| `GX2_SET_POLYGON_OFFSET` | Ignored and logged; coplanar decals can z-fight |
| Display list call with a host pointer | Ignored |
| Anything else | Logged once, then Starlet stops decoding for the run |

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
  - ignored depth snapshots and polygon offsets.

  The first line of each kind includes the BP, CP and XF state. Limits: at most 40 lines
  per kind and 400 in total; a kind logs again only after 300 quiet frames.
- `AURORA_STARLET_TRACE=1` adds detail for rejected draws and texture loads.
- `STARLET_MERGE=0` disables draw merging.
- Draws that Starlet accepts but renders wrong are not logged. Compare them with
  `AURORA_RENDERER` unset.

Report issues at https://github.com/Producdevity/starlet/issues. Include:

- the full log;
- the Starlet, Aurora and game revisions;
- the device, driver, and GL strings;
- a timestamped screenshot;
- the same scene without Starlet.

## License

Starlet and the bridge are GPL-3.0. A binary that links Starlet is GPL-3.0 as a whole,
dynamically linked or not, and distributing it requires offering its complete source.
Compatible project licenses: GPL-3.0, GPL-2.0-or-later, Apache-2.0, MIT, BSD. Incompatible:
GPL-2.0-only, proprietary, non-commercial. Never distribute game data.

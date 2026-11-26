# Aurora bridge

`starlet_backend/` is the bridge: it decodes Aurora's GX command stream into Starlet's renderer.
Two Aurora trees use it. Each has its own hooks patch and builds the bridge for its own stream:

| Aurora | Patch | Bridge location | GL context |
|---|---|---|---|
| encounter/aurora `5794daa` (WebGPU, Dawn stays linked) | `aurora-hooks-upstream.patch` | built in place from `AURORA_STARLET_DIR` | SDL-owned; a thread of the bridge owns it |
| encounter/aurora `5179721` (older upstream) | `aurora-hooks-upstream-5179721.patch` | as above | as above |
| bmdhacks/aurora `3fbacee` (GLES fork) | `aurora-hooks-gles-fork.patch` | copied to `lib/starlet_backend/` | Aurora's render worker owns it |

The bridge reads the Aurora tree from `GXAurora.h`: the fork defines `GX_AURORA_REQUEST_DEPTH_SNAPSHOT`
(id `0x38`); upstream trees do not. Each optional command (`SET_TEX_IMG_PTR` `0x38`, `LOAD_DISP_COPY_SRC`
`0x3b`, `SET_DRAWS_BLOCKING`, readbacks, `CLEAR`) is compiled in when its macro exists, so `5179721`
(none of `0x38`, `0x3b`, `0x52`) and `5794daa` both build.
`aurora_commands.hpp` lists every command id and payload size; the ids are `static_assert`ed
against the header, so a renumbered command fails the build.

Guide: [docs/Aurora-integration.md](../../docs/Aurora-integration.md).

## Upstream tree

```sh
git -C extern/aurora am "$PWD/extern/starlet/integrations/aurora/aurora-hooks-upstream.patch"
cmake -S . -B build -DAURORA_STARLET_DIR="$PWD/extern/starlet" -DSTARLET_HOST=OFF
```

Without `AURORA_STARLET_DIR` the hooks compile to nothing. With it, `AURORA_RENDERER=starlet`:

- creates the SDL3 window with `SDL_WINDOW_OPENGL` and OpenGL ES 3.1 attributes;
- skips Dawn, `gfx::initialize`, the render worker and ImGui;
- draws RmlUi with the bridge's render interface when Aurora is built with `AURORA_ENABLE_RMLUI`;
- drains the GX FIFO on the calling thread into the bridge;
- presents each frame letterboxed 4:3 from the bridge's GL thread with `SDL_GL_SwapWindow`.

`AURORA_STARLET_VSYNC=1` sets swap interval 1; the default is 0.

The game needs `aurora_starlet_note_write` in its `DC*Range` functions (see the guide).

## GLES fork

```sh
cp -r extern/starlet/integrations/aurora/starlet_backend extern/aurora/lib/starlet_backend
git -C extern/aurora apply "$PWD/extern/starlet/integrations/aurora/aurora-hooks-gles-fork.patch"
```

Unsupported Aurora commands log through the issue log (`[starlet-issue]`). A stream the bridge cannot follow stops decoding for the run.

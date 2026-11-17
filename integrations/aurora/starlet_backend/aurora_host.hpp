#pragma once

// Aurora's own headers, which the bridge reaches by path. The bridge is built in place for the
// upstream tree, with Aurora's lib/ on the include path, and copied to lib/starlet_backend/ for
// the GLES fork, where those headers are the parent directory's.
#include "aurora_commands.hpp"

#if STARLET_AURORA_UPSTREAM
#include "dolphin/os/internal.hpp"
#include "gx/fifo.hpp"
#include "gx/gx.hpp"
#include "internal.hpp"
#include "logging.hpp"
#include "window.hpp"
#else
#include "../dolphin/os/internal.hpp"
#include "../gfx/render_worker.hpp"
#include "../gl/gl_core.hpp"
#include "../gl/state.hpp"
#include "../gx/fifo.hpp"
#include "../gx/gx.hpp"
#include "../internal.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#endif

#!/bin/bash
# Replays every GX trace in the trace directory against this checkout and compares each frame
# with the trace's reference. See docs/Replay-traces.md.
# Usage: tools/replay-all.sh [--update] [--twice] [--shaders] [--only NAME]...
# --shaders only compiles the renderer's programs (seconds).
# Environment: STARLET_TRACES (default: starlet-traces beside this checkout), STARLET_REPLAY_OUT
# (default: $TMPDIR/starlet-replay-out), STARLET_REPLAY_BUILD (build cache), STARLET_REPLAY_CMAKE_ARGS
# (extra configure arguments for the replayer build).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
traces=${STARLET_TRACES:-$(cd "$root/.." && pwd)/starlet-traces}
out=${STARLET_REPLAY_OUT:-${TMPDIR:-/tmp}/starlet-replay-out}
build=${STARLET_REPLAY_BUILD:-$HOME/.cache/starlet-replay/$(printf '%s' "$root" | shasum | cut -c1-12)}
image=starlet-replay:bookworm

update=0 twice=0 shaders=0 only=()
while [ $# -gt 0 ]; do
  case "$1" in
    --update) update=1 ;;
    --twice) twice=1 ;;
    --shaders) shaders=1 ;;
    --only) only+=("${2:?--only needs a trace name}"); shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

[ -d "$traces" ] || { echo "no trace directory: $traces" >&2; exit 2; }
mkdir -p "$build" "$out"
docker image inspect "$image" >/dev/null 2>&1 || docker build -q -t "$image" "$root/tools/replay" >/dev/null
commit=$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)
git -C "$root" diff --quiet HEAD 2>/dev/null || commit="$commit-dirty"
traces_mode=ro
[ "$update" = 1 ] && traces_mode=rw

exec docker run --rm \
  -v "$root:/starlet:ro" -v "$traces:/traces:$traces_mode" -v "$out:/out" -v "$build:/build" \
  -e UPDATE="$update" -e TWICE="$twice" -e SHADERS_ONLY="$shaders" -e COMMIT="$commit" -e ONLY="${only[*]:-}" -e OUT_HOST="$out" -e CMAKE_ARGS="${STARLET_REPLAY_CMAKE_ARGS:-}" \
  "$image" bash /starlet/tools/replay/run-all.sh

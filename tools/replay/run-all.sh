#!/bin/bash
# Runs inside starlet-replay:bookworm for tools/replay-all.sh.
# Mounts: /starlet (checkout), /traces, /out, /build. Env: UPDATE, TWICE, SHADERS_ONLY, COMMIT, ONLY, OUT_HOST,
# CMAKE_ARGS (extra configure arguments, such as -DSTARLET_GX_TRACE=ON).
set -uo pipefail
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EGL_PLATFORM=surfaceless

start=$(date +%s)
cmake -S /starlet -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARLET_GLES=ON -DSTARLET_REPLAY=ON -DSTARLET_TESTS=OFF ${CMAKE_ARGS:-} >/build/configure.log 2>&1 \
  && ninja -C /build starlet-replay >/build/build.log 2>&1 \
  || { echo "build failed:"; tail -30 /build/build.log /build/configure.log; exit 2; }
replay=/build/tools/replay/starlet-replay
echo "built in $(( $(date +%s) - start )) s"

# Every program the renderer generates outside draws, and the programs listed by the traces'
# references (fast shading) and by the ports' shipped lists in /traces/programs.
status=0
shopt -s nullglob
lists=(/traces/*.ref/programs.list /traces/programs/*.list)
t0=$(date +%s)
if "$replay" --shaders "${lists[@]}" >/out/shaders.txt 2>&1; then
  echo "PASS shaders ($(grep -c '^shaders: /' /out/shaders.txt) program lists, $(( $(date +%s) - t0 )) s): $(grep '^shaders: [0-9]' /out/shaders.txt | sed 's/^shaders: //')"
else
  status=1
  echo "FAILED shaders: $(grep -c '^FAILED' /out/shaders.txt) programs; details in ${OUT_HOST:-/out}/shaders.txt"
  grep '^FAILED' /out/shaders.txt | head -5 | cut -c1-200 | sed 's/^/  /'
fi
[ "${SHADERS_ONLY:-0}" = 1 ] && exit $status

run() { # trace, dump dir -> hashes on stdout
  case "$1" in
    *.zst) zstd -dcq "$1" | "$replay" - --quiet --dump "$2" ;;
    *) "$replay" "$1" --quiet --dump "$2" ;;
  esac
}

for trace in /traces/*.trace /traces/*.trace.zst; do
  name=$(basename "$trace"); name=${name%.zst}; name=${name%.trace}
  if [ -n "$ONLY" ] && ! [[ " $ONLY " == *" $name "* ]]; then continue; fi
  ref=/traces/$name.ref
  dir=/out/$name
  rm -rf "${dir:?}"; mkdir -p "$dir"
  t0=$(date +%s)
  run "$trace" "$dir" >"$dir/hashes.txt" 2>"$dir/replay.log"
  rc=$?
  seconds=$(( $(date +%s) - t0 ))
  if [ "$rc" = 3 ]; then
    echo "DIVERGED $name: $(head -1 "$dir/hashes.txt")"; status=1; continue
  elif [ "$rc" = 4 ]; then
    echo "FAILED $name: $(grep '^FAILED' "$dir/hashes.txt" | head -1)"; status=1; continue
  elif [ "$rc" != 0 ]; then
    echo "FAILED $name: $(tail -1 "$dir/replay.log")"; status=1; continue
  fi
  frames=$(grep -c '^frame ' "$dir/hashes.txt")
  if [ "$TWICE" = 1 ]; then
    mkdir -p "$dir/again"
    run "$trace" "$dir/again" >"$dir/again/hashes.txt" 2>/dev/null
    if ! cmp -s "$dir/hashes.txt" "$dir/again/hashes.txt"; then
      echo "NONDETERMINISTIC $name: $(diff "$dir/hashes.txt" "$dir/again/hashes.txt" | grep -c '^<') frames differ between two replays"; status=1
    fi
  fi
  if [ "$UPDATE" = 1 ]; then
    rm -rf "${ref:?}"; mkdir -p "$ref"
    cp "$dir"/frame-*.png "$ref"/
    [ -f "$dir/programs.list" ] && cp "$dir/programs.list" "$ref"/
    { echo "# starlet $COMMIT"; cat "$dir/hashes.txt"; } >"$ref/hashes.txt"
    echo "UPDATED $name ($frames frames, ${seconds} s)"
    continue
  fi
  if [ ! -f "$ref/hashes.txt" ]; then
    echo "NO REFERENCE $name ($frames frames)"; status=1; continue
  fi
  bad=()
  while read -r _ frame hash; do
    expected=$(awk -v f="$frame" '$1 == "frame" && $2 == f { print $3 }' "$ref/hashes.txt")
    [ "$expected" = "$hash" ] && continue
    bad+=("$frame")
    png=$(printf 'frame-%03d.png' "$frame")
    [ -f "$ref/$png" ] && "$replay" --compare "$ref/$png" "$dir/$png" "$dir/diff-$png" >"$dir/diff-$frame.txt" 2>&1
  done < <(grep '^frame ' "$dir/hashes.txt")
  expected_frames=$(grep -c '^frame ' "$ref/hashes.txt")
  [ "$frames" = "$expected_frames" ] || bad+=("count $frames/$expected_frames")
  if [ ${#bad[@]} = 0 ]; then
    echo "PASS $name ($frames frames, ${seconds} s)"
  else
    status=1
    echo "DIFF $name: frames ${bad[*]} ($(head -1 "$ref/hashes.txt" | sed 's/^# //')); diff images in ${OUT_HOST:-/out}/$name"
    for f in "$dir"/diff-*.txt; do echo "  $(basename "$f" .txt): $(cat "$f")"; done
  fi
done
echo "total $(( $(date +%s) - start )) s"
exit $status

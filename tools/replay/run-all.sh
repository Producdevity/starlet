#!/bin/bash
# Runs inside starlet-replay:bookworm for tools/replay-all.sh.
# Mounts: /starlet (checkout), /traces, /out, /build. Env: UPDATE, TWICE, SHADERS_ONLY, COLD (also, only or off),
# COLD_PROFILES, COLD_LIMIT, COLD_JOBS, COMMIT, ONLY, OUT_HOST, CMAKE_ARGS (extra configure arguments, such as
# -DSTARLET_GX_TRACE=ON).
set -uo pipefail
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EGL_PLATFORM=surfaceless

start=$(date +%s)
cmake -S /starlet -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARLET_GLES=ON -DSTARLET_REPLAY=ON -DSTARLET_TESTS=OFF ${CMAKE_ARGS:-} >/build/configure.log 2>&1 \
  && ninja -C /build starlet-replay >/build/build.log 2>&1 \
  || { echo "build failed:"; tail -n 30 /build/build.log /build/configure.log; exit 2; }
replay=/build/tools/replay/starlet-replay
echo "built in $(( $(date +%s) - start )) s"

# Every program the renderer generates outside draws, and the programs listed by the traces'
# references (fast shading) and by the ports' shipped lists in /traces/programs.
status=0
shopt -s nullglob
COLD=${COLD:-also}
lists=(/traces/*.ref/programs.list /traces/programs/*.list)
t0=$(date +%s)
if [ "$COLD" = only ]; then
  :
elif "$replay" --shaders "${lists[@]}" >/out/shaders.txt 2>&1; then
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

# Frames of $1 (a hashes file) and the images in $2 that differ from the reference $3: sets bad, writes diffs.
compare_frames() {
  local hashes=$1 dir=$2 ref=$3 frame hash expected png expected_frames count
  bad=()
  while read -r _ frame hash; do
    expected=$(awk -v f="$frame" '$1 == "frame" && $2 == f { print $3 }' "$ref/hashes.txt")
    [ "$expected" = "$hash" ] && continue
    bad+=("$frame")
    png=$(printf 'frame-%03d.png' "$frame")
    [ -f "$ref/$png" ] && "$replay" --compare "$ref/$png" "$dir/$png" "$dir/diff-$png" >"$dir/diff-$frame.txt" 2>&1
  done < <(grep '^frame ' "$hashes")
  expected_frames=$(grep -c '^frame ' "$ref/hashes.txt")
  count=$(grep -c '^frame ' "$hashes")
  [ "$count" = "$expected_frames" ] || bad+=("count $count/$expected_frames")
}

[ "$COLD" = only ] && traces_done=1
for trace in /traces/*.trace /traces/*.trace.zst; do
  [ "${traces_done:-0}" = 1 ] && break
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
  compare_frames "$dir/hashes.txt" "$dir" "$ref"
  if [ ${#bad[@]} = 0 ]; then
    echo "PASS $name ($frames frames, ${seconds} s)"
  else
    status=1
    echo "DIFF $name: frames ${bad[*]} ($(head -1 "$ref/hashes.txt" | sed 's/^# //')); diff images in ${OUT_HOST:-/out}/$name"
    for f in "$dir"/diff-*.txt; do echo "  $(basename "$f" .txt): $(cat "$f")"; done
  fi
done

# Cold-cache mode: each trace again and again on one renderer with the program builder on and an empty
# program cache, until no draw waits for a program; the last pass is compared with the reference.
# A profile is the unready-link and builder settings (default, waker, eager), plus "+half" for a program
# list of every second program of the reference's list.
cold_job() { # trace name profile
  local trace=$1 name=$2 profile=$3 base=${3%+half} dir list=()
  dir=/out/$name/cold-$profile
  rm -rf "${dir:?}"; mkdir -p "$dir"
  if [ "$base" != "$profile" ]; then
    awk 'NR == 1 || NR % 2 == 0' "/traces/$name.ref/programs.list" >"$dir/half.list"
    list=(--list "$dir/half.list")
  fi
  local t0=$(date +%s) rc
  case "$trace" in
    *.zst) zstd -dcq "$trace" | timeout 120 "$replay" - --cold "$base" "${list[@]}" --limit "${COLD_LIMIT:-30}" --dump "$dir" >"$dir/hashes.txt" 2>"$dir/replay.log" ;;
    *) timeout 120 "$replay" "$trace" --cold "$base" "${list[@]}" --limit "${COLD_LIMIT:-30}" --dump "$dir" >"$dir/hashes.txt" 2>"$dir/replay.log" ;;
  esac
  rc=${PIPESTATUS[1]:-$?}
  echo "$rc $(( $(date +%s) - t0 ))" >"$dir/rc"
}

cold_report() { # name profile
  local name=$1 profile=$2 dir=/out/$1/cold-$2 rc seconds label="cold $2 $1" first
  read -r rc seconds <"$dir/rc"
  case "$rc" in
    0) ;;
    3) echo "DIVERGED $label: $(head -1 "$dir/hashes.txt")"; status=1; return ;;
    5) echo "HUNG $label: $(grep '^HUNG' "$dir/hashes.txt" | head -1)"; status=1; return ;;
    6) echo "FAILED $label:"; grep '^FAILED' "$dir/hashes.txt" | head -4 | cut -c1-300 | sed 's/^FAILED: /  /'; status=1; return ;;
    124) echo "HUNG $label: no result after 120 s"; status=1; return ;;
    *) echo "FAILED $label: exit $rc: $(grep '^CRASHED' "$dir/hashes.txt" | head -1)$(tail -n 1 "$dir/replay.log" | cut -c1-200)"; status=1; return ;;
  esac
  compare_frames "$dir/hashes.txt" "$dir" "/traces/$name.ref"
  first=$(grep '^cold ' "$dir/hashes.txt" | sed 's/^cold [a-z+]*: //')
  if [ ${#bad[@]} = 0 ]; then
    echo "PASS $label ($seconds s; $first)"
  else
    status=1
    echo "DIFF $label: frames ${bad[*]}; diff images in ${OUT_HOST:-/out}/$name/cold-$profile"
    for f in "$dir"/diff-*.txt; do echo "  $(basename "$f" .txt): $(cat "$f")"; done
  fi
}

if [ "$UPDATE" != 1 ] && [ "$COLD" != off ]; then
  t0=$(date +%s)
  profiles=(${COLD_PROFILES:-default waker eager default+half})
  jobs_max=${COLD_JOBS:-5}
  names=()
  for trace in /traces/*.trace /traces/*.trace.zst; do
    name=$(basename "$trace"); name=${name%.zst}; name=${name%.trace}
    if [ -n "$ONLY" ] && ! [[ " $ONLY " == *" $name "* ]]; then continue; fi
    if [ ! -f "/traces/$name.ref/hashes.txt" ]; then echo "NO REFERENCE cold $name"; status=1; continue; fi
    names+=("$name")
    for profile in "${profiles[@]}"; do
      while [ "$(jobs -rp | wc -l)" -ge "$jobs_max" ]; do wait -n; done
      cold_job "$trace" "$name" "$profile" &
    done
  done
  wait
  for name in "${names[@]}"; do
    for profile in "${profiles[@]}"; do cold_report "$name" "$profile"; done
  done
  echo "cold total $(( $(date +%s) - t0 )) s, programs built by workers and by the GL thread: $(cat /out/*/cold-*/hashes.txt | awk '/^cold / { for (i = 1; i < NF; ++i) { if ($i == "worker") w += $(i + 2); if ($i == "thread") g += $(i + 2) } } END { print w + 0 " and " g + 0 }')"
fi
echo "total $(( $(date +%s) - start )) s"
exit $status

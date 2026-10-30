#!/bin/sh
# bench.sh — local replica of the Speed Demon board run, for iteration.
#
# usage:  ./bench.sh <a|c|d> <track-dir> [data-dir] [runs]
# example: ./bench.sh a repo-staging/track-a data/a-full 3
#
# What it does, mirroring harness/run_submission.py:
#   1. docker build the track dir (offline)
#   2. run it N times under the board's exact sandbox flags + THEIR sd-timer
#      (wall time = exec→exit inside the container, like the board)
#   3. sha256 the single output file and compare with the spec's expected hash
#   4. print per-run walls, middle-3 mean, spread, and PASS/FAIL per run
#
# Notes:
#   - cpuset 0-11 is the box's P-core pin; Docker Desktop on Windows may not
#     allow cpuset — set CPUSET="" to skip (timings then un-pinned).
#   - M (memory.peak) is not readable through Docker Desktop; for a rough peak
#     watch `docker stats` during a run, or compare ru_maxrss printed by the
#     program itself during development.
set -u
TRACK=${1:?usage: bench.sh <a|c|d> <track-dir> [data-dir] [runs]}
DIR=${2:?usage: bench.sh <a|c|d> <track-dir> [data-dir] [runs]}
RUNS=${4:-3}
HERE=$(cd "$(dirname "$0")" && pwd -W)   # pwd -W = native D:/... form — Docker cannot use /d/... paths
WS=$(cd "$HERE/.." && pwd -W)
[ -d "$DIR" ] && DIR=$(cd "$DIR" && pwd -W)
if [ -n "${3:-}" ]; then DATA=$(cd "$3" && pwd -W); else DATA="D:/hermes/speed-demon/data/$TRACK-full"; fi
DOCKER='/c/Program Files/Docker/Docker/resources/bin/docker.exe'
CPUSET=${CPUSET---cpuset-cpus=0-11}
TIMER="$WS/scratch/bin/sd-timer"

case "$TRACK" in
  a) OUTNAME=summary.txt;   EXPECT=495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3 ;;
  c) OUTNAME=aggregates.txt; EXPECT=d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536 ;;
  d) OUTNAME=distances.txt;  EXPECT=a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e ;;
  *) echo "unknown track $TRACK"; exit 2 ;;
esac

[ -f "$TIMER" ] || { echo "sd-timer missing at $TIMER"; exit 2; }
TAG="sd-bench-$TRACK-$(date +%s)"

echo "== build =="
"$DOCKER" build -t "$TAG" "$DIR" || { echo BUILD_FAIL; exit 3; }

# The board runs the image's real ENTRYPOINT+CMD under its timer; do the same.
# (docker run's --entrypoint override replaces the image's entrypoint, so pull it out first.)
CMD=$("$DOCKER" image inspect -f '{{json .Config}}' "$TAG" | python -c "import json,sys; c=json.load(sys.stdin); print(' '.join((c.get('Entrypoint') or [])+(c.get('Cmd') or [])))")
[ -n "$CMD" ] || { echo NO_ENTRYPOINT; exit 3; }
echo "image command: $CMD"

echo "== run ($RUNS) =="
i=0
while [ $i -lt "$RUNS" ]; do
  i=$((i+1))
  WORK="$WS/scratch/bench/$TAG-$i"
  rm -rf "$WORK"; mkdir -p "$WORK/out" "$WORK/sd"
  START=$(date +%s%N)
  "$DOCKER" run --rm --name "b-$TAG-$i" $CPUSET \
    --memory=8g --memory-swap=8g --pids-limit=512 \
    --network=none --read-only --tmpfs /tmp:rw,size=1g,exec \
    --cap-drop=ALL --cap-add=SETUID --cap-add=SETGID --security-opt=no-new-privileges \
    --ulimit nofile=8192:8192 \
    -v "$DATA:/data:ro" -v "$WORK/out:/out:rw" -v "$WORK/sd:/sd:rw" -v "$(dirname "$TIMER"):/sd-bin:ro" \
    -e TRACK="$TRACK" --entrypoint /sd-bin/sd-timer "$TAG" \
    /sd/timing.json 65534 65534 -- $CMD >/dev/null 2>"$WORK/err.log" || true
  END=$(date +%s%N)
  OUTSIDE=$(( (END-START)/1000000 ))
  if [ -f "$WORK/sd/timing.json" ]; then
    WALL=$(python -c "import json;print(round(json.load(open(r'$WORK/sd/timing.json'))['wall_ns']/1e9,4))" 2>/dev/null || echo ERR)
    EXIT=$(python -c "import json;d=json.load(open(r'$WORK/sd/timing.json'));print(d['exit_code'],d['signal'])" 2>/dev/null || echo ERR)
  else
    WALL=NO_TIMING; EXIT=NO_FILE
  fi
  if [ -f "$WORK/out/$OUTNAME" ]; then
    H=$(sha256sum "$WORK/out/$OUTNAME" | cut -d' ' -f1)
    if [ "$H" = "$EXPECT" ]; then RESULT="OK"; else RESULT="HASH_MISMATCH $H"; fi
  else
    RESULT="NO_OUTPUT"
  fi
  echo "run $i: wall(sd-timer)=$WALL outside=${OUTSIDE}ms exit/sig=$EXIT $RESULT"
done
echo "== done: expected $EXPECT ($OUTNAME) =="

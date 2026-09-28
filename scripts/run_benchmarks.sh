#!/usr/bin/env bash
# Full performance / scalability sweep for Parts 1-3.
# Writes CSV files to results/<hostname>/ so runs from different machines
# ("all shared-memory platforms you possess") do not overwrite each other.
#
#   ./scripts/run_benchmarks.sh            # full sweep (~10-20 min)
#   QUICK=1 ./scripts/run_benchmarks.sh    # small smoke run (~1 min)
#
# Override the thread counts with THREADS="1,2,4,8".
set -euo pipefail
cd "$(dirname "$0")/.."

make -s all

HOST=$(hostname -s)
OUT=results/$HOST
mkdir -p "$OUT"

# Hardware description, to be quoted in the report.
{
  echo "host: $HOST"
  echo "date: $(date)"
  if [[ "$(uname)" == "Darwin" ]]; then
    echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
    echo "logical cpus: $(sysctl -n hw.ncpu)"
    echo "performance cores: $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo ?)"
    echo "efficiency cores: $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo ?)"
    echo "cache line: $(sysctl -n hw.cachelinesize) bytes"
  else
    lscpu
  fi
  echo "compiler: $(${CXX:-clang++} --version | head -1)"
} > "$OUT/machine.txt"

NCPU=$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )
if [[ -z "${THREADS:-}" ]]; then
  THREADS="1,2,4,8"
  for t in 12 16 24 32 48 64; do (( t <= NCPU )) && THREADS="$THREADS,$t"; done
  (( NCPU > 8 && NCPU != 12 && NCPU != 16 )) && THREADS="$THREADS,$NCPU"
fi
OVERSUB=$((2 * NCPU))

if [[ "${QUICK:-0}" == 1 ]]; then
  LOCK_ITERS=2000; BAR_EPISODES=20000; CNT_ITERS=200000; REPS=1
  OUTSIDE_LIST="0 4"
else
  LOCK_ITERS=10000; BAR_EPISODES=200000; CNT_ITERS=1000000; REPS=5
  OUTSIDE_LIST="0 1 4 16"
fi

echo "machine info  -> $OUT/machine.txt"
echo "thread counts -> $THREADS (oversubscribed: $OVERSUB)"

# ---------------------------------------------------------------- Part 1 ---
F=$OUT/locks.csv
echo "Part 1: locks -> $F"
./bin/bench_lock --lock=all --threads="$THREADS" --iters=$LOCK_ITERS \
                 --outside=0 --reps=$REPS > "$F"
for w in $OUTSIDE_LIST; do
  [[ $w == 0 ]] && continue
  ./bin/bench_lock --lock=all --threads="$THREADS" --iters=$LOCK_ITERS \
                   --outside=$w --reps=$REPS --no-header >> "$F"
done
# Short critical section (N=101, ~50 ns): lock hand-off cost dominates, so
# the differences between TAS / TTAS / MCS become visible.
./bin/bench_lock --lock=all --threads="$THREADS" --iters=$((LOCK_ITERS * 10)) \
                 --outside=0 --n=101 --reps=$REPS --no-header >> "$F"
# Backoff with sleep_for (the assignment's snippet) instead of spinning.
./bin/bench_lock --lock=tas_backoff,ttas_backoff --threads="$THREADS" \
                 --iters=$LOCK_ITERS --outside=0 --reps=$REPS \
                 --backoff=sleep --no-header >> "$F"
# Oversubscription: more threads than cores (few iterations: spinlocks
# degrade badly when the lock holder gets preempted).
./bin/bench_lock --lock=all --threads=$OVERSUB --iters=$((LOCK_ITERS / 50)) \
                 --outside=0 --reps=$REPS --no-header >> "$F"

# ---------------------------------------------------------------- Part 2 ---
F=$OUT/barriers.csv
echo "Part 2: barriers -> $F"
./bin/bench_barrier --barrier=all --threads="$THREADS" \
                    --episodes=$BAR_EPISODES --reps=$REPS > "$F"
./bin/bench_barrier --barrier=sense_backoff --threads="$THREADS" \
                    --episodes=$((BAR_EPISODES / 10)) --reps=$REPS \
                    --backoff=sleep --no-header >> "$F"

# ---------------------------------------------------------------- Part 3 ---
F=$OUT/counters.csv
echo "Part 3: counters -> $F"
./bin/bench_counter --counter=all --threads="$THREADS" --iters=$CNT_ITERS \
                    --work=0 --reps=$REPS > "$F"
./bin/bench_counter --counter=all --threads="$THREADS" --iters=$((CNT_ITERS / 4)) \
                    --work=100 --reps=$REPS --no-header >> "$F"

echo "done. Plot with: python3 scripts/plot.py $OUT"

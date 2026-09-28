# Parallel Computing — Assignment 1

Spinlocks (TAS / TTAS / MCS), a sense-reversing barrier and concurrent
counters in C++20, with correctness tests and a benchmark sweep.

## Layout

```
include/
  common.h      cache-line size, cpu_relax(), exponential Backoff, is_prime(), CLI parsing
  locks.h       Part 1: TAS, TAS+backoff, TTAS, TTAS+backoff, MCS  (+ std::mutex, pthread, OpenMP baselines)
  barriers.h    Part 2: sense-reversing barrier (+backoff)          (+ std::barrier, pthread condvar baselines)
  counters.h    Part 3: lock-based, CAS, CAS+backoff, fetch-and-add counters
src/
  bench_lock.cpp     the program from the assignment (thread_entry / is_prime / OUTSIDE_WORK)
  bench_barrier.cpp  barrier episodes per second, no work between barriers
  bench_counter.cpp  execution time of T threads x I increments
tests/
  test_all.cpp       correctness tests for all three parts
scripts/
  run_benchmarks.sh  full parameter sweep -> results/<hostname>/*.csv
  plot.py            CSV -> PNG figures for the report
old/                 the original first attempt (kept for reference)
```

## Build & run

```sh
make                 # builds bin/bench_lock, bin/bench_barrier, bin/bench_counter, bin/test_all
make test            # correctness tests (~40 s)
make tsan            # the same tests under ThreadSanitizer (~15 s)
make bench           # full benchmark sweep (~10-20 min), close other apps first
pip3 install matplotlib && python3 scripts/plot.py results/$(hostname -s)
```

Single experiments:

```sh
./bin/bench_lock    --lock=tas,ttas,mcs --threads=1,2,4,8 --outside=0 --n=2003 --iters=20000 --reps=5
./bin/bench_barrier --barrier=sense,std_barrier --threads=2,4,8 --episodes=200000
./bin/bench_counter --counter=cas,cas_backoff,faa --threads=1,2,4,8 --iters=1000000
```

Common options: `--reps=K` (report the median of K runs),
`--backoff=spin|sleep`, `--min-delay=NS`, `--max-delay=NS`.

OpenMP baselines (`omp_lock`, `omp_barrier`) are compiled in automatically
once libomp is available (`brew install libomp` on macOS).

## Design notes

* **One interface per part.** Every lock has `acquire()/release()`, every
  barrier `wait(tid)`, every counter `increment()/get()`. All benchmarks and
  tests are templates, so each implementation runs through exactly the same code.
* **MCS without arguments.** The assignment calls `lock.acquire()` with no
  arguments, so each thread's queue node is a `thread_local`. The explicit
  `acquire(Node&)` overloads remain for holding several MCS locks at once.
* **False sharing.** Every hot atomic is `alignas(CACHE_LINE)`. The line is
  128 bytes on Apple Silicon and 64 bytes on x86.
* **Memory ordering.** Locks use acquire on lock and release on unlock. The
  barrier uses `acq_rel` on the arrival counter and release/acquire on the
  sense flag. Counters use `relaxed`, because a counter only needs its
  increment to be atomic. See the comments in each header.
* **Backoff.** It follows the assignment's snippet: the delay starts small
  and doubles after every failed attempt, up to `MAX_DELAY`.
  `--backoff=sleep` uses `std::this_thread::sleep_for`, as in the snippet.
  `--backoff=spin` (the default) busy-waits instead, because OS sleep
  granularity is microseconds: even `sleep_for(1ns)` gives up the core.
* **Benchmark hygiene.** `-O2`; `do_not_optimize()` keeps the compiler from
  deleting the `is_prime` work; the median of several runs is reported; the
  barrier and counter benchmarks start all threads at a start gate so thread
  creation is not timed.

## How correctness is tested (`tests/test_all.cpp`)

| Part | What is checked |
|------|-----------------|
| Locks | (a) an atomic occupancy counter inside the critical section must always be 0 on entry; (b) two plain ints updated in sequence (`a++; maybe yield; b++`) must be equal on entry; (c) a plain counter must end at exactly threads × iters. Random `yield()` inside the critical section widens race windows. |
| Barrier | Each round: write my slot = k → `wait` → every slot must equal k → `wait`. A thread that leaves early sees k−1; one that runs ahead makes others see k+1. Plain (non-atomic) slots also check that the barrier publishes memory. |
| Counters | `increment()` returns the previous value; over all threads the returned values must be exactly {0 … total−1} with no duplicates or gaps. This is stronger than checking the final total. |

Every test runs with 2 threads, an odd count (3), all cores, and 2× the core
count (oversubscription, so threads get preempted while holding or waiting).
Threads start together behind a start gate so they truly overlap.

**Negative controls.** `NoLock`, `NoBarrier` and `RacyCounter` are run
through the same tests and must fail. This shows the tests can detect a
broken implementation.

**ThreadSanitizer** (`make tsan`) checks the happens-before relation on
every access, so it also catches a wrong memory ordering that happens not to
fail on one run. Run `./bin/test_all_tsan --controls` to watch it report the
negative controls. `std::barrier` is skipped under TSan: libc++ waits inside
its non-instrumented dylib, so TSan reports a false positive there.

## Observations so far (Apple M5 Pro, 5P + 10E cores)

These come from short smoke runs. Re-run `make bench` for the numbers you
put in the report.

* **Oversubscription (30 threads on 15 cores).** MCS drops from about 700k
  to about 1.1k critical sections/s. FIFO hand-off to a *preempted* waiter
  stalls the whole queue. TAS/TTAS and `std::mutex` are barely affected.
  The spinning sense barrier also degrades: spinning waiters occupy the
  cores that the late arrivers need.
* **Short critical section (N=101).** TTAS/TAS with backoff scale best, and
  MCS is the slowest here: its fair hand-off always moves the lock to
  another core (possibly an efficiency core). TAS/TTAS let the releasing
  thread re-acquire while its cache is still hot, which is unfair but fast.
* **Counters.** At high contention, CAS with backoff can beat fetch-and-add,
  because backoff lets one thread keep the cache line for a run of
  increments (unfair). Lock-based counters are one to two orders of
  magnitude slower.
* **Barriers.** The sense-reversing barrier keeps pace with `std::barrier`
  and beats it at 15 threads. The condition-variable barrier is about 50×
  slower (every episode goes through the kernel). `sleep_for` backoff is a
  bad fit for barriers.

## Not done

Part 4 (MPI-3 one-sided, optional): not implemented. MPI is not installed
on this machine (`brew install open-mpi` would provide `mpicxx`).

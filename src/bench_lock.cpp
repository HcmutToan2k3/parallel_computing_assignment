// bench_lock.cpp - Part 1: performance of mutex locks.
//
// This is the program from the assignment:
//
//   for i in 0..NUM_OF_ITERS:
//       lock.acquire();  is_prime(N);  lock.release();        // critical
//       for j in 0..OUTSIDE_WORK: is_prime(N);                // parallel
//
// OUTSIDE_WORK controls contention: 0 -> all work is in the critical
// section; k -> 1/(k+1) of the work is in the critical section.
//
// The #defines are the defaults; every value can be overridden on the
// command line so a script can sweep parameters without recompiling:
//
//   ./bin/bench_lock --lock=tas,ttas,mcs --threads=1,2,4,8 \
//                    --iters=20000 --outside=0 --n=2003 --reps=5
//
// Output: one CSV line per (lock, threads) with the median of --reps runs.
#include <cstdio>
#include <thread>
#include <vector>

#include "common.h"
#include "locks.h"

/* Configurations (defaults) */
#define NUM_OF_THREADS 4
#define NUM_OF_ITERS   20000
#define OUTSIDE_WORK   0
#define N              2003   // a prime -> is_prime does N-2 divisions

struct Config {
    int  iters;
    int  outside_work;
    int  n;
};

// Number of completed critical sections. It is a plain (non-atomic) long
// that is only modified inside the critical section, so if the lock works
// it must end up exactly equal to threads * iters. This is a cheap sanity
// check that runs on every benchmark.
static long cs_counter = 0;

/* Child thread's code */
template <class Lock>
void thread_entry(Lock& lock, const Config& cfg) {
    for (int i = 0; i < cfg.iters; ++i) {
        lock.acquire();

        // check if a number N is a prime (work inside the critical section)
        do_not_optimize(is_prime(cfg.n));
        ++cs_counter;

        lock.release();

        for (int j = 0; j < cfg.outside_work; ++j)
            do_not_optimize(is_prime(cfg.n));
    }
}

template <class Lock>
double run_once(int num_threads, const Config& cfg) {
    Lock lock;  /* Shared variable */
    cs_counter = 0;

    auto t0 = Clock::now();
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back([&] { thread_entry(lock, cfg); });
    for (auto& th : threads) th.join();
    double elapsed = seconds_since(t0);

    long expected = (long)num_threads * cfg.iters;
    if (cs_counter != expected) {
        std::fprintf(stderr, "MUTUAL EXCLUSION VIOLATED: counter=%ld expected=%ld\n",
                     cs_counter, expected);
        std::exit(1);
    }
    return elapsed;
}

int main(int argc, char** argv) {
    Args args(argc, argv);
    args.apply_backoff();

    Config cfg{
        (int)args.get("iters",   (long)NUM_OF_ITERS),
        (int)args.get("outside", (long)OUTSIDE_WORK),
        (int)args.get("n",       (long)N),
    };
    int reps = (int)args.get("reps", 3L);

    std::string lock_arg = args.get("lock", std::string("all"));
    std::vector<std::string> locks =
        lock_arg == "all" ? all_lock_names() : split_csv(lock_arg);
    std::vector<std::string> thread_list =
        split_csv(args.get("threads", std::to_string(NUM_OF_THREADS)));

    if (!args.has("no-header"))
        std::printf("lock,threads,iters,outside_work,n,backoff,"
                    "time_s,time_min_s,time_max_s,cs_per_sec\n");

    for (const auto& name : locks) {
        for (const auto& ts : thread_list) {
            int threads = std::atoi(ts.c_str());
            bool ok = with_lock(name, [&](auto tag) {
                using L = typename decltype(tag)::type;
                std::vector<double> times;
                for (int r = 0; r < reps; ++r)
                    times.push_back(run_once<L>(threads, cfg));
                double med = median(times);
                std::printf("%s,%d,%d,%d,%d,%s,%.6f,%.6f,%.6f,%.1f\n",
                            name.c_str(), threads, cfg.iters, cfg.outside_work,
                            cfg.n,
                            backoff_cfg::mode == backoff_cfg::Mode::Sleep ? "sleep" : "spin",
                            med,
                            *std::min_element(times.begin(), times.end()),
                            *std::max_element(times.begin(), times.end()),
                            (double)threads * cfg.iters / med);
                std::fflush(stdout);
            });
            if (!ok) {
                std::fprintf(stderr, "unknown lock '%s'\n", name.c_str());
                return 1;
            }
        }
    }
    return 0;
}

// bench_barrier.cpp - Part 2: performance of barriers.
//
// Metric (from the assignment): number of completed barrier episodes per
// second with NO useful work between barriers.
//
//   ./bin/bench_barrier --barrier=sense,std_barrier --threads=2,4,8 \
//                       --episodes=200000 --reps=5
//
// Threads are created first and held at a start gate, so thread creation
// cost is not included in the timing.
#include <cstdio>
#include <thread>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "barriers.h"
#include "common.h"

template <class Barrier>
double run_once(int num_threads, long episodes) {
    Barrier barrier(num_threads);
    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) cpu_relax();
            for (long e = 0; e < episodes; ++e)
                barrier.wait(t);
        });
    }
    while (ready.load() != num_threads) cpu_relax();

    auto t0 = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    return seconds_since(t0);
}

#ifdef _OPENMP
double run_once_omp(int num_threads, long episodes) {
    double elapsed = 0;
#pragma omp parallel num_threads(num_threads)
    {
#pragma omp barrier
        double t0 = omp_get_wtime();
        for (long e = 0; e < episodes; ++e) {
#pragma omp barrier
        }
#pragma omp master
        elapsed = omp_get_wtime() - t0;
    }
    return elapsed;
}
#endif

int main(int argc, char** argv) {
    Args args(argc, argv);
    args.apply_backoff();

    long episodes = args.get("episodes", 100000L);
    int reps = (int)args.get("reps", 3L);

    std::string arg = args.get("barrier", std::string("all"));
    std::vector<std::string> names = arg == "all" ? all_barrier_names() : split_csv(arg);
    std::vector<std::string> thread_list = split_csv(args.get("threads", std::string("4")));

    if (!args.has("no-header"))
        std::printf("barrier,threads,episodes,backoff,time_s,episodes_per_sec\n");

    for (const auto& name : names) {
        for (const auto& ts : thread_list) {
            int threads = std::atoi(ts.c_str());
            std::vector<double> times;

            bool ok = true;
#ifdef _OPENMP
            if (name == "omp_barrier") {
                for (int r = 0; r < reps; ++r) times.push_back(run_once_omp(threads, episodes));
            } else
#endif
            ok = with_barrier(name, [&](auto tag) {
                using B = typename decltype(tag)::type;
                for (int r = 0; r < reps; ++r) times.push_back(run_once<B>(threads, episodes));
            });
            if (!ok) {
                std::fprintf(stderr, "unknown barrier '%s'\n", name.c_str());
                return 1;
            }
            double med = median(times);
            std::printf("%s,%d,%ld,%s,%.6f,%.1f\n", name.c_str(), threads, episodes,
                        backoff_cfg::mode == backoff_cfg::Mode::Sleep ? "sleep" : "spin",
                        med, episodes / med);
            std::fflush(stdout);
        }
    }
    return 0;
}

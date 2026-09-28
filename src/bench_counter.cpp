// bench_counter.cpp - Part 3: performance of concurrent counters.
//
// Each of T threads performs --iters increments on one shared counter.
// Metric (from the assignment): execution time (we also print Mops/s).
// --work=k inserts k iterations of a tiny private loop between increments
// to model lower contention (0 = maximum contention).
//
//   ./bin/bench_counter --counter=cas,faa,lock_mcs --threads=1,2,4,8 \
//                       --iters=1000000 --reps=5
#include <cstdio>
#include <thread>
#include <vector>

#include "common.h"
#include "counters.h"

template <class Counter>
double run_once(int num_threads, long iters, int work) {
    Counter counter;
    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) cpu_relax();
            for (long i = 0; i < iters; ++i) {
                counter.increment();
                for (int w = 0; w < work; ++w) do_not_optimize(w);
            }
        });
    }
    while (ready.load() != num_threads) cpu_relax();

    auto t0 = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    double elapsed = seconds_since(t0);

    long expected = (long)num_threads * iters;
    if (counter.get() != expected) {
        std::fprintf(stderr, "COUNTER WRONG: got %ld expected %ld\n", counter.get(), expected);
        std::exit(1);
    }
    return elapsed;
}

int main(int argc, char** argv) {
    Args args(argc, argv);
    args.apply_backoff();

    long iters = args.get("iters", 1000000L);
    int  work  = (int)args.get("work", 0L);
    int  reps  = (int)args.get("reps", 3L);

    std::string arg = args.get("counter", std::string("all"));
    std::vector<std::string> names = arg == "all" ? all_counter_names() : split_csv(arg);
    std::vector<std::string> thread_list = split_csv(args.get("threads", std::string("4")));

    if (!args.has("no-header"))
        std::printf("counter,threads,iters,work,backoff,time_s,mops_per_sec\n");

    for (const auto& name : names) {
        for (const auto& ts : thread_list) {
            int threads = std::atoi(ts.c_str());
            bool ok = with_counter(name, [&](auto tag) {
                using C = typename decltype(tag)::type;
                std::vector<double> times;
                for (int r = 0; r < reps; ++r) times.push_back(run_once<C>(threads, iters, work));
                double med = median(times);
                std::printf("%s,%d,%ld,%d,%s,%.6f,%.2f\n", name.c_str(), threads, iters, work,
                            backoff_cfg::mode == backoff_cfg::Mode::Sleep ? "sleep" : "spin",
                            med, (double)threads * iters / med / 1e6);
                std::fflush(stdout);
            });
            if (!ok) {
                std::fprintf(stderr, "unknown counter '%s'\n", name.c_str());
                return 1;
            }
        }
    }
    return 0;
}

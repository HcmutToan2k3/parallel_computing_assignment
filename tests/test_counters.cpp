// test_counters.cpp - Part 3: correctness of the concurrent counters.
//
// increment() returns the PREVIOUS value (fetch-and-increment). T threads
// each call it I times and record every returned value. Afterwards:
//   * the final value must be exactly T * I (no lost updates), and
//   * the returned values, merged and sorted, must be exactly
//     {0, 1, ..., T*I - 1}: no duplicates (two threads never got the same
//     value) and no gaps.
// This is stronger than only checking the final value.
//
// Negative control: RacyCounter ("racy", load + store) must fail.
//
//   ./bin/test_counters                          all counters, all thread counts
//   ./bin/test_counters --only=cas,faa --threads=4 --iters=10000
#include <algorithm>
#include <thread>
#include <vector>

#include "counters.h"
#include "test_common.h"

template <class Counter>
bool test_counter(int threads, long iters, std::string &detail)
{
    Counter counter;
    std::vector<std::vector<long>> seen(threads);
    StartGate gate(threads);
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
    {
        ts.emplace_back([&, t]
                        {
            seen[t].reserve(iters);
            gate.arrive_and_wait();
            for (long i = 0; i < iters; ++i) seen[t].push_back(counter.increment()); });
    }
    for (auto &th : ts)
        th.join();

    long total = (long)threads * iters;
    std::vector<long> all;
    all.reserve(total);
    for (auto &v : seen)
        all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    long dup = 0, bad = 0;
    for (long i = 0; i < total; ++i)
    {
        if (all[i] != i)
            ++bad;
        if (i > 0 && all[i] == all[i - 1])
            ++dup;
    }
    detail = "final=" + std::to_string(counter.get()) + "/" + std::to_string(total) +
             " duplicate_tickets=" + std::to_string(dup);
    return counter.get() == total && bad == 0;
}

int main(int argc, char **argv)
{
    TestRun run("counter", argc, argv);

    std::vector<std::string> counters = all_counter_names();
    if (run.controls)
        counters.push_back("racy");

    for (const auto &name : counters)
    {
        if (!run.want(name))
            continue;
        for (int t : run.thread_counts)
        {
            // Lock-based counters (MCS especially) are very slow when
            // oversubscribed, so use few increments there.
            long iters = run.iters(t, 100000, 200);
            with_counter(name, [&](auto tag)
                         {
                using C = typename decltype(tag)::type;
                std::string detail;
                run.start();
                bool ok = test_counter<C>(t, iters, detail);
                run.report(name, t, ok, name != "racy", detail); });
        }
    }

    return run.finish();
}

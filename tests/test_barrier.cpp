// test_barrier.cpp - Part 2: correctness of the barriers.
//
// Each round k, every thread does:
//     write my slot = k;  wait;  check every slot == k;  wait
//   * A thread that leaves the barrier early sees k-1 in someone's slot.
//   * A thread that runs ahead into round k+1 makes a thread that is still
//     checking see k+1 (the second wait prevents this for a correct barrier).
//   * Slots are plain (non-atomic) longs, so this also checks memory
//     visibility: the barrier must publish every thread's writes. Under
//     ThreadSanitizer a missing acquire/release shows up as a data race.
// Random yields before arriving make threads arrive in random order.
//
// Negative control: NoBarrier ("none") must fail.
//
//   ./bin/test_barrier                         all barriers, all thread counts
//   ./bin/test_barrier --only=sense --threads=4 --iters=1000
#include <thread>
#include <vector>

#include "barriers.h"
#include "test_common.h"

template <class Barrier>
bool test_barrier(int threads, long rounds, std::string &detail)
{
    Barrier barrier(threads);
    struct alignas(CACHE_LINE) Slot
    {
        long v = 0;
    };
    std::vector<Slot> slot(threads);
    std::atomic<long> violations{0};
    StartGate gate(threads);

    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
    {
        ts.emplace_back([&, t]
                        {
            XorShift rng(t + 7);
            gate.arrive_and_wait();
            for (long k = 1; k <= rounds; ++k) {
                // Random delay before arriving -> threads arrive in random order.
                if ((rng.next() & 15) == 0) std::this_thread::yield();
                slot[t].v = k;
                barrier.wait(t);
                for (int j = 0; j < threads; ++j)
                    if (slot[j].v != k) violations.fetch_add(1, std::memory_order_relaxed);
                barrier.wait(t);
            } });
    }
    for (auto &th : ts)
        th.join();
    detail = "rounds=" + std::to_string(rounds) +
             " violations=" + std::to_string(violations.load());
    return violations.load() == 0;
}

int main(int argc, char **argv)
{
    TestRun run("barrier", argc, argv);

    std::vector<std::string> barriers = {"sense", "sense_backoff", "pthread_cond"};
    // libc++'s std::barrier blocks inside the (non-instrumented) libc++
    // dylib, so TSan cannot see its synchronization and reports a false
    // positive. It is still tested in the normal build.
    if (!UNDER_TSAN)
        barriers.push_back("std_barrier");
    if (run.controls)
        barriers.push_back("none");

    for (const auto &name : barriers)
    {
        if (!run.want(name))
            continue;
        for (int t : run.thread_counts)
        {
            // Oversubscribed spinning barriers are slow: waiters occupy the
            // cores the late arrivers need.
            long rounds = run.iters(t, 5000, 100);
            with_barrier(name, [&](auto tag)
                         {
                using B = typename decltype(tag)::type;
                std::string detail;
                run.start();
                bool ok = test_barrier<B>(t, rounds, detail);
                run.report(name, t, ok, name != "none", detail); });
        }
    }

    return run.finish();
}

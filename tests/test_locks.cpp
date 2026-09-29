// test_locks.cpp - Part 1: correctness of the mutex locks.
//
// Three independent checks inside the critical section (CS):
//   (a) occupancy : an atomic counter of threads currently inside the CS;
//                   fetch_add must always return 0.
//   (b) invariant : two plain ints a, b are updated one after another
//                   (a++, maybe yield, b++). Anyone entering the CS must
//                   observe a == b; a second thread inside would see a
//                   half-finished update.
//   (c) total     : a plain long incremented in the CS must equal
//                   threads * iters at the end (no lost updates).
// Random std::this_thread::yield() inside the CS widens the race window so
// violations show up quickly.
//
// Negative control: NoLock ("none") must fail.
//
//   ./bin/test_locks                       all locks, all thread counts
//   ./bin/test_locks --only=mcs --threads=4 --iters=1000
#include <thread>
#include <vector>

#include "locks.h"
#include "test_common.h"

template <class Lock>
bool test_lock(int threads, long iters, std::string &detail)
{
    Lock lock;
    std::atomic<int> inside{0};
    std::atomic<long> violations{0};
    int a = 0, b = 0; // invariant a == b outside of the CS
    long total = 0;
    StartGate gate(threads);

    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
    {
        ts.emplace_back([&, t]
                        {
            XorShift rng(t + 1);
            gate.arrive_and_wait();
            for (long i = 0; i < iters; ++i) {
                lock.acquire();
                if (inside.fetch_add(1, std::memory_order_relaxed) != 0)
                    violations.fetch_add(1, std::memory_order_relaxed);
                if (a != b)
                    violations.fetch_add(1, std::memory_order_relaxed);
                ++a;
                if ((rng.next() & 63) == 0) std::this_thread::yield();
                ++b;
                ++total;
                inside.fetch_sub(1, std::memory_order_relaxed);
                lock.release();
            } });
    }
    for (auto &th : ts)
        th.join();

    long expected = (long)threads * iters;
    detail = "violations=" + std::to_string(violations.load()) +
             " total=" + std::to_string(total) + "/" + std::to_string(expected);
    return violations.load() == 0 && total == expected && a == b;
}

// MCS explicit-node API: one thread may hold two different MCS locks at once
// if it passes a distinct node for each.
bool test_mcs_nested(int threads, long iters, std::string &detail)
{
    MCSLock l1, l2;
    long c1 = 0, c2 = 0;
    StartGate gate(threads);
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
    {
        ts.emplace_back([&]
                        {
            MCSLock::Node n1, n2;
            gate.arrive_and_wait();
            for (long i = 0; i < iters; ++i) {
                l1.acquire(n1);
                ++c1;
                l2.acquire(n2);
                ++c2;
                l2.release(n2);
                l1.release(n1);
            } });
    }
    for (auto &th : ts)
        th.join();
    long expected = (long)threads * iters;
    detail = "c1=" + std::to_string(c1) + " c2=" + std::to_string(c2) +
             " expected=" + std::to_string(expected);
    return c1 == expected && c2 == expected;
}

int main(int argc, char **argv)
{
    TestRun run("lock", argc, argv);

    std::vector<std::string> locks = all_lock_names();
    if (run.controls)
        locks.push_back("none");

    for (const auto &name : locks)
    {
        if (!run.want(name))
            continue;
        for (int t : run.thread_counts)
        {
            // With 2x oversubscription spinlocks are very slow: the holder (or,
            // for MCS, the next thread in the FIFO queue) can be preempted
            // while everybody else spins. Correctness still needs to be
            // checked there, but a hundred iterations are enough.
            long iters = run.iters(t, 20000, 100);
            with_lock(name, [&](auto tag)
                      {
                using L = typename decltype(tag)::type;
                std::string detail;
                run.start();
                bool ok = test_lock<L>(t, iters, detail);
                run.report(name, t, ok, name != "none", detail); });
        }
    }

    if (run.want("mcs_nested"))
    {
        std::string detail;
        run.start();
        bool ok = test_mcs_nested(4, run.iters(4, 20000, 100), detail);
        run.report("mcs_nested", 4, ok, true, detail);
    }

    return run.finish();
}

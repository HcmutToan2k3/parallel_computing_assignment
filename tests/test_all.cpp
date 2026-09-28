// test_all.cpp - correctness tests for Parts 1, 2 and 3.
//
// Testing strategy (see README.md for the full discussion):
//
//  LOCKS  - three independent checks inside the critical section:
//     (a) occupancy : an atomic counter of threads currently inside the CS;
//                     fetch_add must always return 0.
//     (b) invariant : two plain ints a, b are updated one after another
//                     (a++, maybe yield, b++). Anyone entering the CS must
//                     observe a == b; a second thread inside would see a
//                     half-finished update.
//     (c) total     : a plain long incremented in the CS must equal
//                     threads * iters at the end (no lost updates).
//     Random std::this_thread::yield() inside the CS widens the race
//     window so violations show up quickly.
//
//  BARRIER - rounds of: write my slot = k; wait; check all slots == k; wait.
//     If a thread leaves early it sees k-1 in someone's slot; if a thread
//     runs ahead into round k+1 somebody still checking sees k+1.
//     Plain (non-atomic) slots also check memory visibility: the barrier
//     must publish every thread's writes. Under ThreadSanitizer a missing
//     acquire/release would be reported as a data race.
//
//  COUNTER - increment() returns the previous value. Collected over all
//     threads, the returned values must be exactly {0,...,total-1}, each
//     once (no duplicates = no two threads got the same ticket, no gaps).
//
//  Each check is run with several thread counts, including an odd number
//  and more threads than cores (oversubscription -> preemption while
//  holding a lock / waiting at a barrier).
//
//  NEGATIVE CONTROLS - NoLock, NoBarrier and RacyCounter are expected to
//  FAIL. If they passed, the test would be too weak to be trusted.
#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

#include "barriers.h"
#include "common.h"
#include "counters.h"
#include "locks.h"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif
#ifndef UNDER_TSAN
#define UNDER_TSAN 0
#endif

static int g_failures = 0;

static Clock::time_point g_t0;

static void report(const char* part, const std::string& name, int threads,
                   bool passed, bool expect_pass, const std::string& detail) {
    double secs = seconds_since(g_t0);
    const char* status;
    if (expect_pass) {
        status = passed ? "PASS" : "FAIL";
        if (!passed) ++g_failures;
    } else {
        // Negative control: we WANT to detect a failure.
        status = passed ? "WARN (control not detected)" : "PASS (control detected)";
    }
    std::printf("[%-7s] %-18s threads=%-3d %-26s %6.2fs  %s\n", part, name.c_str(),
                threads, status, secs, detail.c_str());
    std::fflush(stdout);
}

// Tiny per-thread PRNG for random yields.
struct XorShift {
    unsigned x;
    explicit XorShift(unsigned seed) : x(seed * 2654435761u + 1) {}
    unsigned next() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
};

// Start gate: every thread waits here until all threads have been created,
// so that they really run concurrently. Without it, with small iteration
// counts, early threads can finish before late ones start and a broken
// implementation would look correct.
class StartGate {
public:
    explicit StartGate(int n) : n_(n) {}
    void arrive_and_wait() {
        ready_.fetch_add(1);
        while (ready_.load() < n_) std::this_thread::yield();
    }

private:
    const int n_;
    std::atomic<int> ready_{0};
};

// ---------------------------------------------------------------------------
// Locks
// ---------------------------------------------------------------------------
template <class Lock>
bool test_lock(int threads, int iters, std::string& detail) {
    Lock lock;
    std::atomic<int>  inside{0};
    std::atomic<long> violations{0};
    int  a = 0, b = 0;       // invariant a == b outside of the CS
    long total = 0;
    StartGate gate(threads);

    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            XorShift rng(t + 1);
            gate.arrive_and_wait();
            for (int i = 0; i < iters; ++i) {
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
            }
        });
    }
    for (auto& th : ts) th.join();

    long expected = (long)threads * iters;
    detail = "violations=" + std::to_string(violations.load()) +
             " total=" + std::to_string(total) + "/" + std::to_string(expected);
    return violations.load() == 0 && total == expected && a == b;
}

// MCS explicit-node API: one thread may hold two different MCS locks at once
// if it passes a distinct node for each.
bool test_mcs_nested(int threads, int iters, std::string& detail) {
    MCSLock l1, l2;
    long c1 = 0, c2 = 0;
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&] {
            MCSLock::Node n1, n2;
            for (int i = 0; i < iters; ++i) {
                l1.acquire(n1);
                ++c1;
                l2.acquire(n2);
                ++c2;
                l2.release(n2);
                l1.release(n1);
            }
        });
    }
    for (auto& th : ts) th.join();
    long expected = (long)threads * iters;
    detail = "c1=" + std::to_string(c1) + " c2=" + std::to_string(c2) +
             " expected=" + std::to_string(expected);
    return c1 == expected && c2 == expected;
}

// ---------------------------------------------------------------------------
// Barriers
// ---------------------------------------------------------------------------
template <class Barrier>
bool test_barrier(int threads, int rounds, std::string& detail) {
    Barrier barrier(threads);
    struct alignas(CACHE_LINE) Slot { long v = 0; };
    std::vector<Slot> slot(threads);
    std::atomic<long> violations{0};
    StartGate gate(threads);

    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
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
            }
        });
    }
    for (auto& th : ts) th.join();
    detail = "rounds=" + std::to_string(rounds) +
             " violations=" + std::to_string(violations.load());
    return violations.load() == 0;
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------
template <class Counter>
bool test_counter(int threads, long iters, std::string& detail) {
    Counter counter;
    std::vector<std::vector<long>> seen(threads);
    StartGate gate(threads);
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            seen[t].reserve(iters);
            gate.arrive_and_wait();
            for (long i = 0; i < iters; ++i) seen[t].push_back(counter.increment());
        });
    }
    for (auto& th : ts) th.join();

    long total = (long)threads * iters;
    std::vector<long> all;
    all.reserve(total);
    for (auto& v : seen) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    long dup = 0, bad = 0;
    for (long i = 0; i < total; ++i) {
        if (all[i] != i) ++bad;
        if (i > 0 && all[i] == all[i - 1]) ++dup;
    }
    detail = "final=" + std::to_string(counter.get()) + "/" + std::to_string(total) +
             " duplicate_tickets=" + std::to_string(dup);
    return counter.get() == total && bad == 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    Args args(argc, argv);
    args.apply_backoff();

    int hw = (int)std::max(2u, std::thread::hardware_concurrency());
    // 2 threads, an odd count, all cores, and 2x oversubscription.
    std::vector<int> thread_counts = {2, 3, hw, 2 * hw};
    // ThreadSanitizer is ~10x slower; use fewer iterations there.
    int scale = UNDER_TSAN ? 10 : 1;
    std::printf("hardware threads: %d%s\n\n", hw, UNDER_TSAN ? "  (ThreadSanitizer build)" : "");

    // Negative controls are racy on purpose. Under TSan they are skipped
    // unless --controls is given (then TSan reports their races, which
    // demonstrates that TSan would catch a broken implementation).
    bool controls = !UNDER_TSAN || args.has("controls");

    // ---- Part 1: locks ----
    std::vector<std::string> locks = all_lock_names();
    if (controls) locks.push_back("none");
    for (const auto& name : locks) {
        for (int t : thread_counts) {
            // With 2x oversubscription spinlocks are very slow: the holder (or,
            // for MCS, the next thread in the FIFO queue) can be preempted
            // while everybody else spins. Correctness still needs to be
            // checked there, but a few hundred iterations are enough.
            int iters = (t > hw ? 100 : 20000) / scale;
            with_lock(name, [&](auto tag) {
                using L = typename decltype(tag)::type;
                std::string detail;
                g_t0 = Clock::now();
                bool ok = test_lock<L>(t, iters, detail);
                report("lock", name, t, ok, name != "none", detail);
            });
        }
    }
    {
        std::string detail;
        g_t0 = Clock::now();
        bool ok = test_mcs_nested(4, 20000 / scale, detail);
        report("lock", "mcs_nested", 4, ok, true, detail);
    }
    std::printf("\n");

    // ---- Part 2: barriers ----
    std::vector<std::string> barriers = {"sense", "sense_backoff", "pthread_cond"};
    // libc++'s std::barrier blocks inside the (non-instrumented) libc++
    // dylib, so TSan cannot see its synchronization and reports a false
    // positive. It is still tested in the normal build.
    if (!UNDER_TSAN) barriers.push_back("std_barrier");
    if (controls) barriers.push_back("none");
    for (const auto& name : barriers) {
        for (int t : thread_counts) {
            int rounds = (t > hw ? 100 : 5000) / scale;
            with_barrier(name, [&](auto tag) {
                using B = typename decltype(tag)::type;
                std::string detail;
                g_t0 = Clock::now();
                bool ok = test_barrier<B>(t, rounds, detail);
                report("barrier", name, t, ok, name != "none", detail);
            });
        }
    }
    std::printf("\n");

    // ---- Part 3: counters ----
    std::vector<std::string> counters = all_counter_names();
    if (controls) counters.push_back("racy");
    for (const auto& name : counters) {
        for (int t : thread_counts) {
            long iters = (t > hw ? 200 : 100000) / scale;
            with_counter(name, [&](auto tag) {
                using C = typename decltype(tag)::type;
                std::string detail;
                g_t0 = Clock::now();
                bool ok = test_counter<C>(t, iters, detail);
                report("counter", name, t, ok, name != "racy", detail);
            });
        }
    }

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL TESTS PASSED", g_failures);
    return g_failures ? 1 : 0;
}

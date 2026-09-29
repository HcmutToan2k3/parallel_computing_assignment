// test_common.h - helpers shared by the three correctness test programs:
//
//   bin/test_locks     Part 1  (tests/test_locks.cpp)
//   bin/test_barrier   Part 2  (tests/test_barrier.cpp)
//   bin/test_counters  Part 3  (tests/test_counters.cpp)
//
// Common ideas used by all three:
//  * Each check runs with several thread counts: 2, an odd count (3), all
//    cores, and 2x the cores (oversubscription -> threads get preempted
//    while holding a lock / waiting at a barrier).
//  * Threads start together behind a StartGate so they really overlap.
//  * NEGATIVE CONTROLS (NoLock, NoBarrier, RacyCounter) go through the same
//    checks and are expected to FAIL. If they passed, the test would be too
//    weak to be trusted.
//
// Command-line options (all programs):
//   --only=a,b     run only these implementations (default: all)
//   --threads=4,8  thread counts to use (default: 2,3,cores,2*cores)
//   --iters=N      iterations / rounds per thread (default: per test)
//   --controls     under ThreadSanitizer, also run the negative controls
//   --backoff=spin|sleep, --min-delay=, --max-delay=   (see common.h)
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "common.h"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif
#ifndef UNDER_TSAN
#define UNDER_TSAN 0
#endif

// Tiny per-thread PRNG for random yields.
struct XorShift
{
    unsigned x;
    explicit XorShift(unsigned seed) : x(seed * 2654435761u + 1) {}
    unsigned next()
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    }
};

// Start gate: every thread waits here until all threads have been created,
// so that they really run concurrently. Without it, with small iteration
// counts, early threads can finish before late ones start and a broken
// implementation would look correct.
class StartGate
{
public:
    explicit StartGate(int n) : n_(n) {}
    void arrive_and_wait()
    {
        ready_.fetch_add(1);
        while (ready_.load() < n_)
            std::this_thread::yield();
    }

private:
    const int n_;
    std::atomic<int> ready_{0};
};

// Parsed options + result bookkeeping for one test program.
class TestRun
{
public:
    TestRun(const char *part, int argc, char **argv) : part_(part), args_(argc, argv)
    {
        args_.apply_backoff();

        hw = (int)std::max(2u, std::thread::hardware_concurrency());
        std::string threads_arg = args_.get("threads", std::string());
        if (threads_arg.empty())
            thread_counts = {2, 3, hw, 2 * hw};
        else
            for (const auto &s : split_csv(threads_arg))
                thread_counts.push_back(std::atoi(s.c_str()));

        only_ = split_csv(args_.get("only", std::string()));
        iters_override_ = args_.get("iters", 0L);
        // Negative controls are racy on purpose. Under TSan they are skipped
        // unless --controls is given (then TSan reports their races, which
        // demonstrates that TSan would catch a broken implementation).
        controls = !UNDER_TSAN || args_.has("controls");

        std::printf("%s tests - hardware threads: %d%s\n\n", part_, hw,
                    UNDER_TSAN ? "  (ThreadSanitizer build)" : "");
    }

    // Is this implementation selected by --only?
    bool want(const std::string &name) const
    {
        return only_.empty() || std::find(only_.begin(), only_.end(), name) != only_.end();
    }

    // Iterations for a run with t threads: `normal` when t <= cores, `oversub`
    // when oversubscribed (spinning is very slow there), divided by 10 under
    // TSan (~10x slower). --iters overrides all of it.
    long iters(int t, long normal, long oversub) const
    {
        if (iters_override_)
            return iters_override_;
        return (t > hw ? oversub : normal) / (UNDER_TSAN ? 10 : 1);
    }

    void start() { t0_ = Clock::now(); }

    // expect_pass = false marks a negative control: it must FAIL.
    void report(const std::string &name, int threads, bool passed, bool expect_pass,
                const std::string &detail)
    {
        double secs = seconds_since(t0_);
        const char *status;
        if (expect_pass)
        {
            status = passed ? "PASS" : "FAIL";
            if (!passed)
                ++failures_;
        }
        else
        {
            status = passed ? "WARN (control not detected)" : "PASS (control detected)";
        }
        std::printf("[%-7s] %-18s threads=%-3d %-26s %6.2fs  %s\n", part_, name.c_str(),
                    threads, status, secs, detail.c_str());
        std::fflush(stdout);
    }

    // Prints the summary; use as the program's exit code.
    int finish() const
    {
        std::printf("\n%s: %s, %d failure(s)\n", part_,
                    failures_ ? "FAILED" : "ALL TESTS PASSED", failures_);
        return failures_ ? 1 : 0;
    }

    int hw;
    std::vector<int> thread_counts;
    bool controls;

private:
    const char *part_;
    Args args_;
    std::vector<std::string> only_;
    long iters_override_;
    int failures_ = 0;
    Clock::time_point t0_;
};

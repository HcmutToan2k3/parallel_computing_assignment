// barriers.h - Part 2: barrier synchronization.
//
// Every barrier is constructed with the number of participating threads
// and exposes   wait(tid)   where tid is in [0, n). (Barriers that do not
// need the id simply ignore it.)
//
//   SenseBarrier          centralized sense-reversing barrier (spinning)
//   SenseBackoffBarrier   same, with exponential backoff while waiting
//   --- existing barriers used as baselines ---
//   StdBarrier            C++20 std::barrier
//   PthreadCondBarrier    mutex + condition variable (macOS has no
//                         pthread_barrier_t, so this is the Pthreads way)
//   (OpenMP's "#pragma omp barrier" is benchmarked directly in
//    bench_barrier.cpp because it only works inside an omp parallel region.)
//   NoBarrier             does nothing: negative control for the tests
#pragma once

#include <atomic>
#include <barrier>
#include <memory>
#include <pthread.h>
#include <vector>

#include "common.h"

// ===========================================================================
// Sense-reversing barrier
//
// Shared state:  count  (threads still to arrive in this episode)
//                sense  (a global flag that flips once per episode)
// Per-thread:    local_sense
//
// wait():
//   local_sense = !local_sense               // the value `sense` will
//                                             // take when this episode ends
//   if fetch_sub(count, 1) == 1:             // I am the last to arrive
//       count = n                            // reset for the next episode
//       sense = local_sense                  // release everybody
//   else:
//       spin until sense == local_sense
//
// Why "sense reversing"? A naive barrier that only resets a counter has a
// race: a fast thread can leave episode k, enter episode k+1 and decrement
// the counter before slow threads have noticed that episode k finished.
// Because the flag alternates true/false/true..., a thread waiting in
// episode k waits for a *different* value than a thread in episode k+1,
// so the barrier can be reused immediately and safely.
//
// Memory ordering:
//   * fetch_sub is acq_rel so the last arriver "sees" everything all other
//     threads did before the barrier;
//   * sense.store(release) / sense.load(acquire) passes that on to every
//     waiter, so after wait() returns all pre-barrier writes of all
//     threads are visible;
//   * count is reset *before* the release store of sense, so a thread that
//     observes the new sense and races into the next episode is guaranteed
//     to see count == n.
// ===========================================================================
template <bool UseBackoff>
class SenseBarrierT {
public:
    explicit SenseBarrierT(int n)
        : n_(n), count_(n), local_sense_(std::make_unique<PaddedBool[]>(n)) {}

    void wait(int tid) {
        bool my_sense = !local_sense_[tid].value;
        local_sense_[tid].value = my_sense;

        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            count_.store(n_, std::memory_order_relaxed);
            sense_.store(my_sense, std::memory_order_release);
        } else if constexpr (UseBackoff) {
            Backoff backoff;
            while (sense_.load(std::memory_order_acquire) != my_sense)
                backoff();
        } else {
            while (sense_.load(std::memory_order_acquire) != my_sense)
                cpu_relax();
        }
    }

private:
    // Each thread's local sense lives on its own cache line; it is only ever
    // touched by its owner.
    struct alignas(CACHE_LINE) PaddedBool { bool value = false; };

    const int n_;
    alignas(CACHE_LINE) std::atomic<int>  count_;
    alignas(CACHE_LINE) std::atomic<bool> sense_{false};
    std::unique_ptr<PaddedBool[]> local_sense_;
};

using SenseBarrier        = SenseBarrierT<false>;
using SenseBackoffBarrier = SenseBarrierT<true>;

// ===========================================================================
// Baselines
// ===========================================================================
class StdBarrier {
public:
    explicit StdBarrier(int n) : b_(n) {}
    void wait(int) { b_.arrive_and_wait(); }

private:
    std::barrier<> b_;
};

// Classic Pthreads barrier with a generation counter (instead of a sense
// flag) so that spurious wake-ups and immediate reuse are handled.
class PthreadCondBarrier {
public:
    explicit PthreadCondBarrier(int n) : n_(n), count_(n) {
        pthread_mutex_init(&m_, nullptr);
        pthread_cond_init(&cv_, nullptr);
    }
    ~PthreadCondBarrier() {
        pthread_cond_destroy(&cv_);
        pthread_mutex_destroy(&m_);
    }
    PthreadCondBarrier(const PthreadCondBarrier&) = delete;
    PthreadCondBarrier& operator=(const PthreadCondBarrier&) = delete;

    void wait(int) {
        pthread_mutex_lock(&m_);
        unsigned long gen = generation_;
        if (--count_ == 0) {
            ++generation_;
            count_ = n_;
            pthread_cond_broadcast(&cv_);
        } else {
            while (gen == generation_)
                pthread_cond_wait(&cv_, &m_);
        }
        pthread_mutex_unlock(&m_);
    }

private:
    const int n_;
    int count_;
    unsigned long generation_ = 0;
    pthread_mutex_t m_;
    pthread_cond_t cv_;
};

// Negative control: does not synchronize at all.
class NoBarrier {
public:
    explicit NoBarrier(int) {}
    void wait(int) {}
};

// ===========================================================================
// Name -> type dispatch.
// ===========================================================================
template <class T> struct barrier_tag { using type = T; };

inline const std::vector<std::string>& all_barrier_names() {
    static const std::vector<std::string> names = {
        "sense", "sense_backoff", "std_barrier", "pthread_cond",
#ifdef _OPENMP
        "omp_barrier",
#endif
    };
    return names;
}

template <class F>
bool with_barrier(const std::string& name, F&& f) {
    if (name == "sense")         { f(barrier_tag<SenseBarrier>{});        return true; }
    if (name == "sense_backoff") { f(barrier_tag<SenseBackoffBarrier>{}); return true; }
    if (name == "std_barrier")   { f(barrier_tag<StdBarrier>{});          return true; }
    if (name == "pthread_cond")  { f(barrier_tag<PthreadCondBarrier>{});  return true; }
    if (name == "none")          { f(barrier_tag<NoBarrier>{});           return true; }
    return false;
}

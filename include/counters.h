// counters.h - Part 3: concurrent counters.
//
// Interface:
//     long increment();   // atomically adds 1, returns the PREVIOUS value
//     long get() const;   // current value (call when no increments run)
//
// Returning the previous value (fetch-and-increment semantics) is what
// makes a strong correctness test possible: across all threads, the
// returned values must be exactly {0, 1, ..., total-1}, each once.
//
//   LockCounter<L>      plain long protected by any lock from locks.h
//                       (tas, ttas, mcs, std::mutex, ...)
//   CASCounter          compare-and-swap retry loop
//   CASBackoffCounter   CAS + exponential backoff after a failed CAS
//   FAACounter          single hardware fetch-and-add
//   RacyCounter         load + store, NOT atomic: negative control
#pragma once

#include <atomic>

#include "common.h"
#include "locks.h"

// ---------------------------------------------------------------------------
// Mutex-lock based: the simplest approach. Only one thread at a time can be
// inside increment(), so a normal (non-atomic) long is enough.
// ---------------------------------------------------------------------------
template <class Lock>
class LockCounter {
public:
    long increment() {
        lock_.acquire();
        long old = value_++;
        lock_.release();
        return old;
    }
    long get() const { return value_; }

private:
    Lock lock_;
    alignas(CACHE_LINE) long value_ = 0;
};

// ---------------------------------------------------------------------------
// Compare-and-swap: read the value, compute value+1, and install it only if
// nobody changed the value in between. On failure compare_exchange_weak
// reloads `old` with the current value and we retry. Lock-free: some thread
// always makes progress, but under heavy contention many CAS attempts fail.
// ---------------------------------------------------------------------------
class CASCounter {
public:
    long increment() {
        long old = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(old, old + 1,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed))
            cpu_relax();
        return old;
    }
    long get() const { return value_.load(std::memory_order_relaxed); }

private:
    alignas(CACHE_LINE) std::atomic<long> value_{0};
};

// CAS + exponential backoff: the pattern given in the assignment text.
class CASBackoffCounter {
public:
    long increment() {
        Backoff backoff;
        long old = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(old, old + 1,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed))
            backoff();
        return old;
    }
    long get() const { return value_.load(std::memory_order_relaxed); }

private:
    alignas(CACHE_LINE) std::atomic<long> value_{0};
};

// ---------------------------------------------------------------------------
// Fetch-and-add: one hardware instruction (LDADD on ARMv8.1+, LOCK XADD on
// x86). It never fails and never retries, so it is wait-free.
// Note: relaxed ordering is sufficient for a pure counter - we only need
// atomicity of the increment, not ordering with other memory.
// ---------------------------------------------------------------------------
class FAACounter {
public:
    long increment() { return value_.fetch_add(1, std::memory_order_relaxed); }
    long get() const { return value_.load(std::memory_order_relaxed); }

private:
    alignas(CACHE_LINE) std::atomic<long> value_{0};
};

// Negative control: read-modify-write done as two separate atomic
// operations. Two threads can read the same value and both write value+1,
// so updates get lost. (Uses relaxed atomics rather than a plain long so the
// program has no undefined behaviour - it is just logically wrong.)
class RacyCounter {
public:
    long increment() {
        long old = value_.load(std::memory_order_relaxed);
        value_.store(old + 1, std::memory_order_relaxed);
        return old;
    }
    long get() const { return value_.load(std::memory_order_relaxed); }

private:
    alignas(CACHE_LINE) std::atomic<long> value_{0};
};

// ===========================================================================
// Name -> type dispatch.
// ===========================================================================
template <class T> struct counter_tag { using type = T; };

inline const std::vector<std::string>& all_counter_names() {
    static const std::vector<std::string> names = {
        "lock_tas", "lock_ttas", "lock_ttas_backoff", "lock_mcs",
        "lock_std_mutex", "cas", "cas_backoff", "faa",
    };
    return names;
}

template <class F>
bool with_counter(const std::string& name, F&& f) {
    if (name == "lock_tas")          { f(counter_tag<LockCounter<TASLock>>{});         return true; }
    if (name == "lock_tas_backoff")  { f(counter_tag<LockCounter<TASBackoffLock>>{});  return true; }
    if (name == "lock_ttas")         { f(counter_tag<LockCounter<TTASLock>>{});        return true; }
    if (name == "lock_ttas_backoff") { f(counter_tag<LockCounter<TTASBackoffLock>>{}); return true; }
    if (name == "lock_mcs")          { f(counter_tag<LockCounter<MCSLock>>{});         return true; }
    if (name == "lock_std_mutex")    { f(counter_tag<LockCounter<StdMutexLock>>{});    return true; }
    if (name == "lock_pthread")      { f(counter_tag<LockCounter<PthreadMutexLock>>{});return true; }
    if (name == "cas")               { f(counter_tag<CASCounter>{});                   return true; }
    if (name == "cas_backoff")       { f(counter_tag<CASBackoffCounter>{});            return true; }
    if (name == "faa")               { f(counter_tag<FAACounter>{});                   return true; }
    if (name == "racy")              { f(counter_tag<RacyCounter>{});                  return true; }
    return false;
}

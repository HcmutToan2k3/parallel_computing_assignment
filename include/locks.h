// locks.h - Part 1: mutual-exclusion locks.
//
// Every lock exposes the same interface used by the assignment's code
// fragment:
//
//      lock.acquire();   ... critical section ...   lock.release();
//
// so any benchmark / test can be instantiated with any lock type.
//
//   TASLock            test-and-set, spins with atomic exchange
//   TASBackoffLock     test-and-set + exponential backoff
//   TTASLock           test-and-test-and-set (spin on a plain read first)
//   TTASBackoffLock    TTAS + exponential backoff
//   MCSLock            queue lock, each thread spins on its own node
//   --- existing locks used as baselines ---
//   StdMutexLock       std::mutex
//   PthreadMutexLock   pthread_mutex_t
//   OmpLock            omp_lock_t   (only when compiled with OpenMP)
//   NoLock             does nothing: a *negative control* for the tests
#pragma once

#include <atomic>
#include <mutex>
#include <pthread.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "common.h"

// ===========================================================================
// TAS - Test-And-Set
//
// acquire: atomically write "true" and look at the old value. If the old
//          value was already true somebody else holds the lock -> retry.
// Problem: every retry is a *write* (exchange), so the cache line holding
//          the flag ping-pongs between all spinning cores, which also slows
//          down the lock holder when it tries to release.
// ===========================================================================
class TASLock {
public:
    void acquire() {
        while (locked_.exchange(true, std::memory_order_acquire))
            cpu_relax();
    }
    void release() { locked_.store(false, std::memory_order_release); }

private:
    alignas(CACHE_LINE) std::atomic<bool> locked_{false};
};

// TAS + exponential backoff: after a failed exchange, wait an exponentially
// growing amount of time so fewer threads hammer the cache line at once.
class TASBackoffLock {
public:
    void acquire() {
        Backoff backoff;
        while (locked_.exchange(true, std::memory_order_acquire))
            backoff();
    }
    void release() { locked_.store(false, std::memory_order_release); }

private:
    alignas(CACHE_LINE) std::atomic<bool> locked_{false};
};

// ===========================================================================
// TTAS - Test-and-Test-And-Set
//
// First *read* the flag until it looks free (reads hit the local cache,
// no coherence traffic while the lock is held), and only then try the
// expensive exchange. When the holder releases, every spinner sees the
// invalidation, and they all race for the exchange; only one wins.
// ===========================================================================
class TTASLock {
public:
    void acquire() {
        for (;;) {
            while (locked_.load(std::memory_order_relaxed))   // test
                cpu_relax();
            if (!locked_.exchange(true, std::memory_order_acquire))  // test-and-set
                return;
        }
    }
    void release() { locked_.store(false, std::memory_order_release); }

private:
    alignas(CACHE_LINE) std::atomic<bool> locked_{false};
};

// TTAS + exponential backoff: back off after *losing* the race, which is
// exactly the moment we know the lock is contended.
class TTASBackoffLock {
public:
    void acquire() {
        Backoff backoff;
        for (;;) {
            while (locked_.load(std::memory_order_relaxed))
                cpu_relax();
            if (!locked_.exchange(true, std::memory_order_acquire))
                return;
            backoff();
        }
    }
    void release() { locked_.store(false, std::memory_order_release); }

private:
    alignas(CACHE_LINE) std::atomic<bool> locked_{false};
};

// ===========================================================================
// MCS queue lock (Mellor-Crummey & Scott, 1991)
//
// Waiting threads form a linked list (FIFO queue). `tail_` points to the
// last node. Each thread spins only on the `locked` flag of *its own*
// node, so a release touches exactly one other core's cache line
// (O(1) coherence traffic per hand-off) and the lock is FIFO-fair.
//
// acquire(node):
//   node.next = null; node.locked = true
//   pred = tail.exchange(&node)          // enqueue ourselves atomically
//   if pred != null:                     // queue was not empty
//       pred->next = &node               // link behind predecessor
//       spin while node.locked           // wait for hand-off
//
// release(node):
//   if node.next == null:                // apparently nobody behind us
//       if CAS(tail, &node, null) return // really nobody -> lock is free
//       wait until node.next != null     // someone is mid-enqueue
//   node.next->locked = false            // hand lock to successor
//
// The assignment's fragment calls acquire()/release() with no arguments, so
// the node is kept in a thread_local variable. Restriction: a thread must
// not hold two MCS locks at the same time using the no-argument API (use
// the explicit-node overloads for that).
// ===========================================================================
class MCSLock {
public:
    struct alignas(CACHE_LINE) Node {
        std::atomic<Node*> next{nullptr};
        std::atomic<bool>  locked{false};
    };

    void acquire(Node& node) {
        node.next.store(nullptr, std::memory_order_relaxed);
        node.locked.store(true, std::memory_order_relaxed);

        // acq_rel: release publishes our node initialisation to the thread
        // that later reads it through tail_; acquire pairs with the previous
        // releaser's CAS on tail_.
        Node* pred = tail_.exchange(&node, std::memory_order_acq_rel);
        if (pred != nullptr) {
            pred->next.store(&node, std::memory_order_release);
            while (node.locked.load(std::memory_order_acquire))
                cpu_relax();
        }
    }

    void release(Node& node) {
        Node* succ = node.next.load(std::memory_order_acquire);
        if (succ == nullptr) {
            Node* expected = &node;
            if (tail_.compare_exchange_strong(expected, nullptr,
                                              std::memory_order_release,
                                              std::memory_order_relaxed))
                return;
            // A new thread swapped itself into tail_ but has not yet
            // written node.next; wait for the link to appear.
            while ((succ = node.next.load(std::memory_order_acquire)) == nullptr)
                cpu_relax();
        }
        succ->locked.store(false, std::memory_order_release);
    }

    void acquire() { acquire(my_node()); }
    void release() { release(my_node()); }

private:
    static Node& my_node() {
        thread_local Node node;
        return node;
    }

    alignas(CACHE_LINE) std::atomic<Node*> tail_{nullptr};
};

// ===========================================================================
// Baselines: existing mutex implementations wrapped in the same interface.
// ===========================================================================
class StdMutexLock {
public:
    void acquire() { m_.lock(); }
    void release() { m_.unlock(); }

private:
    std::mutex m_;
};

class PthreadMutexLock {
public:
    PthreadMutexLock() { pthread_mutex_init(&m_, nullptr); }
    ~PthreadMutexLock() { pthread_mutex_destroy(&m_); }
    PthreadMutexLock(const PthreadMutexLock&) = delete;
    PthreadMutexLock& operator=(const PthreadMutexLock&) = delete;

    void acquire() { pthread_mutex_lock(&m_); }
    void release() { pthread_mutex_unlock(&m_); }

private:
    pthread_mutex_t m_;
};

#ifdef _OPENMP
class OmpLock {
public:
    OmpLock() { omp_init_lock(&l_); }
    ~OmpLock() { omp_destroy_lock(&l_); }
    OmpLock(const OmpLock&) = delete;
    OmpLock& operator=(const OmpLock&) = delete;

    void acquire() { omp_set_lock(&l_); }
    void release() { omp_unset_lock(&l_); }

private:
    omp_lock_t l_;
};
#endif

// Negative control: provides NO mutual exclusion. The correctness tests
// run it too and expect it to FAIL - this proves the tests can actually
// detect a broken lock.
class NoLock {
public:
    void acquire() {}
    void release() {}
};

// ===========================================================================
// Name -> type dispatch, so programs can pick a lock with --lock=<name>.
// with_lock("mcs", [](auto tag){ using L = typename decltype(tag)::type; ...});
// ===========================================================================
template <class T> struct type_tag { using type = T; };

inline const std::vector<std::string>& all_lock_names() {
    static const std::vector<std::string> names = {
        "tas", "tas_backoff", "ttas", "ttas_backoff", "mcs",
        "std_mutex", "pthread_mutex",
#ifdef _OPENMP
        "omp_lock",
#endif
    };
    return names;
}

template <class F>
bool with_lock(const std::string& name, F&& f) {
    if (name == "tas")           { f(type_tag<TASLock>{});          return true; }
    if (name == "tas_backoff")   { f(type_tag<TASBackoffLock>{});   return true; }
    if (name == "ttas")          { f(type_tag<TTASLock>{});         return true; }
    if (name == "ttas_backoff")  { f(type_tag<TTASBackoffLock>{});  return true; }
    if (name == "mcs")           { f(type_tag<MCSLock>{});          return true; }
    if (name == "std_mutex")     { f(type_tag<StdMutexLock>{});     return true; }
    if (name == "pthread_mutex") { f(type_tag<PthreadMutexLock>{}); return true; }
#ifdef _OPENMP
    if (name == "omp_lock")      { f(type_tag<OmpLock>{});          return true; }
#endif
    if (name == "none")          { f(type_tag<NoLock>{});           return true; }
    return false;
}

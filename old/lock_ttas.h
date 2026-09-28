#ifndef LOCK_TTAS_H
#define LOCK_TTAS_H

#include <atomic>
#include <thread>

class TTASLock {
private:
    std::atomic<bool> locked{false};

public:
    TTASLock() = default;

    TTASLock(const TTASLock&) = delete;
    TTASLock& operator=(const TTASLock&) = delete;

    void lock()
    {
        for (;;) {

            // First test: read-only spinning
            while (locked.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }

            // Second test: atomic acquisition
            bool expected = false;

            if (locked.compare_exchange_weak(
                    expected,
                    true,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }

    bool try_lock()
    {
        bool expected = false;

        return locked.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    void unlock()
    {
        locked.store(false, std::memory_order_release);
    }
};

#endif // TTAS_H
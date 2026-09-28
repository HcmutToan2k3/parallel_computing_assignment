#pragma once

#include <atomic>

class lock_tas {
private:
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

public:
    void acquire() {
        while (flag.test_and_set(std::memory_order_acquire)) {
            // spin
        }
    }

    void release() {
        flag.clear(std::memory_order_release);
    }
};
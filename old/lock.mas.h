#ifndef MCS_H
#define MCS_H

#include <atomic>
#include <thread>

class MCSLock {
public:
    struct Node {
        std::atomic<Node*> next{nullptr};
        std::atomic<bool> waiting{false};
    };

private:
    std::atomic<Node*> tail{nullptr};

public:
    MCSLock() = default;

    MCSLock(const MCSLock&) = delete;
    MCSLock& operator=(const MCSLock&) = delete;

    void lock(Node& node)
    {
        node.next.store(nullptr, std::memory_order_relaxed);
        node.waiting.store(true, std::memory_order_relaxed);

        // Insert ourselves at the end of the queue
        Node* predecessor =
            tail.exchange(&node, std::memory_order_acq_rel);

        if (predecessor != nullptr) {

            predecessor->next.store(
                &node,
                std::memory_order_release);

            // Spin on our own node
            while (node.waiting.load(
                       std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    }

    void unlock(Node& node)
    {
        Node* successor =
            node.next.load(std::memory_order_acquire);

        // No successor currently exists
        if (successor == nullptr) {

            Node* expected = &node;

            if (tail.compare_exchange_strong(
                    expected,
                    nullptr,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                return;
            }

            // A successor is joining the queue
            while ((successor =
                        node.next.load(
                            std::memory_order_acquire))
                   == nullptr) {
                std::this_thread::yield();
            }
        }

        // Wake up successor
        successor->waiting.store(
            false,
            std::memory_order_release);
    }

    bool try_lock(Node& node)
    {
        node.next.store(nullptr, std::memory_order_relaxed);
        node.waiting.store(false, std::memory_order_relaxed);

        Node* expected = nullptr;

        return tail.compare_exchange_strong(
            expected,
            &node,
            std::memory_order_acquire,
            std::memory_order_relaxed);
    }
};

#endif // MCS_H
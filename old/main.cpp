#include <chrono>
#include <thread>
#include <vector>
#include <iostream>

#include "lock_tas.h"

using namespace std::chrono;

#define NUM_OF_THREADS 5
#define NUM_OF_ITERS 100000
#define OUTSIDE_WORK 5
#define N 5

lock_tas lock;

long long counter = 0;

int is_prime(int a){
    for (int i = 2; i < a/2 + 1 ; i++) {
        if (a % i == 0) {
            return 0;
        }
    }
    return 1;
}

void thread_entry()
{
    for (int i = 0; i < NUM_OF_ITERS; i++) {

        lock.acquire();

        // Critical section
        counter++;
        is_prime(N);

        lock.release();

        // Outside critical section
        for (int j = 0; j < OUTSIDE_WORK; j++)
            is_prime(N);
    }
}

int main()
{
    std::vector<std::thread> threads;

    auto start = high_resolution_clock::now();

    for (int i = 0; i < NUM_OF_THREADS; i++)
        threads.emplace_back(thread_entry);

    for (auto& t : threads)
        t.join();

    auto end = high_resolution_clock::now();

    auto elapsed =
        duration_cast<microseconds>(end - start).count();

    std::cout << "counter = " << counter << '\n';
    std::cout << "time    = " << elapsed << " us\n";

    double operations =
        static_cast<double>(NUM_OF_THREADS) * NUM_OF_ITERS;

    std::cout << "throughput = "
              << operations / (elapsed / 1e6)
              << " lock acquisitions/sec\n";
}
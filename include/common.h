// common.h - small utilities shared by every lock / barrier / counter and
// by the benchmark and test programs.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Cache line size.
// Apple Silicon (M1..M5) uses 128-byte cache lines; x86 uses 64.
// Every hot atomic is padded to its own line to avoid *false sharing*
// (two unrelated variables on one line would bounce between cores).
// ---------------------------------------------------------------------------
#if defined(__APPLE__) && defined(__aarch64__)
constexpr std::size_t CACHE_LINE = 128;
#else
constexpr std::size_t CACHE_LINE = 64;
#endif

// ---------------------------------------------------------------------------
// cpu_relax(): hint to the core that we are in a spin-wait loop.
// On x86 this is PAUSE, on ARM it is YIELD. It saves power and, on SMT
// cores, gives the sibling hyper-thread more resources. It does NOT
// give up the time slice (that would be std::this_thread::yield()).
// ---------------------------------------------------------------------------
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// ---------------------------------------------------------------------------
// do_not_optimize(x): prevents the compiler from deleting a computation
// whose result is otherwise unused (e.g. the is_prime(N) calls from the
// assignment). Without this, -O2 may remove the "work" entirely.
// ---------------------------------------------------------------------------
template <class T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

// ---------------------------------------------------------------------------
// is_prime(): exactly the primality test given in the assignment.
// It is intentionally naive: cost is ~p divisions when p is prime, which
// lets us control the length of a critical section through N.
// ---------------------------------------------------------------------------
inline int is_prime(int p) {
    int d;
    for (d = 2; d < p; d = d + 1)
        if (p % d == 0)
            return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Exponential backoff (as described in the assignment, section 1).
//
//   unsigned delay = MIN;
//   while (!compare_exchange(...)) {
//       wait(delay);
//       if (delay < MAX_DELAY) delay *= 2;
//   }
//
// Two waiting strategies are provided and chosen at run time:
//   * sleep : std::this_thread::sleep_for(nanoseconds(delay))  (the
//             assignment's snippet). Note: on macOS/Linux the OS timer
//             granularity is microseconds, so even sleep_for(1ns) sleeps
//             for several microseconds and deschedules the thread.
//   * spin  : busy-wait for ~delay nanoseconds using cpu_relax(). Stays
//             on the core; this is what most production spinlocks do.
// ---------------------------------------------------------------------------
namespace backoff_cfg {
enum class Mode { Spin, Sleep };
inline Mode     mode      = Mode::Spin;
inline unsigned min_delay = 16;     // ns
inline unsigned max_delay = 16384;  // ns   (MAX_DELAY)
}  // namespace backoff_cfg

class Backoff {
public:
    Backoff() : delay_(backoff_cfg::min_delay) {}

    void operator()() {
        if (backoff_cfg::mode == backoff_cfg::Mode::Sleep) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(delay_));
        } else {
            // A cpu_relax() is roughly a few ns; the exact constant does not
            // matter, only that the wait grows exponentially.
            for (unsigned i = 0; i < delay_ / 4 + 1; ++i) cpu_relax();
        }
        if (delay_ < backoff_cfg::max_delay) delay_ *= 2;
    }

private:
    unsigned delay_;
};

// ---------------------------------------------------------------------------
// Timing helper.
// ---------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

inline double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

inline double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// ---------------------------------------------------------------------------
// Minimal "--key=value" command-line parser.
// ---------------------------------------------------------------------------
class Args {
public:
    Args(int argc, char** argv) : argc_(argc), argv_(argv) {}

    std::string get(const char* key, const std::string& def) const {
        std::string prefix = std::string("--") + key + "=";
        for (int i = 1; i < argc_; ++i)
            if (std::strncmp(argv_[i], prefix.c_str(), prefix.size()) == 0)
                return argv_[i] + prefix.size();
        return def;
    }
    long get(const char* key, long def) const {
        std::string s = get(key, std::string());
        return s.empty() ? def : std::strtol(s.c_str(), nullptr, 10);
    }
    bool has(const char* key) const {
        std::string flag = std::string("--") + key;
        for (int i = 1; i < argc_; ++i)
            if (flag == argv_[i]) return true;
        return false;
    }

    // Applies --backoff=spin|sleep, --min-delay=, --max-delay=.
    void apply_backoff() const {
        std::string m = get("backoff", std::string("spin"));
        backoff_cfg::mode = (m == "sleep") ? backoff_cfg::Mode::Sleep
                                           : backoff_cfg::Mode::Spin;
        backoff_cfg::min_delay = (unsigned)get("min-delay", (long)backoff_cfg::min_delay);
        backoff_cfg::max_delay = (unsigned)get("max-delay", (long)backoff_cfg::max_delay);
    }

private:
    int argc_;
    char** argv_;
};

// Split "a,b,c" into {"a","b","c"}.
inline std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find(',', start);
        if (end == std::string::npos) end = s.size();
        if (end > start) out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

#pragma once

#include <atomic>

#include <cstdlib>

namespace stdlike {

class Mutex {
public:
    void Lock() {
        uint32_t expected = 0;
        if (state_.compare_exchange_strong(expected, 1, std::memory_order::acquire)) {
            return;
        }

        while (1) {
            expected = state_.load(std::memory_order::relaxed);
            if (!expected) {
                if (state_.compare_exchange_strong(expected, 1, std::memory_order::acquire)) {
                    return;
                }
                continue;
            }
            if (expected == 1 && state_.compare_exchange_strong(expected, 2, std::memory_order::acquire)) {
                expected = 2;
                state_.wait(expected);
            }
        }
    }

    void Unlock() {
        uint32_t prev = state_.fetch_sub(1, std::memory_order_release);
        if (prev != 1){
            state_.store(0, std::memory_order_release);
            state_.notify_one();
        }
    }

private:
    std::atomic<uint32_t> state_{0};
};

}  // namespace stdlike

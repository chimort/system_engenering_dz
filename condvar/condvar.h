#pragma once

#include <cstdint>
#include <atomic>

namespace stdlike {

class CondVar {
public:

    CondVar() : waiters_{0}, seq_{0} {}   

    template <class Mutex>
    void Wait(Mutex& mutex) {
        uint64_t gen = seq_.load(std::memory_order_acquire);
        waiters_.fetch_add(1, std::memory_order_relaxed);
        
        mutex.unlock();

        if (seq_.load(std::memory_order_acquire) == gen) {
            seq_.wait(gen, std::memory_order_relaxed);
        }

        waiters_.fetch_sub(1, std::memory_order_relaxed);
        mutex.lock();
    }

    void NotifyOne() {
        if(waiters_.load(std::memory_order_acquire) > 0) {
            seq_.fetch_add(1, std::memory_order_release);
            seq_.notify_one();
        }
    }

    void NotifyAll() {
        if (waiters_.load(std::memory_order_acquire) > 0) {
            seq_.fetch_add(1, std::memory_order_release);
            seq_.notify_all();
        }
    }

private:
    std::atomic<int> waiters_;
    std::atomic<uint64_t> seq_;
};

}  // namespace stdlike

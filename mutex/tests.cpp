#include "tests.h"

#include "mutex.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TestCase {
    const char* name;
    void (*fn)();
};

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestLockUnlockSingleThread() {
    stdlike::Mutex mutex;
    mutex.Lock();
    mutex.Unlock();
    mutex.Lock();
    mutex.Unlock();
}

void TestMutualExclusionUnderContention() {
    stdlike::Mutex mutex;
    int counter = 0;
    constexpr int kThreads = 4;
    constexpr int kIterations = 20000;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            for (int iter = 0; iter < kIterations; ++iter) {
                mutex.Lock();
                ++counter;
                mutex.Unlock();
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    Require(counter == kThreads * kIterations, "Mutex should protect the shared counter");
}

void TestWaiterEventuallyAcquiresLock() {
    stdlike::Mutex mutex;
    std::atomic<bool> waiter_acquired{false};

    mutex.Lock();
    std::thread waiter([&] {
        mutex.Lock();
        waiter_acquired.store(true, std::memory_order_release);
        mutex.Unlock();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Require(!waiter_acquired.load(std::memory_order_acquire), "Waiter should block while lock is held");

    mutex.Unlock();
    waiter.join();

    Require(waiter_acquired.load(std::memory_order_acquire), "Waiter should acquire lock after unlock");
}

}  // namespace

int RunMutexTests() {
    const TestCase tests[] = {
        {"lock_unlock_single_thread", &TestLockUnlockSingleThread},
        {"mutual_exclusion_under_contention", &TestMutualExclusionUnderContention},
        {"waiter_eventually_acquires_lock", &TestWaiterEventuallyAcquiresLock},
    };

    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": unknown error\n";
        }
    }

    if (failed == 0) {
        std::cout << "All mutex tests passed\n";
    }

    return failed == 0 ? 0 : 1;
}

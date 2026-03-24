#include "tests.h"

#include "condvar.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct TestCase {
    const char* name;
    void (*fn)();
};

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestNotifyOneWakesSingleWaiter() {
    stdlike::CondVar cv;
    std::mutex mutex;
    int permits = 0;
    int resumed = 0;

    std::vector<std::thread> workers;
    workers.reserve(2);
    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([&] {
            std::unique_lock<std::mutex> lock(mutex);
            while (permits == 0) {
                cv.Wait(mutex);
            }
            --permits;
            ++resumed;
        });
    }

    std::this_thread::sleep_for(50ms);

    {
        std::lock_guard<std::mutex> lock(mutex);
        permits = 1;
    }
    cv.NotifyOne();

    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (resumed == 1) {
                break;
            }
        }
        std::this_thread::sleep_for(10ms);
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        Require(resumed == 1, "A single permit should let only one waiter pass after NotifyOne");
        permits = 2;
    }

    cv.NotifyAll();
    for (auto& worker : workers) {
        worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        Require(resumed == 2, "Both waiters should eventually resume");
    }
}

void TestNotifyAllWakesAllWaiters() {
    stdlike::CondVar cv;
    std::mutex mutex;
    int permits = 0;
    int resumed = 0;

    std::vector<std::thread> workers;
    workers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        workers.emplace_back([&] {
            std::unique_lock<std::mutex> lock(mutex);
            while (permits == 0) {
                cv.Wait(mutex);
            }
            --permits;
            ++resumed;
        });
    }

    std::this_thread::sleep_for(50ms);

    {
        std::lock_guard<std::mutex> lock(mutex);
        permits = 3;
    }
    cv.NotifyAll();

    for (auto& worker : workers) {
        worker.join();
    }

    std::lock_guard<std::mutex> lock(mutex);
    Require(resumed == 3, "NotifyAll should wake all waiters");
}

void TestWaitReacquiresMutex() {
    stdlike::CondVar cv;
    std::mutex mutex;
    bool ready = false;
    bool lock_was_held_after_wait = false;

    std::thread worker([&] {
        std::unique_lock<std::mutex> lock(mutex);
        while (!ready) {
            cv.Wait(mutex);
        }
        lock_was_held_after_wait = !mutex.try_lock();
        if (!lock_was_held_after_wait) {
            mutex.unlock();
        }
    });

    std::this_thread::sleep_for(50ms);

    {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
    }
    cv.NotifyOne();

    worker.join();
    Require(lock_was_held_after_wait, "Wait should reacquire the mutex before returning");
}

}  // namespace

int RunCondVarTests() {
    const TestCase tests[] = {
        {"notify_one_wakes_single_waiter", &TestNotifyOneWakesSingleWaiter},
        {"notify_all_wakes_all_waiters", &TestNotifyAllWakesAllWaiters},
        {"wait_reacquires_mutex", &TestWaitReacquiresMutex},
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
        std::cout << "All condvar tests passed\n";
    }

    return failed == 0 ? 0 : 1;
}

#include "tests.h"

#include "future.h"
#include "promise.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

void TestSetValueAndGet() {
    stdlike::Promise<int> promise;
    auto future = promise.MakeFuture();

    std::thread producer([&] {
        std::this_thread::sleep_for(20ms);
        promise.SetValue(42);
    });

    const int value = future.Get();
    producer.join();
    Require(value == 42, "Future should return the promised value");
}

void TestSetExceptionPropagates() {
    stdlike::Promise<int> promise;
    auto future = promise.MakeFuture();

    std::thread producer([&] {
        promise.SetException(std::make_exception_ptr(std::runtime_error("boom")));
    });

    bool threw = false;
    try {
        (void)future.Get();
    } catch (const std::runtime_error& ex) {
        threw = true;
        Require(std::string(ex.what()) == "boom", "Future should preserve exception payload");
    }

    producer.join();
    Require(threw, "Future should rethrow a stored exception");
}

void TestBrokenPromiseTurnsIntoException() {
    auto future = [] {
        stdlike::Promise<int> promise;
        return promise.MakeFuture();
    }();

    bool threw = false;
    try {
        (void)future.Get();
    } catch (const std::runtime_error& ex) {
        threw = true;
        Require(std::string(ex.what()).find("Broken promise") != std::string::npos,
                "Broken promise should produce a descriptive exception");
    }

    Require(threw, "Future should fail if promise is destroyed without value");
}

}  // namespace

int RunFutureTests() {
    const TestCase tests[] = {
        {"set_value_and_get", &TestSetValueAndGet},
        {"set_exception_propagates", &TestSetExceptionPropagates},
        {"broken_promise_turns_into_exception", &TestBrokenPromiseTurnsIntoException},
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
        std::cout << "All future tests passed\n";
    }

    return failed == 0 ? 0 : 1;
}

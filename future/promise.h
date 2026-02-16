#pragma once

#include "future.h"

#include <memory>
#include <cassert>
#include <utility>
#include <exception>
#include <stdexcept>

namespace stdlike {

template <typename T>
class Promise {
public:
    Promise() : state_(std::make_shared<detail::SharedState<T>>()) {
    }

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) noexcept = default;

    ~Promise() noexcept {
        if (!state_) {
            return;
        }

        std::lock_guard<std::mutex> lk(state_->m);
        if (!state_->ready) {
            state_->result = std::make_exception_ptr(std::runtime_error("Broken promise: Promise destroyed without setting a value/exception"));
            state_->ready = true;
            state_->cv.notify_one();
        }
    }

    Future<T> MakeFuture() {
        Future<T> f(state_);
        return f;
    }


    void SetValue(T value) {
        std::lock_guard<std::mutex> lk(state_->m);
        state_->result = std::move(value);
        state_->ready = true;
        state_->cv.notify_one();
    }

    void SetException(std::exception_ptr ex) {
        assert(state_ && "SetException on moved-from Promise");
        std::lock_guard<std::mutex> lk(state_->m);
        assert(!state_->ready && "SetValue/SetException called more than once (UB by contract)");
        state_->result = ex;
        state_->ready = true;
        state_->cv.notify_one();
    }

private:
    std::shared_ptr<detail::SharedState<T>> state_;
};

}  // namespace stdlike

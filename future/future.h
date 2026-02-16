#pragma once

#include <variant>
#include <exception>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace stdlike {

template <typename T>
class Promise; 

namespace detail {

template <typename T>
struct SharedState {
    std::variant<std::monostate, T, std::exception_ptr> result{std::monostate{}};
    bool ready = false;
    bool consumed = false; 
    std::mutex m;
    std::condition_variable cv;
};

}  // namespace detail

template <typename T>
class Future {
    template <typename U>
    friend class Promise;

public:
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;

    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) noexcept = default;

    T Get() {
        auto s = state_;
        std::unique_lock<std::mutex> lk(s->m);
        s->cv.wait(lk, [&] { return s->ready; });
        s->consumed = true;

        if (std::holds_alternative<std::exception_ptr>(s->result)) {
            std::exception_ptr ex = std::get<std::exception_ptr>(s->result);
            lk.unlock();
            std::rethrow_exception(ex);
        }

        T value = std::move(std::get<T>(s->result));
        s->result = std::monostate{};
        return value;
    }

private:
    Future() = default;
    explicit Future(std::shared_ptr<detail::SharedState<T>> s) : state_(std::move(s)) {}

    std::shared_ptr<detail::SharedState<T>> state_;
};

}  // namespace stdlike

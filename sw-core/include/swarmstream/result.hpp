// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#pragma once

#include <concepts>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace swarmstream {

template <typename E>
class Unexpected {
public:
    constexpr explicit Unexpected(const E& e) : error_(e) {}
    constexpr explicit Unexpected(E&& e) : error_(std::move(e)) {}

    constexpr const E& error() const& noexcept { return error_; }
    constexpr E& error() & noexcept { return error_; }
    constexpr const E&& error() const&& noexcept { return std::move(error_); }
    constexpr E&& error() && noexcept { return std::move(error_); }

private:
    E error_;
};

template <typename E>
Unexpected(E) -> Unexpected<E>;

template <typename T, typename E>
class Result {
public:
    using value_type = T;
    using error_type = E;
    using unexpected_type = Unexpected<E>;

    constexpr Result(const T& val) : storage_(val) {}
    constexpr Result(T&& val) : storage_(std::move(val)) {}
    constexpr Result(const Unexpected<E>& unexp) : storage_(unexp) {}
    constexpr Result(Unexpected<E>&& unexp) : storage_(std::move(unexp)) {}

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] constexpr const T& value() const& {
        if (!has_value()) {
            throw std::runtime_error("Bad result access: value requested from error result");
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] constexpr T& value() & {
        if (!has_value()) {
            throw std::runtime_error("Bad result access: value requested from error result");
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] constexpr T&& value() && {
        if (!has_value()) {
            throw std::runtime_error("Bad result access: value requested from error result");
        }
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] constexpr const T* operator->() const noexcept {
        return &std::get<T>(storage_);
    }

    [[nodiscard]] constexpr T* operator->() noexcept {
        return &std::get<T>(storage_);
    }

    [[nodiscard]] constexpr const T& operator*() const& noexcept {
        return std::get<T>(storage_);
    }

    [[nodiscard]] constexpr T& operator*() & noexcept {
        return std::get<T>(storage_);
    }

    [[nodiscard]] constexpr const E& error() const& noexcept {
        return std::get<Unexpected<E>>(storage_).error();
    }

    [[nodiscard]] constexpr E& error() & noexcept {
        return std::get<Unexpected<E>>(storage_).error();
    }

    [[nodiscard]] constexpr E&& error() && noexcept {
        return std::get<Unexpected<E>>(std::move(storage_)).error();
    }

private:
    std::variant<T, Unexpected<E>> storage_;
};

// Specialization for void value type: Result<void, E>
template <typename E>
class Result<void, E> {
public:
    using value_type = void;
    using error_type = E;
    using unexpected_type = Unexpected<E>;

    constexpr Result() noexcept : has_value_(true) {}
    constexpr Result(const Unexpected<E>& unexp) : has_value_(false), error_(unexp.error()) {}
    constexpr Result(Unexpected<E>&& unexp) : has_value_(false), error_(std::move(unexp.error())) {}

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return has_value_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return has_value_;
    }

    constexpr void value() const {
        if (!has_value_) {
            throw std::runtime_error("Bad result access: void value requested from error result");
        }
    }

    [[nodiscard]] constexpr const E& error() const& noexcept {
        return error_;
    }

    [[nodiscard]] constexpr E& error() & noexcept {
        return error_;
    }

    [[nodiscard]] constexpr E&& error() && noexcept {
        return std::move(error_);
    }

private:
    bool has_value_{true};
    E error_{};
};

} // namespace swarmstream

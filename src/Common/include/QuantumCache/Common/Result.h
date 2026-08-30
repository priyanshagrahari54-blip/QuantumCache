#pragma once
#include "QuantumCache/Common/ErrorCode.h"
#include <optional>
#include <utility>

namespace QuantumCache::Common {

// A minimal, dependency-free Result<T> used across all Stage 1 modules so
// that component boundaries communicate success/failure explicitly instead
// of relying on exceptions or sentinel values. This is real, used code —
// not a placeholder.
template <typename T>
class Result {
public:
    static Result Success(T value) { return Result(std::move(value)); }
    static Result Failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool IsOk() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return IsOk(); }

    [[nodiscard]] const T& Value() const& { return value_.value(); }
    [[nodiscard]] T& Value() & { return value_.value(); }
    [[nodiscard]] T&& Value() && { return std::move(value_.value()); }

    [[nodiscard]] const Error& Err() const& { return error_; }

private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<T> value_;
    Error error_{ErrorCode::Unknown, "uninitialized result"};
};

// Specialization for operations with no return value beyond success/failure.
template <>
class Result<void> {
public:
    static Result Success() { return Result(true, Error{}); }
    static Result Failure(Error error) { return Result(false, std::move(error)); }

    [[nodiscard]] bool IsOk() const noexcept { return ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const Error& Err() const& { return error_; }

private:
    Result(bool ok, Error error) : ok_(ok), error_(std::move(error)) {}
    bool ok_;
    Error error_;
};

} // namespace QuantumCache::Common

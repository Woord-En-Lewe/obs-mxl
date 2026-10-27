// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <string_view>

namespace obsmxl
{
/**
 * Result of an operation on a flow or on the underlying MXL backend.
 * Mirrors the subset of mxlStatus that the plugin actually needs to
 * distinguish, without leaking the C SDK into the core library.
 */
enum class Status
{
    Ok = 0,
    Timeout,
    TooEarly,
    TooLate,
    NotFound,
    FlowInvalid,
    Conflict,
    InvalidArgument,
    Unsupported,
    Error,
};

constexpr std::string_view to_string(Status status) noexcept
{
    switch (status)
    {
    case Status::Ok:
        return "ok";
    case Status::Timeout:
        return "timeout";
    case Status::TooEarly:
        return "too-early";
    case Status::TooLate:
        return "too-late";
    case Status::NotFound:
        return "not-found";
    case Status::FlowInvalid:
        return "flow-invalid";
    case Status::Conflict:
        return "conflict";
    case Status::InvalidArgument:
        return "invalid-argument";
    case Status::Unsupported:
        return "unsupported";
    case Status::Error:
        return "error";
    }
    return "unknown";
}

constexpr bool is_ok(Status status) noexcept
{
    return status == Status::Ok;
}
} // namespace obsmxl

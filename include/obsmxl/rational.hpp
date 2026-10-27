// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <cstdint>
#include <string>

namespace obsmxl
{
/** A rational number, as used by NMOS/MXL for grain and sample rates. */
struct Rational
{
    std::uint32_t numerator{0};
    std::uint32_t denominator{1};

    constexpr bool valid() const noexcept { return (numerator != 0) && (denominator != 0); }

    constexpr double to_double() const noexcept
    {
        return valid() ? (static_cast<double>(numerator) / static_cast<double>(denominator)) : 0.0;
    }

    friend constexpr bool operator==(Rational const& lhs, Rational const& rhs) noexcept
    {
        // Cross-multiply to compare without floating point.
        return (static_cast<std::uint64_t>(lhs.numerator) * rhs.denominator) ==
               (static_cast<std::uint64_t>(rhs.numerator) * lhs.denominator);
    }

    friend constexpr bool operator!=(Rational const& lhs, Rational const& rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

std::string to_string(Rational const& value);
} // namespace obsmxl

// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <atomic>
#include <cstdint>

#include <obsmxl/rational.hpp>

namespace obsmxl
{
using Ns = std::int64_t;

inline constexpr std::uint64_t undefined_index = ~std::uint64_t{0};

/**
 * Time source used by the pipelines.
 *
 * host_now_ns() must use the clock that the host uses for media timestamps
 * (OBS: os_gettime_ns(), i.e. CLOCK_MONOTONIC based). tai_now_ns() must be
 * the SMPTE ST 2059 TAI epoch used by MXL.
 */
class Clock
{
public:
    virtual ~Clock() = default;
    virtual Ns host_now_ns() const noexcept = 0;
    virtual Ns tai_now_ns() const noexcept = 0;
};

/** Process-wide clock backed by CLOCK_TAI (Linux) or CLOCK_REALTIME+offset. */
class SystemClock final : public Clock
{
public:
    static SystemClock& instance() noexcept;

    Ns host_now_ns() const noexcept override;
    Ns tai_now_ns() const noexcept override;

private:
    SystemClock() = default;
};

/**
 * Number of nanoseconds between the UTC and TAI epochs that is used when
 * the platform has no CLOCK_TAI (currently macOS). 37s is the TAI-UTC
 * offset in effect since 2017-01-01.
 */
inline constexpr Ns default_tai_utc_offset_ns = 37'000'000'000LL;

bool has_native_tai_clock() noexcept;

/** Calibrates and caches the host->TAI offset. Thread-safe. */
class ClockOffset
{
public:
    /**
     * Samples both clocks. Recalibrates at most once per
     * min_interval_ns; pass 0 to force a sample. Returns false when the
     * clock could not be read.
     */
    bool calibrate(Clock const& clock, Ns min_interval_ns = 1'000'000'000LL) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool calibrated() const noexcept;
    [[nodiscard]] Ns offset_ns() const noexcept;

    /** host = tai + offset */
    [[nodiscard]] Ns to_host(Ns tai_ns) const noexcept;
    [[nodiscard]] Ns to_tai(Ns host_ns) const noexcept;

private:
    std::atomic<Ns> offset_{0};
    std::atomic<Ns> sampled_at_host_{0};
    std::atomic<bool> calibrated_{false};
};

/**
 * Converts a timestamp to a flow index using the same rounding as the MXL
 * SDK (nearest index). Index 0 is the SMPTE ST 2059 epoch.
 */
std::uint64_t timestamp_to_index(Rational const& rate, Ns timestamp_ns) noexcept;

/** Converts a flow index back to a timestamp (nearest nanosecond). */
Ns index_to_timestamp(Rational const& rate, std::uint64_t index) noexcept;

/** Nanoseconds covered by one index of the given rate, rounded down. */
Ns index_duration_ns(Rational const& rate) noexcept;
} // namespace obsmxl

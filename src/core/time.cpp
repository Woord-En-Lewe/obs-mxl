// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/time.hpp>

#include <ctime>
#include <string>

namespace obsmxl
{
namespace
{
Ns read_clock(clockid_t clock_id) noexcept
{
    timespec ts{};
    if (::clock_gettime(clock_id, &ts) != 0)
    {
        return 0;
    }
    return static_cast<Ns>(ts.tv_sec) * 1'000'000'000LL + static_cast<Ns>(ts.tv_nsec);
}
} // namespace

bool has_native_tai_clock() noexcept
{
#if defined(CLOCK_TAI)
    return true;
#else
    return false;
#endif
}

SystemClock& SystemClock::instance() noexcept
{
    static SystemClock clock;
    return clock;
}

Ns SystemClock::host_now_ns() const noexcept
{
    return read_clock(CLOCK_MONOTONIC);
}

Ns SystemClock::tai_now_ns() const noexcept
{
#if defined(CLOCK_TAI)
    auto const tai = read_clock(CLOCK_TAI);
    if (tai != 0)
    {
        return tai;
    }
#endif
    // Fallback for platforms without a TAI clock and for environments where
    // CLOCK_TAI is unavailable (for example restricted containers).
    return read_clock(CLOCK_REALTIME) + default_tai_utc_offset_ns;
}

bool ClockOffset::calibrate(Clock const& clock, Ns min_interval_ns) noexcept
{
    Ns const host_before = clock.host_now_ns();
    Ns const tai = clock.tai_now_ns();
    Ns const host_after = clock.host_now_ns();

    if ((host_before == 0) || (host_after == 0) || (tai == 0))
    {
        return false;
    }

    Ns const host_mid = host_before + (host_after - host_before) / 2;

    if (calibrated_.load(std::memory_order_acquire) && (min_interval_ns > 0))
    {
        Ns const previous = sampled_at_host_.load(std::memory_order_relaxed);
        if ((host_mid >= previous) && ((host_mid - previous) < min_interval_ns))
        {
            return true;
        }
    }

    offset_.store(host_mid - tai, std::memory_order_release);
    sampled_at_host_.store(host_mid, std::memory_order_relaxed);
    calibrated_.store(true, std::memory_order_release);
    return true;
}

void ClockOffset::reset() noexcept
{
    calibrated_.store(false, std::memory_order_release);
    sampled_at_host_.store(0, std::memory_order_relaxed);
    offset_.store(0, std::memory_order_relaxed);
}

bool ClockOffset::calibrated() const noexcept
{
    return calibrated_.load(std::memory_order_acquire);
}

Ns ClockOffset::offset_ns() const noexcept
{
    return offset_.load(std::memory_order_acquire);
}

Ns ClockOffset::to_host(Ns tai_ns) const noexcept
{
    return tai_ns + offset_.load(std::memory_order_acquire);
}

Ns ClockOffset::to_tai(Ns host_ns) const noexcept
{
    return host_ns - offset_.load(std::memory_order_acquire);
}

std::uint64_t timestamp_to_index(Rational const& rate, Ns timestamp_ns) noexcept
{
    if (!rate.valid() || (timestamp_ns < 0))
    {
        return undefined_index;
    }

    // Mirrors mxl::lib::timestampToIndex: round to the nearest index.
    __int128 const numerator = static_cast<__int128>(timestamp_ns) * rate.numerator +
                               500'000'000LL * static_cast<__int128>(rate.denominator);
    __int128 const denominator = 1'000'000'000LL * static_cast<__int128>(rate.denominator);
    return static_cast<std::uint64_t>(numerator / denominator);
}

Ns index_to_timestamp(Rational const& rate, std::uint64_t index) noexcept
{
    if (!rate.valid())
    {
        return 0;
    }

    __int128 const numerator = static_cast<__int128>(index) * rate.denominator * 1'000'000'000LL +
                               static_cast<__int128>(rate.numerator) / 2;
    return static_cast<Ns>(numerator / static_cast<__int128>(rate.numerator));
}

Ns index_duration_ns(Rational const& rate) noexcept
{
    if (!rate.valid())
    {
        return 0;
    }
    return static_cast<Ns>((static_cast<__int128>(rate.denominator) * 1'000'000'000LL) /
                           rate.numerator);
}

std::string to_string(Rational const& value)
{
    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}
} // namespace obsmxl

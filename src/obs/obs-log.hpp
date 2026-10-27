// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <obsmxl/log.hpp>
#include <obsmxl/time.hpp>

namespace obsmxl::obs
{
/** Logger that forwards core messages to libobs (blog). */
class ObsLogger final : public Logger
{
public:
    void log(LogLevel level, std::string_view message) override;
};

ObsLogger& logger();

/**
 * Clock aligned with libobs: os_gettime_ns() for host timestamps and the
 * system TAI clock for MXL timestamps.
 */
class ObsClock final : public Clock
{
public:
    Ns host_now_ns() const noexcept override;
    Ns tai_now_ns() const noexcept override;
};

ObsClock& clock();
} // namespace obsmxl::obs

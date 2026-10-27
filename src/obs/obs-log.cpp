// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include "obs-log.hpp"

#include <obs-module.h>
#include <util/platform.h>

namespace obsmxl::obs
{
void ObsLogger::log(LogLevel level, std::string_view message)
{
    int obs_level = LOG_INFO;
    switch (level)
    {
    case LogLevel::Debug:
        obs_level = LOG_DEBUG;
        break;
    case LogLevel::Info:
        obs_level = LOG_INFO;
        break;
    case LogLevel::Warning:
        obs_level = LOG_WARNING;
        break;
    case LogLevel::Error:
        obs_level = LOG_ERROR;
        break;
    }

    blog(obs_level, "[obs-mxl] %.*s", static_cast<int>(message.size()), message.data());
}

ObsLogger& logger()
{
    static ObsLogger instance;
    return instance;
}

Ns ObsClock::host_now_ns() const noexcept
{
    return static_cast<Ns>(os_gettime_ns());
}

Ns ObsClock::tai_now_ns() const noexcept
{
    return SystemClock::instance().tai_now_ns();
}

ObsClock& clock()
{
    static ObsClock instance;
    return instance;
}
} // namespace obsmxl::obs

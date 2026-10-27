// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/log.hpp>

#include <cstdio>

namespace obsmxl
{
std::string_view to_string(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    }
    return "unknown";
}

StderrLogger::StderrLogger(LogLevel minimum) noexcept : minimum_(minimum) {}

void StderrLogger::log(LogLevel level, std::string_view message)
{
    if (static_cast<int>(level) < static_cast<int>(minimum_))
    {
        return;
    }

    std::fprintf(stderr, "[obs-mxl:%s] %.*s\n", to_string(level).data(),
                 static_cast<int>(message.size()), message.data());
}
} // namespace obsmxl

// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <string_view>

namespace obsmxl
{
enum class LogLevel
{
    Debug,
    Info,
    Warning,
    Error,
};

std::string_view to_string(LogLevel level) noexcept;

/**
 * Minimal logging sink. The OBS adapter implements this with blog(), the
 * tests/CLI tools with stderr. May be nullptr at every call site.
 */
class Logger
{
public:
    virtual ~Logger() = default;
    virtual void log(LogLevel level, std::string_view message) = 0;
};

class NullLogger final : public Logger
{
public:
    void log(LogLevel, std::string_view) override {}
};

class StderrLogger final : public Logger
{
public:
    explicit StderrLogger(LogLevel minimum = LogLevel::Info) noexcept;
    void log(LogLevel level, std::string_view message) override;

private:
    LogLevel minimum_;
};
} // namespace obsmxl

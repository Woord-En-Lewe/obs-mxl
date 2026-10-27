// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/domain-options.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <obsmxl/json.hpp>

namespace obsmxl
{
namespace
{
std::filesystem::path options_path(std::string const& domain)
{
    return std::filesystem::path{domain} / "options.json";
}
} // namespace

Status write_domain_history(std::string const& domain, int history_ms, std::string* error)
{
    if (domain.empty() || (history_ms < 50) || (history_ms > 5000))
    {
        if (error)
        {
            *error = "invalid domain or history duration";
        }
        return Status::InvalidArgument;
    }

    std::error_code ec;
    std::filesystem::create_directories(domain, ec);
    if (ec)
    {
        if (error)
        {
            *error = "cannot create domain directory: " + ec.message();
        }
        return Status::Error;
    }

    // MXL currently reads exactly this option from <domain>/options.json.
    // Writing the file is an explicit user action ("Manage domain
    // history"); see docs/implementation-notes.md for the trade-off.
    JsonWriter writer;
    writer.begin_object();
    writer.key(domain_history_option)
        .value(static_cast<std::int64_t>(static_cast<std::int64_t>(history_ms) * 1'000'000LL));
    writer.end_object();

    std::ofstream stream{options_path(domain), std::ios::binary | std::ios::trunc};
    if (!stream)
    {
        if (error)
        {
            *error = "cannot write " + options_path(domain).string();
        }
        return Status::Error;
    }
    stream << writer.str();
    if (!stream)
    {
        if (error)
        {
            *error = "failed to write " + options_path(domain).string();
        }
        return Status::Error;
    }
    return Status::Ok;
}

int read_domain_history(std::string const& domain, int fallback_ms)
{
    std::ifstream stream{options_path(domain), std::ios::binary};
    if (!stream)
    {
        return fallback_ms;
    }
    std::stringstream buffer;
    buffer << stream.rdbuf();

    auto const parsed = JsonValue::parse(buffer.str());
    if (!parsed || !parsed->is_object())
    {
        return fallback_ms;
    }
    auto const* value = parsed->find(domain_history_option);
    if (!value || !value->is_number())
    {
        return fallback_ms;
    }
    auto const ns = static_cast<std::int64_t>(value->as_number());
    auto const ms = ns / 1'000'000LL;
    return (ms >= 50 && ms <= 5000) ? static_cast<int>(ms) : fallback_ms;
}
} // namespace obsmxl

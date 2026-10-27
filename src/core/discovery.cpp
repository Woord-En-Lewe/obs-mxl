// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/discovery.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <obsmxl/json.hpp>

namespace obsmxl
{
namespace
{
constexpr std::size_t max_flow_definition_size = 256U * 1024U;

std::string read_file(std::filesystem::path const& path, std::uintmax_t limit)
{
    std::error_code ec;
    auto const size = std::filesystem::file_size(path, ec);
    if (ec || (size == 0) || (size > limit))
    {
        return {};
    }

    std::ifstream stream{path, std::ios::binary};
    if (!stream)
    {
        return {};
    }

    std::string content;
    content.resize(static_cast<std::size_t>(size));
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream)
    {
        return {};
    }
    return content;
}

Rational read_rational(JsonValue const& root, std::string_view key)
{
    Rational result{0, 1};
    auto const* value = root.find(key);
    if (!value || !value->is_object())
    {
        return result;
    }
    result.numerator = static_cast<std::uint32_t>(value->number_or("numerator", 0.0));
    result.denominator = static_cast<std::uint32_t>(value->number_or("denominator", 1.0));
    if (result.denominator == 0)
    {
        result.denominator = 1;
    }
    return result;
}

std::string format_from_urn(std::string_view urn)
{
    constexpr std::string_view prefix = "urn:x-nmos:format:";
    if (urn.substr(0, prefix.size()) == prefix)
    {
        return std::string{urn.substr(prefix.size())};
    }
    return {};
}
} // namespace

std::string FlowSummary::display_name() const
{
    std::string result = label.empty() ? id : label;
    if (!media_type.empty())
    {
        result += " [";
        result += media_type;
        if (audio)
        {
            if (channel_count > 0)
            {
                result += " " + std::to_string(channel_count) + "ch";
            }
            if (rate.valid())
            {
                result += " " + std::to_string(rate.numerator / rate.denominator) + "Hz";
            }
        }
        else if ((width > 0) && (height > 0))
        {
            result += " " + std::to_string(width) + "x" + std::to_string(height);
            if (rate.valid())
            {
                result += " @" + to_string(rate);
            }
        }
        result += "]";
    }
    if (!id.empty())
    {
        result += "  " + id;
    }
    return result;
}

std::optional<FlowSummary> read_flow_summary(std::filesystem::path const& flow_directory)
{
    auto const definition = read_file(flow_directory / "flow_def.json", max_flow_definition_size);
    if (definition.empty())
    {
        return std::nullopt;
    }

    std::string error;
    auto const parsed = JsonValue::parse(definition, &error);
    if (!parsed || !parsed->is_object())
    {
        return std::nullopt;
    }

    FlowSummary summary;
    summary.id = parsed->string_or("id");
    if (summary.id.empty())
    {
        auto name = flow_directory.filename().string();
        if (name.size() > flow_directory_suffix.size())
        {
            name.resize(name.size() - flow_directory_suffix.size());
        }
        summary.id = std::move(name);
    }

    summary.label = parsed->string_or("label");
    summary.media_type = parsed->string_or("media_type");
    summary.format = format_from_urn(parsed->string_or("format"));
    summary.audio = (summary.media_type.rfind("audio/", 0) == 0) || (summary.format == "audio");

    if (summary.audio)
    {
        summary.channel_count = static_cast<std::uint32_t>(parsed->number_or("channel_count", 0.0));
        summary.rate = read_rational(*parsed, "sample_rate");
    }
    else
    {
        summary.width = static_cast<std::uint32_t>(parsed->number_or("frame_width", 0.0));
        summary.height = static_cast<std::uint32_t>(parsed->number_or("frame_height", 0.0));
        summary.rate = read_rational(*parsed, "grain_rate");
    }

    return summary;
}

std::vector<FlowSummary> discover_flows(std::filesystem::path const& domain)
{
    std::vector<FlowSummary> result;

    std::error_code ec;
    if (!std::filesystem::is_directory(domain, ec))
    {
        return result;
    }

    for (auto const& entry : std::filesystem::directory_iterator{domain, ec})
    {
        if (ec)
        {
            break;
        }
        if (!entry.is_directory(ec) || ec)
        {
            continue;
        }
        auto const name = entry.path().filename().string();
        if (name.size() <= flow_directory_suffix.size() ||
            name.compare(name.size() - flow_directory_suffix.size(), flow_directory_suffix.size(),
                         flow_directory_suffix) != 0)
        {
            continue;
        }

        if (auto summary = read_flow_summary(entry.path()))
        {
            result.push_back(std::move(*summary));
        }
    }

    std::sort(result.begin(), result.end(),
              [](FlowSummary const& a, FlowSummary const& b)
              {
                  if (a.label != b.label)
                  {
                      return a.label < b.label;
                  }
                  return a.id < b.id;
              });
    return result;
}

std::vector<FlowSummary> discover_video_flows(std::filesystem::path const& domain)
{
    auto flows = discover_flows(domain);
    std::erase_if(flows, [](FlowSummary const& flow) { return flow.audio; });
    return flows;
}

std::vector<FlowSummary> discover_audio_flows(std::filesystem::path const& domain)
{
    auto flows = discover_flows(domain);
    std::erase_if(flows, [](FlowSummary const& flow) { return !flow.audio; });
    return flows;
}
} // namespace obsmxl

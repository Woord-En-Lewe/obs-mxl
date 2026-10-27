// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/settings.hpp>

#include <algorithm>
#include <set>
#include <string>

#include <obsmxl/uuid.hpp>

namespace obsmxl
{
namespace
{
std::string bool_to_string(bool value)
{
    return value ? "true" : "false";
}

bool string_to_bool(std::string const& value, bool fallback)
{
    if ((value == "true") || (value == "1") || (value == "yes"))
    {
        return true;
    }
    if ((value == "false") || (value == "0") || (value == "no"))
    {
        return false;
    }
    return fallback;
}

int string_to_int(std::string const& value, int fallback)
{
    try
    {
        std::size_t consumed = 0;
        int const parsed = std::stoi(value, &consumed);
        return (consumed == value.size()) ? parsed : fallback;
    }
    catch (...)
    {
        return fallback;
    }
}

std::string derived_audio_uuid(std::string const& video_uuid)
{
    if (auto derived = derive_uuid(video_uuid, "/audio"))
    {
        return format_uuid(*derived);
    }
    return {};
}

void write_endpoint(SettingsMap& map, std::string const& prefix,
                    OutputEndpointSettings const& endpoint)
{
    map[prefix + ".enabled"] = bool_to_string(endpoint.enabled);
    map[prefix + ".flow_name"] = endpoint.flow_name;
    map[prefix + ".video_flow_uuid"] = endpoint.video_flow_uuid;
    map[prefix + ".audio_flow_uuid"] = endpoint.audio_flow_uuid;
    map[prefix + ".include_audio"] = bool_to_string(endpoint.include_audio);
}

OutputEndpointSettings read_endpoint(SettingsMap const& map, std::string const& prefix,
                                     OutputEndpointSettings fallback)
{
    auto const get = [&map](std::string const& key) -> std::string
    {
        auto const it = map.find(key);
        return (it != map.end()) ? it->second : std::string{};
    };

    if (auto const value = get(prefix + ".enabled"); !value.empty())
    {
        fallback.enabled = string_to_bool(value, fallback.enabled);
    }
    if (auto const value = get(prefix + ".flow_name"); !value.empty())
    {
        fallback.flow_name = value;
    }
    if (auto const value = get(prefix + ".video_flow_uuid"); !value.empty())
    {
        fallback.video_flow_uuid = value;
    }
    if (auto const value = get(prefix + ".audio_flow_uuid"); !value.empty())
    {
        fallback.audio_flow_uuid = value;
    }
    else
    {
        fallback.audio_flow_uuid = derived_audio_uuid(fallback.video_flow_uuid);
    }
    if (auto const value = get(prefix + ".include_audio"); !value.empty())
    {
        fallback.include_audio = string_to_bool(value, fallback.include_audio);
    }
    return fallback;
}
} // namespace

PluginSettings make_default_settings()
{
    // Deterministic defaults so that an untouched configuration always
    // refers to the same flows across sessions and hosts. Users can
    // generate fresh UUIDs from the dialog at any time.
    auto const namespace_uuid = *parse_uuid(uuid_namespace_url);
    auto const program_video = format_uuid(uuid_v5(namespace_uuid, "obs-mxl/program/video"));
    auto const preview_video = format_uuid(uuid_v5(namespace_uuid, "obs-mxl/preview/video"));

    PluginSettings settings;
    settings.domain_path = "/dev/shm/mxl";
    settings.history_ms = 200;

    settings.program.enabled = false;
    settings.program.flow_name = "OBS Program";
    settings.program.video_flow_uuid = program_video;
    settings.program.audio_flow_uuid = derived_audio_uuid(program_video);
    settings.program.include_audio = true;

    settings.preview.enabled = false;
    settings.preview.flow_name = "OBS Preview";
    settings.preview.video_flow_uuid = preview_video;
    settings.preview.audio_flow_uuid = derived_audio_uuid(preview_video);
    settings.preview.include_audio = false;

    return settings;
}

SettingsMap settings_to_map(PluginSettings const& settings)
{
    SettingsMap map;
    map["domain_path"] = settings.domain_path;
    map["history_ms"] = std::to_string(settings.history_ms);
    map["manage_domain_history"] = bool_to_string(settings.manage_domain_history);
    write_endpoint(map, "program", settings.program);
    write_endpoint(map, "preview", settings.preview);
    return map;
}

PluginSettings settings_from_map(SettingsMap const& map)
{
    auto const defaults = make_default_settings();
    PluginSettings settings = defaults;

    auto const get = [&map](std::string const& key) -> std::string
    {
        auto const it = map.find(key);
        return (it != map.end()) ? it->second : std::string{};
    };

    if (auto const value = get("domain_path"); !value.empty())
    {
        settings.domain_path = value;
    }
    if (auto const value = get("history_ms"); !value.empty())
    {
        settings.history_ms = std::clamp(string_to_int(value, settings.history_ms), 50, 5000);
    }
    if (auto const value = get("manage_domain_history"); !value.empty())
    {
        settings.manage_domain_history = string_to_bool(value, false);
    }

    settings.program = read_endpoint(map, "program", defaults.program);
    settings.preview = read_endpoint(map, "preview", defaults.preview);

    return settings;
}

Status validate_settings(PluginSettings const& settings, std::string* error,
                         bool require_enabled_roles)
{
    if (settings.domain_path.empty())
    {
        if (error)
        {
            *error = "shared memory domain path must not be empty";
        }
        return Status::InvalidArgument;
    }

    if ((settings.history_ms < 50) || (settings.history_ms > 5000))
    {
        if (error)
        {
            *error = "domain history must be between 50 and 5000 milliseconds";
        }
        return Status::InvalidArgument;
    }

    std::set<std::string> flow_ids;

    auto const validate_endpoint = [&](OutputEndpointSettings const& endpoint,
                                       std::string const& name) -> Status
    {
        if (require_enabled_roles && !endpoint.enabled)
        {
            return Status::Ok;
        }

        if (endpoint.flow_name.empty())
        {
            if (error)
            {
                *error = name + ": flow name must not be empty";
            }
            return Status::InvalidArgument;
        }

        if (!is_valid_uuid(endpoint.video_flow_uuid))
        {
            if (error)
            {
                *error = name + ": video flow UUID is not valid";
            }
            return Status::InvalidArgument;
        }

        if (!flow_ids.insert(endpoint.video_flow_uuid).second)
        {
            if (error)
            {
                *error = name + ": video flow UUID is already used by another output";
            }
            return Status::InvalidArgument;
        }

        if (endpoint.include_audio)
        {
            if (!is_valid_uuid(endpoint.audio_flow_uuid))
            {
                if (error)
                {
                    *error = name + ": audio flow UUID is not valid";
                }
                return Status::InvalidArgument;
            }
            if (!flow_ids.insert(endpoint.audio_flow_uuid).second)
            {
                if (error)
                {
                    *error = name + ": audio flow UUID is already used by another flow";
                }
                return Status::InvalidArgument;
            }
        }

        return Status::Ok;
    };

    if (auto const status = validate_endpoint(settings.program, "Program output");
        status != Status::Ok)
    {
        return status;
    }

    if (auto const status = validate_endpoint(settings.preview, "Preview output");
        status != Status::Ok)
    {
        return status;
    }

    return Status::Ok;
}

bool same_flow_ids(OutputEndpointSettings const& a, OutputEndpointSettings const& b)
{
    return (a.video_flow_uuid == b.video_flow_uuid) && (a.audio_flow_uuid == b.audio_flow_uuid) &&
           (a.include_audio == b.include_audio);
}
} // namespace obsmxl

// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <map>
#include <string>

#include <obsmxl/status.hpp>

namespace obsmxl
{
struct OutputEndpointSettings
{
    bool enabled{false};
    std::string flow_name;
    std::string video_flow_uuid;
    std::string audio_flow_uuid;
    bool include_audio{true};
};

/**
 * Persistent plugin configuration. Stored in the active OBS profile via
 * obs_frontend_get_profile_config() (program/preview endpoints) and in
 * each source's obs_data_t (domain, flow ids, playout delay).
 */
struct PluginSettings
{
    std::string domain_path{"/dev/shm/mxl"};
    int history_ms{200};
    bool manage_domain_history{false};
    OutputEndpointSettings program;
    OutputEndpointSettings preview;
};

using SettingsMap = std::map<std::string, std::string>;

/** Creates a configuration with fresh RFC 4122 v4 flow UUIDs. */
PluginSettings make_default_settings();

SettingsMap settings_to_map(PluginSettings const& settings);
PluginSettings settings_from_map(SettingsMap const& map);

/**
 * Validates the full configuration. When require_enabled_roles is true the
 * enabled endpoints must have valid, disjoint video/audio flow UUIDs.
 */
Status validate_settings(PluginSettings const& settings, std::string* error = nullptr,
                         bool require_enabled_roles = true);

bool same_flow_ids(OutputEndpointSettings const& a, OutputEndpointSettings const& b);
} // namespace obsmxl

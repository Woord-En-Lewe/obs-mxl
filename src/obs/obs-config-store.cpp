// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include "obs-config-store.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

namespace obsmxl::obs
{
namespace
{
constexpr char const* section = "obs-mxl";
} // namespace

PluginSettings load_settings()
{
    auto const defaults = make_default_settings();
    config_t* config = obs_frontend_get_profile_config();
    if (!config)
    {
        return defaults;
    }

    auto const defaults_map = settings_to_map(defaults);
    SettingsMap map;
    for (auto const& [key, fallback] : defaults_map)
    {
        char const* value = config_get_string(config, section, key.c_str());
        map[key] = (value != nullptr) ? value : fallback;
    }
    return settings_from_map(map);
}

void save_settings(PluginSettings const& settings)
{
    config_t* config = obs_frontend_get_profile_config();
    if (!config)
    {
        return;
    }

    for (auto const& [key, value] : settings_to_map(settings))
    {
        config_set_string(config, section, key.c_str(), value.c_str());
    }
    config_save(config);
}
} // namespace obsmxl::obs

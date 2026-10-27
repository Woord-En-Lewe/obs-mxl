// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <obsmxl/settings.hpp>

namespace obsmxl::obs
{
/**
 * Persists the output endpoints in the active OBS profile
 * (obs_frontend_get_profile_config()) under the "obs-mxl" section.
 */
PluginSettings load_settings();
void save_settings(PluginSettings const& settings);
} // namespace obsmxl::obs

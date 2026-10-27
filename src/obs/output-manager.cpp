// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include "output-manager.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>

#include <obsmxl/domain-options.hpp>
#include <obsmxl/uuid.hpp>
#include <obsmxl/version.hpp>

#include "obs-config-store.hpp"
#include "obs-log.hpp"
#include "preview-renderer.hpp"

namespace obsmxl::obs
{
namespace
{
std::string sanitize_group_hint(std::string const& name, char const* role)
{
    std::string result = name;
    for (auto& c : result)
    {
        if ((c == ':') || (c == '\n') || (c == '\r'))
        {
            c = '-';
        }
    }
    if (result.empty())
    {
        result = "OBS";
    }
    return result + ":" + role;
}
} // namespace

OutputManager& OutputManager::instance()
{
    static OutputManager manager;
    return manager;
}

OutputManager::~OutputManager() = default;

OutputManager::Endpoint& OutputManager::endpoint_for(OutputRole role)
{
    return (role == OutputRole::Program) ? program_ : preview_;
}

char const* OutputManager::role_name(OutputRole role)
{
    return (role == OutputRole::Program) ? "Program" : "Preview";
}

PluginSettings OutputManager::settings() const
{
    std::lock_guard lock{mutex_};
    return settings_;
}

bool OutputManager::is_active(OutputRole role) const
{
    std::lock_guard lock{mutex_};
    auto const& endpoint = (role == OutputRole::Program) ? program_ : preview_;
    return (endpoint.output != nullptr) && obs_output_active(endpoint.output);
}

std::string OutputManager::status(OutputRole role) const
{
    std::lock_guard lock{mutex_};
    auto const& endpoint = (role == OutputRole::Program) ? program_ : preview_;
    if (!endpoint.output)
    {
        return "INACTIVE";
    }
    if (!obs_output_active(endpoint.output))
    {
        return "STARTING";
    }

    std::string result = "ACTIVE";
    auto const width = obs_output_get_width(endpoint.output);
    auto const height = obs_output_get_height(endpoint.output);
    if ((width > 0) && (height > 0))
    {
        result += " (" + std::to_string(width) + "x" + std::to_string(height);
        if (audio_t* audio = obs_get_audio())
        {
            result += ", " + std::to_string(audio_output_get_channels(audio)) + "ch";
        }
        result += ")";
    }
    return result;
}

bool OutputManager::start_endpoint(OutputRole role, OutputEndpointSettings const& endpoint,
                                   PluginSettings const& settings, std::string* error)
{
    auto& target = endpoint_for(role);
    if (target.output)
    {
        return true;
    }

    obs_data_t* data = obs_data_create();
    obs_data_set_string(data, "domain_path", settings.domain_path.c_str());
    obs_data_set_string(data, "flow_name", endpoint.flow_name.c_str());
    obs_data_set_string(data, "video_flow_uuid", endpoint.video_flow_uuid.c_str());
    obs_data_set_string(data, "audio_flow_uuid", endpoint.audio_flow_uuid.c_str());
    obs_data_set_bool(data, "include_audio", endpoint.include_audio);
    obs_data_set_string(data, "group_hint",
                        sanitize_group_hint(endpoint.flow_name,
                                            (role == OutputRole::Program) ? "Program" : "Preview")
                            .c_str());
    obs_data_set_string(data, "role", role_name(role));

    std::string const display_name =
        std::string{"MXL "} + role_name(role) + " Output (" + endpoint.flow_name + ")";
    obs_output_t* output = obs_output_create(OBSMXL_OUTPUT_ID, display_name.c_str(), data, nullptr);
    obs_data_release(data);

    if (!output)
    {
        return false;
    }

    std::unique_ptr<PreviewRenderer> preview;
    if (role == OutputRole::Preview)
    {
        preview = std::make_unique<PreviewRenderer>();
        std::string preview_error;
        if (!preview->start(&preview_error) || !preview->video())
        {
            if (error)
            {
                *error = "Preview output: " + preview_error;
            }
            obs_output_release(output);
            return false;
        }
        obs_output_set_media(output, preview->video(), obs_get_audio());
    }
    else
    {
        obs_output_set_media(output, obs_get_video(), obs_get_audio());
    }

    if (!obs_output_start(output))
    {
        char const* last_error = obs_output_get_last_error(output);
        if (error)
        {
            *error = std::string{role_name(role)} + " output failed to start" +
                     (last_error ? std::string{": "} + last_error : std::string{});
        }
        obs_output_release(output);
        return false;
    }

    target.output = output;
    target.preview = std::move(preview);
    target.domain = settings.domain_path;
    target.video_flow_id = endpoint.video_flow_uuid;
    target.audio_flow_id = endpoint.audio_flow_uuid;
    target.include_audio = endpoint.include_audio;
    target.history_ms = settings.history_ms;
    target.active = true;
    return true;
}

void OutputManager::stop_endpoint(Endpoint& endpoint)
{
    if (endpoint.output)
    {
        if (obs_output_active(endpoint.output))
        {
            obs_output_stop(endpoint.output);
        }
        obs_output_release(endpoint.output);
        endpoint.output = nullptr;
    }
    endpoint.preview.reset();
    endpoint.active = false;
    endpoint.domain.clear();
    endpoint.video_flow_id.clear();
    endpoint.audio_flow_id.clear();
    endpoint.history_ms = 0;
}

bool OutputManager::apply_locked(PluginSettings const& settings, std::string* error)
{
    std::string validation_error;
    if (validate_settings(settings, &validation_error, true) != Status::Ok)
    {
        if (error)
        {
            *error = validation_error;
        }
        return false;
    }

    if (settings.manage_domain_history)
    {
        std::string domain_error;
        if (write_domain_history(settings.domain_path, settings.history_ms, &domain_error) !=
            Status::Ok)
        {
            if (error)
            {
                *error = "cannot write domain history: " + domain_error;
            }
            return false;
        }
    }

    settings_ = settings;
    loaded_ = true;

    std::string failure;
    auto const reconcile = [&](OutputRole role, OutputEndpointSettings const& desired)
    {
        auto& target = endpoint_for(role);
        bool const needs_restart =
            target.output && ((target.domain != settings.domain_path) ||
                              (target.video_flow_id != desired.video_flow_uuid) ||
                              (target.audio_flow_id != desired.audio_flow_uuid) ||
                              (target.include_audio != desired.include_audio) ||
                              (target.history_ms != settings.history_ms));

        if (!desired.enabled || needs_restart)
        {
            stop_endpoint(target);
        }

        if (!desired.enabled)
        {
            return;
        }

        if (target.output)
        {
            return;
        }

        std::string endpoint_error;
        if (!start_endpoint(role, desired, settings, &endpoint_error))
        {
            if (failure.empty())
            {
                failure = endpoint_error;
            }
        }
    };

    reconcile(OutputRole::Program, settings.program);
    reconcile(OutputRole::Preview, settings.preview);

    save_settings(settings);

    if (!failure.empty())
    {
        if (error)
        {
            *error = failure;
        }
        return false;
    }
    return true;
}

bool OutputManager::apply(PluginSettings const& settings, std::string* error)
{
    std::lock_guard lock{mutex_};
    return apply_locked(settings, error);
}

void OutputManager::auto_start()
{
    std::lock_guard lock{mutex_};
    settings_ = load_settings();
    loaded_ = true;

    if (!settings_.program.enabled && !settings_.preview.enabled)
    {
        return;
    }

    std::string error;
    if (!apply_locked(settings_, &error))
    {
        logger().log(LogLevel::Error, "auto-start failed: " + error);
    }
}

void OutputManager::stop_all()
{
    std::lock_guard lock{mutex_};
    stop_endpoint(program_);
    stop_endpoint(preview_);
}
} // namespace obsmxl::obs

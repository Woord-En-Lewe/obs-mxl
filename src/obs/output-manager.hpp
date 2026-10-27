// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <obsmxl/settings.hpp>

struct obs_output;

namespace obsmxl::obs
{
class PreviewRenderer;

enum class OutputRole
{
    Program,
    Preview,
};

/**
 * Owns the two obs_output_t instances (program and preview) independently
 * of the dialog lifetime. The dialog is only a view over this controller.
 */
class OutputManager
{
public:
    static OutputManager& instance();

    OutputManager(OutputManager const&) = delete;
    OutputManager& operator=(OutputManager const&) = delete;

    /** Reconciles the running outputs with the given configuration. */
    bool apply(PluginSettings const& settings, std::string* error = nullptr);

    /** Loads the profile configuration and starts enabled endpoints. */
    void auto_start();

    /** Stops and releases every output (called on OBS exit). */
    void stop_all();

    [[nodiscard]] PluginSettings settings() const;
    [[nodiscard]] bool is_active(OutputRole role) const;
    [[nodiscard]] std::string status(OutputRole role) const;

private:
    struct Endpoint
    {
        obs_output* output{nullptr};
        std::unique_ptr<PreviewRenderer> preview;
        std::string domain;
        std::string video_flow_id;
        std::string audio_flow_id;
        bool include_audio{false};
        int history_ms{0};
        bool active{false};
    };

    OutputManager() = default;
    ~OutputManager();

    bool apply_locked(PluginSettings const& settings, std::string* error);
    bool start_endpoint(OutputRole role, OutputEndpointSettings const& endpoint,
                        PluginSettings const& settings, std::string* error);
    void stop_endpoint(Endpoint& endpoint);
    Endpoint& endpoint_for(OutputRole role);
    static char const* role_name(OutputRole role);

    mutable std::mutex mutex_;
    Endpoint program_;
    Endpoint preview_;
    PluginSettings settings_;
    bool loaded_{false};
};
} // namespace obsmxl::obs

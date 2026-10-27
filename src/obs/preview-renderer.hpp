// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <graphics/graphics.h>
#include <obs-frontend-api.h>

namespace obsmxl::obs
{
/**
 * Renders the active preview scene (studio mode) or the program scene into
 * a private BGRA video_t so that the MXL preview output can consume it.
 *
 * OBS does not expose the frontend preview view as a video_t, so the scene
 * is rendered off-screen from obs_add_main_render_callback and downloaded
 * through a staging surface. This costs one GPU readback per frame at
 * canvas resolution; it is only active while the preview output is on.
 */
class PreviewRenderer
{
public:
    PreviewRenderer();
    ~PreviewRenderer();

    PreviewRenderer(PreviewRenderer const&) = delete;
    PreviewRenderer& operator=(PreviewRenderer const&) = delete;

    bool start(std::string* error = nullptr);
    void stop();

    [[nodiscard]] video_t* video() const noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    static void render_callback(void* param, std::uint32_t cx, std::uint32_t cy);
    static void frontend_event(enum obs_frontend_event event, void* param);

    void render();
    void set_scene(obs_source* source);

    obs_source* scene_{nullptr};
    mutable std::mutex scene_mutex_;
    gs_texrender_t* texrender_{nullptr};
    gs_stagesurf_t* stagesurf_{nullptr};
    video_t* video_{nullptr};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    bool running_{false};
};
} // namespace obsmxl::obs

// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include "preview-renderer.hpp"

#include <cstring>

#include <graphics/graphics.h>
#include <media-io/video-frame.h>
#include <media-io/video-io.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include "obs-log.hpp"

namespace obsmxl::obs
{
PreviewRenderer::PreviewRenderer() = default;

PreviewRenderer::~PreviewRenderer()
{
    stop();
}

bool PreviewRenderer::start(std::string* error)
{
    if (running_)
    {
        return true;
    }

    obs_video_info ovi{};
    if (!obs_get_video_info(&ovi) || (ovi.base_width == 0) || (ovi.base_height == 0))
    {
        if (error)
        {
            *error = "OBS video output is not initialised";
        }
        return false;
    }

    width_ = ovi.base_width;
    height_ = ovi.base_height;

    video_output_info const* main_info = video_output_get_info(obs_get_video());

    video_output_info video_info{};
    video_info.name = "obs-mxl-preview";
    video_info.format = VIDEO_FORMAT_BGRA;
    video_info.width = width_;
    video_info.height = height_;
    video_info.fps_num = ovi.fps_num;
    video_info.fps_den = ovi.fps_den;
    if ((video_info.fps_num == 0) || (video_info.fps_den == 0))
    {
        video_info.fps_num = 30;
        video_info.fps_den = 1;
    }
    video_info.cache_size = 16;
    video_info.colorspace = main_info ? main_info->colorspace : VIDEO_CS_709;
    video_info.range = main_info ? main_info->range : VIDEO_RANGE_PARTIAL;

    video_t* video = nullptr;
    if ((video_output_open(&video, &video_info) != VIDEO_OUTPUT_SUCCESS) || !video)
    {
        if (error)
        {
            *error = "could not create the preview video queue";
        }
        return false;
    }
    video_ = video;

    obs_enter_graphics();
    texrender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    stagesurf_ = gs_stagesurface_create(width_, height_, GS_BGRA);
    obs_leave_graphics();

    if (!texrender_ || !stagesurf_)
    {
        if (error)
        {
            *error = "could not create the preview render targets";
        }

        obs_enter_graphics();
        if (stagesurf_)
        {
            gs_stagesurface_destroy(stagesurf_);
            stagesurf_ = nullptr;
        }
        if (texrender_)
        {
            gs_texrender_destroy(texrender_);
            texrender_ = nullptr;
        }
        obs_leave_graphics();

        video_output_close(video_);
        video_ = nullptr;
        return false;
    }

    running_ = true;
    obs_add_main_render_callback(&PreviewRenderer::render_callback, this);
    obs_frontend_add_event_callback(&PreviewRenderer::frontend_event, this);

    if (obs_frontend_preview_program_mode_active())
    {
        set_scene(obs_frontend_get_current_preview_scene());
    }
    else
    {
        set_scene(obs_frontend_get_current_scene());
    }

    logger().log(LogLevel::Info, "preview renderer started at " + std::to_string(width_) + "x" +
                                     std::to_string(height_));
    return true;
}

void PreviewRenderer::stop()
{
    if (!running_)
    {
        return;
    }
    running_ = false;

    obs_remove_main_render_callback(&PreviewRenderer::render_callback, this);
    obs_frontend_remove_event_callback(&PreviewRenderer::frontend_event, this);

    set_scene(nullptr);

    obs_enter_graphics();
    if (stagesurf_)
    {
        gs_stagesurface_destroy(stagesurf_);
        stagesurf_ = nullptr;
    }
    if (texrender_)
    {
        gs_texrender_destroy(texrender_);
        texrender_ = nullptr;
    }
    obs_leave_graphics();

    if (video_)
    {
        video_output_close(video_);
        video_ = nullptr;
    }
}

video_t* PreviewRenderer::video() const noexcept
{
    return video_;
}

bool PreviewRenderer::running() const noexcept
{
    return running_;
}

void PreviewRenderer::set_scene(obs_source* source)
{
    obs_source_t* previous = nullptr;
    {
        std::lock_guard lock{scene_mutex_};
        previous = scene_;
        scene_ = source;
    }
    if (previous)
    {
        obs_source_release(previous);
    }
}

void PreviewRenderer::frontend_event(enum obs_frontend_event event, void* param)
{
    auto* self = static_cast<PreviewRenderer*>(param);
    if (!self || !self->running_)
    {
        return;
    }

    switch (event)
    {
    case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
    case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
        self->set_scene(obs_frontend_get_current_preview_scene());
        break;

    case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
        if (!obs_frontend_preview_program_mode_active())
        {
            self->set_scene(obs_frontend_get_current_scene());
        }
        break;

    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
        self->set_scene(nullptr);
        break;

    default:
        break;
    }
}

void PreviewRenderer::render_callback(void* param, std::uint32_t, std::uint32_t)
{
    static_cast<PreviewRenderer*>(param)->render();
}

void PreviewRenderer::render()
{
    obs_source* scene = nullptr;
    {
        std::lock_guard lock{scene_mutex_};
        if (scene_)
        {
            scene = obs_source_get_ref(scene_);
        }
    }

    if (!scene)
    {
        return;
    }

    if (!video_ || !texrender_ || !stagesurf_)
    {
        obs_source_release(scene);
        return;
    }

    gs_texrender_reset(texrender_);
    if (!gs_texrender_begin(texrender_, width_, height_))
    {
        obs_source_release(scene);
        return;
    }

    vec4 background;
    vec4_zero(&background);
    gs_clear(GS_CLEAR_COLOR, &background, 0.0F, 0);
    gs_ortho(0.0F, static_cast<float>(width_), 0.0F, static_cast<float>(height_), -100.0F, 100.0F);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    obs_source_video_render(scene);
    gs_blend_state_pop();
    gs_texrender_end(texrender_);

    video_frame frame{};
    if (!video_output_lock_frame(video_, &frame, 1, os_gettime_ns()))
    {
        obs_source_release(scene);
        return;
    }

    gs_stage_texture(stagesurf_, gs_texrender_get_texture(texrender_));

    std::uint8_t* data = nullptr;
    std::uint32_t linesize = 0;
    if (gs_stagesurface_map(stagesurf_, &data, &linesize) && data)
    {
        for (std::uint32_t row = 0; row < height_; ++row)
        {
            std::memcpy(frame.data[0] + static_cast<std::size_t>(row) * frame.linesize[0],
                        data + static_cast<std::size_t>(row) * linesize, frame.linesize[0]);
        }
        gs_stagesurface_unmap(stagesurf_);
    }

    video_output_unlock_frame(video_);
    obs_source_release(scene);
}
} // namespace obsmxl::obs

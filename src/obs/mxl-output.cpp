// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include "mxl-output.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <media-io/audio-io.h>
#include <media-io/video-io.h>
#include <obs-module.h>
#include <obs.h>
#include <util/platform.h>

#include <obsmxl/backend.hpp>
#include <obsmxl/egress.hpp>
#include <obsmxl/uuid.hpp>
#include <obsmxl/version.hpp>

#include "obs-log.hpp"

namespace obsmxl::obs
{
namespace
{
struct MxlOutputContext
{
    obs_output_t* output{nullptr};
    std::unique_ptr<Backend> backend;
    std::unique_ptr<EgressPipeline> pipeline;
    std::string domain;
    std::string flow_name;
    std::string group_hint;
    std::string video_flow_id;
    std::string audio_flow_id;
    bool include_audio{false};
    PixelFormat input_format{PixelFormat::NV12};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{2};
    std::mutex mutex;
    bool active{false};
};

std::optional<PixelFormat> map_video_format(enum video_format format)
{
    switch (format)
    {
    case VIDEO_FORMAT_NV12:
        return PixelFormat::NV12;
    case VIDEO_FORMAT_P010:
        return PixelFormat::P010;
    case VIDEO_FORMAT_I420:
        return PixelFormat::I420;
    case VIDEO_FORMAT_I010:
        return PixelFormat::I010;
    case VIDEO_FORMAT_BGRA:
        return PixelFormat::BGRA;
    case VIDEO_FORMAT_BGRX:
        return PixelFormat::BGRA;
    case VIDEO_FORMAT_V210:
        return PixelFormat::V210;
    default:
        return std::nullopt;
    }
}

void apply_settings(MxlOutputContext& context, obs_data_t* settings)
{
    context.domain = obs_data_get_string(settings, "domain_path");
    context.flow_name = obs_data_get_string(settings, "flow_name");
    context.group_hint = obs_data_get_string(settings, "group_hint");
    context.video_flow_id = obs_data_get_string(settings, "video_flow_uuid");
    context.audio_flow_id = obs_data_get_string(settings, "audio_flow_uuid");
    context.include_audio = obs_data_get_bool(settings, "include_audio");
}

bool start_output(MxlOutputContext& context)
{
    std::string const role = context.group_hint;
    if (context.domain.empty() || !is_valid_uuid(context.video_flow_id))
    {
        obs_output_set_last_error(context.output,
                                  "MXL output needs a domain and a valid video flow UUID");
        return false;
    }

    if (context.include_audio && !is_valid_uuid(context.audio_flow_id))
    {
        obs_output_set_last_error(context.output,
                                  "MXL output needs a valid audio flow UUID when audio is enabled");
        return false;
    }

    video_scale_info const* conversion = obs_output_get_video_conversion(context.output);
    if (!conversion || (conversion->width == 0) || (conversion->height == 0))
    {
        obs_output_set_last_error(context.output, "OBS did not provide a video format");
        return false;
    }

    context.width = conversion->width;
    context.height = conversion->height;
    auto const mapped_format = map_video_format(conversion->format);
    if (!mapped_format)
    {
        obs_output_set_last_error(context.output,
                                  "OBS delivered an unsupported video format for MXL egress");
        return false;
    }
    context.input_format = *mapped_format;

    obs_video_info ovi{};
    if (!obs_get_video_info(&ovi))
    {
        obs_output_set_last_error(context.output, "OBS video output is not initialised");
        return false;
    }

    audio_t* audio = obs_get_audio();
    context.sample_rate = audio ? audio_output_get_sample_rate(audio) : 48000;
    context.channels = static_cast<std::uint32_t>(std::clamp<std::size_t>(
        audio ? audio_output_get_channels(audio) : 2, 1, max_audio_channels));

    std::string error;
    context.backend = create_backend(BackendKind::Mxl, context.domain, &error);
    if (!context.backend)
    {
        obs_output_set_last_error(context.output,
                                  error.empty() ? "cannot open the MXL domain" : error.c_str());
        return false;
    }

    EgressPipeline::Config config;
    config.domain = context.domain;
    config.label = context.flow_name;
    config.group_hint = context.group_hint.empty() ? "OBS:Program" : context.group_hint;
    config.video_flow_id = context.video_flow_id;
    config.audio_flow_id = context.audio_flow_id;
    config.width = context.width;
    config.height = context.height;
    config.frame_rate = Rational{ovi.fps_num, ovi.fps_den};
    config.sample_rate = context.sample_rate;
    config.channels = context.channels;
    config.include_audio = context.include_audio;
    config.audio_max_batch = 480;

    context.pipeline =
        std::make_unique<EgressPipeline>(*context.backend, config, clock(), &logger());
    if (!context.pipeline->start(&error))
    {
        obs_output_set_last_error(context.output,
                                  error.empty() ? "cannot create MXL flows" : error.c_str());
        context.pipeline.reset();
        context.backend.reset();
        return false;
    }

    if (!obs_output_begin_data_capture(
            context.output, OBS_OUTPUT_VIDEO | (context.include_audio ? OBS_OUTPUT_AUDIO : 0U)))
    {
        context.pipeline->stop();
        context.pipeline.reset();
        context.backend.reset();
        obs_output_set_last_error(context.output, "cannot begin MXL data capture");
        return false;
    }

    context.active = true;
    logger().log(LogLevel::Info,
                 "MXL " + role + " output active (" + std::to_string(context.width) + "x" +
                     std::to_string(context.height) + ", " + std::to_string(context.channels) +
                     "ch @" + std::to_string(context.sample_rate) + "Hz)");
    return true;
}

void stop_output(MxlOutputContext& context)
{
    if (!context.active)
    {
        return;
    }
    context.active = false;

    obs_output_end_data_capture(context.output);
    if (context.pipeline)
    {
        context.pipeline->stop();
        context.pipeline.reset();
    }
    context.backend.reset();
}

void* output_create(obs_data_t* settings, obs_output_t* output)
{
    auto* context = new MxlOutputContext();
    context->output = output;
    apply_settings(*context, settings);
    return context;
}

void output_destroy(void* data)
{
    auto* context = static_cast<MxlOutputContext*>(data);
    if (!context)
    {
        return;
    }
    {
        std::lock_guard lock{context->mutex};
        stop_output(*context);
    }
    delete context;
}

bool output_start(void* data)
{
    auto* context = static_cast<MxlOutputContext*>(data);
    std::lock_guard lock{context->mutex};
    return start_output(*context);
}

void output_stop(void* data, std::uint64_t)
{
    auto* context = static_cast<MxlOutputContext*>(data);
    std::lock_guard lock{context->mutex};
    stop_output(*context);
}

void output_raw_video(void* data, video_data* frame)
{
    auto* context = static_cast<MxlOutputContext*>(data);
    if (!context || !frame)
    {
        return;
    }

    FrameView view;
    view.format = context->input_format;
    view.width = context->width;
    view.height = context->height;
    view.host_timestamp_ns = frame->timestamp;

    switch (view.format)
    {
    case PixelFormat::NV12:
    case PixelFormat::P010:
        view.planes[0] = {frame->data[0], frame->linesize[0]};
        view.planes[1] = {frame->data[1], frame->linesize[1]};
        break;
    case PixelFormat::I420:
    case PixelFormat::YV12:
    case PixelFormat::I010:
        view.planes[0] = {frame->data[0], frame->linesize[0]};
        view.planes[1] = {frame->data[1], frame->linesize[1]};
        view.planes[2] = {frame->data[2], frame->linesize[2]};
        break;
    case PixelFormat::BGRA:
    case PixelFormat::V210:
        view.planes[0] = {frame->data[0], frame->linesize[0]};
        break;
    }

    if (context->pipeline)
    {
        context->pipeline->push_video(view);
    }
}

void output_raw_audio(void* data, audio_data* frame)
{
    auto* context = static_cast<MxlOutputContext*>(data);
    if (!context || !frame || !context->pipeline)
    {
        return;
    }

    PlanarAudioView audio;
    audio.frames = frame->frames;
    audio.sample_rate = context->sample_rate;
    audio.host_timestamp_ns = frame->timestamp;
    audio.channel_count =
        std::min(context->channels, static_cast<std::uint32_t>(max_audio_channels));
    for (std::uint32_t channel = 0; channel < audio.channel_count; ++channel)
    {
        audio.channels[channel] = reinterpret_cast<float const*>(frame->data[channel]);
    }

    context->pipeline->push_audio(audio);
}

void output_update(void* data, obs_data_t* settings)
{
    auto* context = static_cast<MxlOutputContext*>(data);
    std::lock_guard lock{context->mutex};
    apply_settings(*context, settings);
}

void output_get_defaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "domain_path", "/dev/shm/mxl");
    obs_data_set_default_string(settings, "flow_name", "OBS Program");
    obs_data_set_default_string(settings, "group_hint", "OBS:Program");
    obs_data_set_default_bool(settings, "include_audio", true);
}

obs_properties_t* output_get_properties(void* data)
{
    auto* context = static_cast<MxlOutputContext*>(data);
    obs_properties_t* props = obs_properties_create();

    obs_data_t* settings =
        context && context->output ? obs_output_get_settings(context->output) : nullptr;
    if (settings)
    {
        obs_properties_add_text(props, "domain_path", obs_data_get_string(settings, "domain_path"),
                                OBS_TEXT_INFO);
        obs_properties_add_text(props, "video_flow_uuid",
                                obs_data_get_string(settings, "video_flow_uuid"), OBS_TEXT_INFO);
        obs_properties_add_text(props, "audio_flow_uuid",
                                obs_data_get_string(settings, "audio_flow_uuid"), OBS_TEXT_INFO);
        obs_data_release(settings);
    }
    return props;
}

const char* output_get_name(void*)
{
    return obs_module_text("Output.Name");
}

obs_output_info const make_output_info()
{
    obs_output_info info{};
    info.id = OBSMXL_OUTPUT_ID;
    info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO;
    info.get_name = output_get_name;
    info.create = output_create;
    info.destroy = output_destroy;
    info.start = output_start;
    info.stop = output_stop;
    info.raw_video = output_raw_video;
    info.raw_audio = output_raw_audio;
    info.update = output_update;
    info.get_defaults = output_get_defaults;
    info.get_properties = output_get_properties;
    return info;
}
} // namespace

void register_mxl_output()
{
    static obs_output_info const info = make_output_info();
    obs_register_output(&info);
}
} // namespace obsmxl::obs

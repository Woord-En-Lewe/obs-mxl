// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include "mxl-source.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <media-io/audio-io.h>
#include <media-io/video-io.h>
#include <obs-module.h>
#include <obs.h>

#include <obsmxl/backend.hpp>
#include <obsmxl/discovery.hpp>
#include <obsmxl/ingress.hpp>
#include <obsmxl/version.hpp>

#include "obs-log.hpp"

namespace obsmxl::obs
{
namespace
{
constexpr Ns worker_retry_ns = 500'000'000LL;
constexpr Ns worker_max_sleep_ns = 20'000'000LL;
constexpr Ns worker_min_sleep_ns = 1'000'000LL;

struct MxlSourceContext
{
    obs_source_t* source{nullptr};

    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> running{false};

    std::string domain{"/dev/shm/mxl"};
    std::string video_flow_id;
    std::string audio_flow_id;
    int playout_delay_ms{50};
    bool enable_audio{true};
    bool shown{false};
};

enum speaker_layout speakers_for(std::uint32_t channels)
{
    switch (channels)
    {
    case 1:
        return SPEAKERS_MONO;
    case 2:
        return SPEAKERS_STEREO;
    case 3:
        return SPEAKERS_2POINT1;
    case 4:
        return SPEAKERS_4POINT0;
    case 5:
        return SPEAKERS_4POINT1;
    case 6:
        return SPEAKERS_5POINT1;
    case 8:
        return SPEAKERS_7POINT1;
    default:
        return SPEAKERS_STEREO;
    }
}

void publish_video(MxlSourceContext& context, IngressPipeline::VideoFrame const& frame)
{
    if (!context.source)
    {
        return;
    }

    obs_source_frame2 obs_frame{};
    obs_frame.format = VIDEO_FORMAT_V210;
    obs_frame.width = frame.width;
    obs_frame.height = frame.height;
    obs_frame.data[0] = const_cast<std::uint8_t*>(frame.data);
    obs_frame.linesize[0] = frame.stride;
    obs_frame.timestamp = frame.host_timestamp_ns;
    obs_frame.range = VIDEO_RANGE_PARTIAL;
    obs_frame.trc = VIDEO_TRC_DEFAULT;

    obs_source_output_video2(context.source, &obs_frame);
}

void publish_audio(MxlSourceContext& context, IngressPipeline::AudioChunk const& chunk)
{
    if (!context.source)
    {
        return;
    }

    obs_source_audio audio{};
    std::uint32_t const channels = std::min<std::uint32_t>(chunk.channel_count, 8);
    for (std::uint32_t channel = 0; channel < channels; ++channel)
    {
        audio.data[channel] = reinterpret_cast<std::uint8_t const*>(chunk.channels[channel]);
    }
    audio.frames = chunk.frames;
    audio.speakers = speakers_for(channels);
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.samples_per_sec = chunk.sample_rate;
    audio.timestamp = chunk.host_timestamp_ns;

    obs_source_output_audio(context.source, &audio);
}

void worker_main(MxlSourceContext* context)
{
    std::string const domain = context->domain;
    std::string const video_flow_id = context->video_flow_id;
    std::string const audio_flow_id = context->audio_flow_id;
    int const playout_delay_ms = context->playout_delay_ms;
    bool const enable_audio = context->enable_audio;

    std::unique_ptr<Backend> backend;
    std::unique_ptr<IngressPipeline> pipeline;
    Ns last_backend_attempt = 0;
    bool reported_backend_error = false;

    while (context->running.load(std::memory_order_acquire))
    {
        if (!pipeline)
        {
            Ns const now = clock().host_now_ns();
            if ((last_backend_attempt != 0) && ((now - last_backend_attempt) < worker_retry_ns))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                continue;
            }
            last_backend_attempt = now;

            if (video_flow_id.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                continue;
            }

            std::string error;
            backend = create_backend(BackendKind::Mxl, domain, &error);
            if (!backend)
            {
                if (!reported_backend_error)
                {
                    logger().log(LogLevel::Error,
                                 "cannot open MXL domain '" + domain + "': " + error);
                    reported_backend_error = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                continue;
            }
            reported_backend_error = false;

            IngressPipeline::Config config;
            config.domain = domain;
            config.video_flow_id = video_flow_id;
            config.audio_flow_id = audio_flow_id;
            config.playout_delay_ms = playout_delay_ms;
            config.enable_audio = enable_audio && !audio_flow_id.empty();

            pipeline =
                std::make_unique<IngressPipeline>(*backend, std::move(config), clock(), &logger());
        }

        auto result = pipeline->poll();

        if (result.video)
        {
            publish_video(*context, *result.video);
        }
        if (result.audio)
        {
            publish_audio(*context, *result.audio);
        }

        if (!context->running.load(std::memory_order_acquire))
        {
            break;
        }

        Ns const now = clock().host_now_ns();
        Ns sleep_ns = worker_max_sleep_ns;
        if (result.next_poll_host_ns != 0)
        {
            Ns const target = static_cast<Ns>(result.next_poll_host_ns);
            sleep_ns = (target > now) ? (target - now) : worker_min_sleep_ns;
        }
        sleep_ns = std::clamp<Ns>(sleep_ns, worker_min_sleep_ns, worker_max_sleep_ns);

        std::this_thread::sleep_for(std::chrono::nanoseconds{sleep_ns});
    }

    pipeline.reset();
    backend.reset();
}

void start_worker(MxlSourceContext& context)
{
    if (context.worker.joinable())
    {
        return;
    }
    if (context.video_flow_id.empty())
    {
        return;
    }
    context.running.store(true, std::memory_order_release);
    context.worker = std::thread{worker_main, &context};
}

void stop_worker(MxlSourceContext& context)
{
    context.running.store(false, std::memory_order_release);
    if (context.worker.joinable())
    {
        context.worker.join();
    }
}

void apply_source_settings(MxlSourceContext& context, obs_data_t* settings)
{
    context.domain = obs_data_get_string(settings, "domain_path");
    if (context.domain.empty())
    {
        context.domain = "/dev/shm/mxl";
    }
    context.video_flow_id = obs_data_get_string(settings, "video_flow_id");
    context.audio_flow_id = obs_data_get_string(settings, "audio_flow_id");
    context.playout_delay_ms = static_cast<int>(obs_data_get_int(settings, "playout_delay_ms"));
    context.playout_delay_ms = std::clamp(context.playout_delay_ms, 0, 1000);
    context.enable_audio = obs_data_get_bool(settings, "enable_audio");
}

void* source_create(obs_data_t* settings, obs_source_t* source)
{
    auto* context = new MxlSourceContext();
    context->source = source;
    apply_source_settings(*context, settings);
    return context;
}

void source_destroy(void* data)
{
    auto* context = static_cast<MxlSourceContext*>(data);
    if (!context)
    {
        return;
    }
    {
        std::lock_guard lock{context->mutex};
        stop_worker(*context);
    }
    delete context;
}

void source_update(void* data, obs_data_t* settings)
{
    auto* context = static_cast<MxlSourceContext*>(data);
    std::lock_guard lock{context->mutex};

    std::string const old_domain = context->domain;
    std::string const old_video = context->video_flow_id;
    std::string const old_audio = context->audio_flow_id;
    int const old_delay = context->playout_delay_ms;
    bool const old_audio_enabled = context->enable_audio;

    apply_source_settings(*context, settings);

    bool const changed = (old_domain != context->domain) || (old_video != context->video_flow_id) ||
                         (old_audio != context->audio_flow_id) ||
                         (old_delay != context->playout_delay_ms) ||
                         (old_audio_enabled != context->enable_audio);

    if (changed)
    {
        stop_worker(*context);
        if (context->shown)
        {
            start_worker(*context);
        }
    }
}

void source_show(void* data)
{
    auto* context = static_cast<MxlSourceContext*>(data);
    std::lock_guard lock{context->mutex};
    context->shown = true;
    start_worker(*context);
}

void source_hide(void* data)
{
    auto* context = static_cast<MxlSourceContext*>(data);
    std::lock_guard lock{context->mutex};
    context->shown = false;
    stop_worker(*context);
}

void source_get_defaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "domain_path", "/dev/shm/mxl");
    obs_data_set_default_string(settings, "video_flow_id", "");
    obs_data_set_default_string(settings, "audio_flow_id", "");
    obs_data_set_default_int(settings, "playout_delay_ms", 50);
    obs_data_set_default_bool(settings, "enable_audio", true);
}

std::string flow_description(FlowSummary const& flow)
{
    std::string text = flow.label.empty() ? flow.id : flow.label;
    text += "  ";
    text += flow.id;
    if (flow.audio)
    {
        text += "  (";
        text += std::to_string(flow.channel_count);
        text += "ch)";
    }
    else if ((flow.width > 0) && (flow.height > 0))
    {
        text += "  (" + std::to_string(flow.width) + "x" + std::to_string(flow.height);
        if (flow.rate.valid())
        {
            text += " @" + to_string(flow.rate);
        }
        text += ")";
    }
    return text;
}

obs_properties_t* source_get_properties(void* data)
{
    auto* context = static_cast<MxlSourceContext*>(data);
    obs_properties_t* props = obs_properties_create();

    obs_source_t* source = context ? context->source : nullptr;
    obs_data_t* settings = source ? obs_source_get_settings(source) : nullptr;

    obs_properties_add_text(props, "domain_path", obs_module_text("Source.Domain"),
                            OBS_TEXT_DEFAULT);

    auto const domain = context ? context->domain : std::string{"/dev/shm/mxl"};
    auto const videos = discover_video_flows(domain);
    auto const audios = discover_audio_flows(domain);

    obs_property_t* video =
        obs_properties_add_list(props, "video_flow_id", obs_module_text("Source.VideoFlow"),
                                OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    for (auto const& flow : videos)
    {
        obs_property_list_add_string(video, flow_description(flow).c_str(), flow.id.c_str());
    }

    obs_property_t* audio =
        obs_properties_add_list(props, "audio_flow_id", obs_module_text("Source.AudioFlow"),
                                OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(audio, obs_module_text("Source.NoAudio"), "");
    for (auto const& flow : audios)
    {
        obs_property_list_add_string(audio, flow_description(flow).c_str(), flow.id.c_str());
    }

    obs_properties_add_int_slider(props, "playout_delay_ms", obs_module_text("Source.PlayoutDelay"),
                                  0, 1000, 5);
    obs_properties_add_bool(props, "enable_audio", obs_module_text("Source.EnableAudio"));
    obs_properties_add_text(props, "hint", obs_module_text("Source.Hint"), OBS_TEXT_INFO);

    if (settings)
    {
        obs_data_release(settings);
    }
    return props;
}

const char* source_get_name(void*)
{
    return obs_module_text("Source.Name");
}

obs_source_info const make_source_info()
{
    obs_source_info info{};
    info.id = OBSMXL_SOURCE_ID;
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = source_get_name;
    info.create = source_create;
    info.destroy = source_destroy;
    info.get_defaults = source_get_defaults;
    info.get_properties = source_get_properties;
    info.update = source_update;
    info.show = source_show;
    info.hide = source_hide;
    return info;
}
} // namespace

void register_mxl_source()
{
    static obs_source_info const info = make_source_info();
    obs_register_source(&info);
}
} // namespace obsmxl::obs

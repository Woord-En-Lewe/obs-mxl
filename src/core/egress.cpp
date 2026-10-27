// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/egress.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <obsmxl/flow-definition.hpp>
#include <obsmxl/uuid.hpp>

namespace obsmxl
{
namespace
{
constexpr Ns log_interval_ns = 2'000'000'000LL;
constexpr Ns offset_recalibration_ns = 5'000'000'000LL;

std::string join_error(std::string const& prefix, std::string const& detail)
{
    return detail.empty() ? prefix : prefix + ": " + detail;
}
} // namespace

EgressPipeline::EgressPipeline(Backend& backend, Config config, Clock const& clock, Logger* logger)
    : backend_(backend), config_(std::move(config)), clock_(clock), logger_(logger)
{
    config_.audio_max_batch = std::max<std::uint32_t>(1, config_.audio_max_batch);
    offset_.calibrate(clock_, 0);
}

EgressPipeline::~EgressPipeline()
{
    stop();
}

EgressPipeline::Config const& EgressPipeline::config() const noexcept
{
    return config_;
}

VideoStreamInfo const& EgressPipeline::video_info() const noexcept
{
    return video_info_;
}

AudioStreamInfo const& EgressPipeline::audio_info() const noexcept
{
    return audio_info_;
}

bool EgressPipeline::started() const noexcept
{
    return started_.load(std::memory_order_acquire);
}

void EgressPipeline::log_throttled(LogLevel level, std::string const& message)
{
    if (!logger_)
    {
        return;
    }

    Ns const now = clock_.host_now_ns();
    if ((now - last_log_host_) >= log_interval_ns)
    {
        last_log_host_ = now;
        logger_->log(level, message);
    }
}

bool EgressPipeline::create_video_writer(std::string* error)
{
    VideoFlowSpec spec;
    spec.id = config_.video_flow_id;
    spec.label = config_.label;
    spec.group_hint = config_.group_hint;
    spec.grain_rate = config_.frame_rate;
    spec.width = config_.width;
    spec.height = config_.height;
    spec.colorspace = config_.colorspace;

    std::string validation_error;
    if (!validate_video_flow_spec(spec, &validation_error))
    {
        if (error)
        {
            *error = validation_error;
        }
        return false;
    }

    auto const flow_def = make_video_flow_json(spec);
    auto const options = make_writer_options_json(v210_total_slices(config_.height));

    bool created = false;
    if (auto const status =
            backend_.create_video_writer(flow_def, options, video_writer_, video_info_, &created);
        status != Status::Ok)
    {
        if (error)
        {
            *error = join_error("cannot create video flow", backend_.last_error());
        }
        return false;
    }

    if (!created)
    {
        // MXL reuses an existing flow with the same UUID. Refuse to write
        // into a flow whose geometry does not match the OBS canvas.
        if ((video_info_.width != config_.width) || (video_info_.height != config_.height) ||
            (video_info_.grain_rate != config_.frame_rate))
        {
            if (error)
            {
                *error = "a flow with that UUID already exists with a different geometry "
                         "(" +
                         std::to_string(video_info_.width) + "x" +
                         std::to_string(video_info_.height) + " @" +
                         to_string(video_info_.grain_rate) + "); choose another UUID";
            }
            video_writer_.reset();
            return false;
        }
    }

    if (video_info_.stride < v210_line_stride(config_.width))
    {
        if (error)
        {
            *error = "MXL flow reported an invalid V210 stride";
        }
        video_writer_.reset();
        return false;
    }

    if (video_info_.grain_count == 0)
    {
        video_info_.grain_count = 8;
    }

    video_buffer_.assign(v210_buffer_size(video_info_.stride, config_.height), 0);

    if (logger_)
    {
        logger_->log(LogLevel::Info,
                     std::string{created ? "created" : "opened"} + " MXL video flow " +
                         config_.video_flow_id + " (" + std::to_string(config_.width) + "x" +
                         std::to_string(config_.height) + " @" + to_string(config_.frame_rate) +
                         ", " + std::to_string(video_info_.grain_count) + " grains)");
    }
    return true;
}

bool EgressPipeline::create_audio_writer(std::string* error)
{
    AudioFlowSpec spec;
    spec.id = config_.audio_flow_id;
    spec.label = config_.label + " Audio";
    spec.group_hint = config_.group_hint + "/Audio";
    spec.sample_rate = Rational{config_.sample_rate, 1};
    spec.channel_count = config_.channels;

    std::string validation_error;
    if (!validate_audio_flow_spec(spec, &validation_error))
    {
        if (error)
        {
            *error = validation_error;
        }
        return false;
    }

    auto const flow_def = make_audio_flow_json(spec);
    auto const options = make_writer_options_json(config_.audio_max_batch);

    bool created = false;
    if (auto const status =
            backend_.create_audio_writer(flow_def, options, audio_writer_, audio_info_, &created);
        status != Status::Ok)
    {
        if (error)
        {
            *error = join_error("cannot create audio flow", backend_.last_error());
        }
        return false;
    }

    if ((audio_info_.sample_rate != config_.sample_rate) ||
        (audio_info_.channels != config_.channels))
    {
        if (error)
        {
            *error = "an audio flow with that UUID already exists with " +
                     std::to_string(audio_info_.channels) + "ch @" +
                     std::to_string(audio_info_.sample_rate) + "Hz; choose another UUID";
        }
        audio_writer_.reset();
        return false;
    }

    if (audio_info_.max_batch == 0)
    {
        audio_info_.max_batch = config_.audio_max_batch;
    }

    if (logger_)
    {
        logger_->log(LogLevel::Info, std::string{created ? "created" : "opened"} +
                                         " MXL audio flow " + config_.audio_flow_id + " (" +
                                         std::to_string(config_.channels) + "ch @" +
                                         std::to_string(config_.sample_rate) + "Hz)");
    }
    return true;
}

bool EgressPipeline::start(std::string* error)
{
    std::lock_guard lifecycle{lifecycle_mutex_};
    if (started())
    {
        return true;
    }

    if (config_.domain.empty())
    {
        if (error)
        {
            *error = "shared memory domain path must not be empty";
        }
        return false;
    }

    if (!is_valid_uuid(config_.video_flow_id))
    {
        if (error)
        {
            *error = "video flow UUID is not valid";
        }
        return false;
    }

    if (config_.include_audio)
    {
        if (!is_valid_uuid(config_.audio_flow_id))
        {
            if (error)
            {
                *error = "audio flow UUID is not valid";
            }
            return false;
        }
        if (config_.channels > max_audio_channels)
        {
            if (error)
            {
                *error = "audio channel count is not supported";
            }
            return false;
        }
    }

    offset_.calibrate(clock_, 0);

    if (!create_video_writer(error))
    {
        return false;
    }

    if (config_.include_audio && !create_audio_writer(error))
    {
        video_writer_.reset();
        return false;
    }

    reset_video_index();
    reset_audio_index();
    started_.store(true, std::memory_order_release);
    return true;
}

void EgressPipeline::stop() noexcept
{
    std::lock_guard lifecycle{lifecycle_mutex_};
    if (!started_.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    {
        std::lock_guard lock{audio_mutex_};
        audio_writer_.reset();
    }
    {
        std::lock_guard lock{video_mutex_};
        video_writer_.reset();
    }

    video_buffer_.clear();
    reset_video_index();
    reset_audio_index();
    video_info_ = {};
    audio_info_ = {};
}

void EgressPipeline::reset_video_index()
{
    last_video_index_ = undefined_index;
}

void EgressPipeline::reset_audio_index()
{
    last_audio_end_index_ = undefined_index;
}

EgressPipeline::Stats EgressPipeline::stats() const noexcept
{
    Stats result;
    result.video_frames = stat_video_frames_.load(std::memory_order_relaxed);
    result.video_dropped_late = stat_video_dropped_late_.load(std::memory_order_relaxed);
    result.video_dropped_clamped = stat_video_dropped_clamped_.load(std::memory_order_relaxed);
    result.video_errors = stat_video_errors_.load(std::memory_order_relaxed);
    result.audio_chunks = stat_audio_chunks_.load(std::memory_order_relaxed);
    result.audio_frames = stat_audio_frames_.load(std::memory_order_relaxed);
    result.audio_silence_chunks = stat_audio_silence_chunks_.load(std::memory_order_relaxed);
    result.audio_dropped = stat_audio_dropped_.load(std::memory_order_relaxed);
    result.audio_errors = stat_audio_errors_.load(std::memory_order_relaxed);
    return result;
}

void EgressPipeline::push_video(FrameView const& frame)
{
    if (!started())
    {
        return;
    }

    std::lock_guard lock{video_mutex_};
    if (!video_writer_ || !video_info_.grain_rate.valid() || (frame.width != config_.width) ||
        (frame.height != config_.height))
    {
        return;
    }

    offset_.calibrate(clock_, offset_recalibration_ns);

    auto const host_timestamp = (frame.host_timestamp_ns != 0)
                                    ? static_cast<Ns>(frame.host_timestamp_ns)
                                    : clock_.host_now_ns();
    auto const tai_timestamp = offset_.to_tai(host_timestamp);
    auto const tai_now = offset_.to_tai(clock_.host_now_ns());

    std::uint64_t index = timestamp_to_index(config_.frame_rate, tai_timestamp);
    std::uint64_t const now_index = timestamp_to_index(config_.frame_rate, tai_now);
    if ((index == undefined_index) || (now_index == undefined_index))
    {
        stat_video_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Never rewind the ring buffer head: a frame whose timestamp is older
    // than half the flow history is simply dropped.
    std::uint64_t const history = std::max<std::uint64_t>(1, video_info_.grain_count / 2U);
    if (index + history < now_index)
    {
        stat_video_dropped_late_.fetch_add(1, std::memory_order_relaxed);
        log_throttled(LogLevel::Warning, "dropping late video frame (" +
                                             std::to_string(now_index - index) + " grains behind)");
        return;
    }

    // Do not let a future timestamp push the head ahead of the facility
    // clock by more than one frame.
    if (index > now_index + 1)
    {
        index = now_index + 1;
        stat_video_dropped_clamped_.fetch_add(1, std::memory_order_relaxed);
    }

    if ((last_video_index_ != undefined_index) && (index <= last_video_index_))
    {
        stat_video_dropped_late_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto const stride = video_info_.stride;
    auto const convert_status =
        convert_to_v210(frame, video_buffer_.data(), video_buffer_.size(), stride);
    if (convert_status != Status::Ok)
    {
        stat_video_errors_.fetch_add(1, std::memory_order_relaxed);
        log_throttled(LogLevel::Error,
                      "unsupported OBS frame format: " + std::string{to_string(frame.format)});
        return;
    }

    GrainInfo grain;
    std::uint8_t* payload = nullptr;
    if (auto const status = video_writer_->open_grain(index, grain, payload); status != Status::Ok)
    {
        stat_video_errors_.fetch_add(1, std::memory_order_relaxed);
        log_throttled(LogLevel::Warning, "cannot open MXL grain " + std::to_string(index) + ": " +
                                             std::string{to_string(status)});
        return;
    }

    std::memcpy(payload, video_buffer_.data(), video_buffer_.size());
    grain.flags = 0;
    grain.grain_size = static_cast<std::uint32_t>(video_buffer_.size());
    grain.valid_slices = grain.total_slices;

    if (auto const status = video_writer_->commit_grain(grain); status != Status::Ok)
    {
        video_writer_->cancel_grain();
        stat_video_errors_.fetch_add(1, std::memory_order_relaxed);
        log_throttled(LogLevel::Warning, "cannot commit MXL grain " + std::to_string(index) + ": " +
                                             std::string{to_string(status)});
        return;
    }

    last_video_index_ = index;
    stat_video_frames_.fetch_add(1, std::memory_order_relaxed);
}
} // namespace obsmxl

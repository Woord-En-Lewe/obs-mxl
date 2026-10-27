// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/ingress.hpp>

#include <algorithm>
#include <cstring>
#include <string>

#include <obsmxl/flow-definition.hpp>

namespace obsmxl
{
namespace
{
constexpr std::uint64_t video_read_timeout_ns = 25'000'000ULL;
constexpr Ns reconnect_interval_ns = 500'000'000LL;
constexpr Ns metadata_check_interval_ns = 1'000'000'000LL;
constexpr Ns minimum_poll_interval_ns = 1'000'000LL;
constexpr Ns maximum_poll_interval_ns = 20'000'000LL;
constexpr std::uint16_t grain_valid_slices_all = 0xFFFFU;

bool same_video_info(VideoStreamInfo const& a, VideoStreamInfo const& b)
{
    return (a.width == b.width) && (a.height == b.height) && (a.stride == b.stride) &&
           (a.grain_rate == b.grain_rate) && (a.total_slices == b.total_slices);
}
} // namespace

struct IngressPipeline::AudioStorage
{
    std::vector<float> samples;
    std::array<float const*, max_audio_channels> channels{};
    std::uint32_t channel_count{0};
    std::uint32_t frames{0};
};

IngressPipeline::IngressPipeline(Backend& backend, Config config, Clock const& clock,
                                 Logger* logger)
    : backend_(backend), config_(std::move(config)), clock_(clock), logger_(logger)
{
    config_.playout_delay_ms = std::clamp(config_.playout_delay_ms, 0, 5000);
    if (config_.stall_frames_before_blank == 0)
    {
        config_.stall_frames_before_blank = 3;
    }
    offset_.calibrate(clock_, 0);
}

IngressPipeline::~IngressPipeline()
{
    stop();
    close_readers();
}

void IngressPipeline::stop() noexcept
{
    stop_requested_.store(true, std::memory_order_release);
}

IngressPipeline::Config const& IngressPipeline::config() const noexcept
{
    return config_;
}

IngressPipeline::Stats IngressPipeline::stats() const noexcept
{
    Stats result;
    result.video_frames = stat_video_frames_.load(std::memory_order_relaxed);
    result.blank_frames = stat_blank_frames_.load(std::memory_order_relaxed);
    result.dropped_late = stat_dropped_late_.load(std::memory_order_relaxed);
    result.timeouts = stat_timeouts_.load(std::memory_order_relaxed);
    result.resyncs = stat_resyncs_.load(std::memory_order_relaxed);
    result.audio_chunks = stat_audio_chunks_.load(std::memory_order_relaxed);
    result.audio_frames = stat_audio_frames_.load(std::memory_order_relaxed);
    result.audio_gaps = stat_audio_gaps_.load(std::memory_order_relaxed);
    result.reconnects = stat_reconnects_.load(std::memory_order_relaxed);
    return result;
}

bool IngressPipeline::ensure_readers()
{
    if (readers_open_ && (!config_.enable_audio || config_.audio_flow_id.empty() || audio_reader_))
    {
        return true;
    }

    if (!video_reader_)
    {
        if (auto const status = backend_.open_video_reader(config_.video_flow_id, video_reader_);
            status != Status::Ok)
        {
            if (logger_)
            {
                logger_->log(LogLevel::Debug, "cannot open video flow " + config_.video_flow_id +
                                                  ": " + std::string{to_string(status)} + " (" +
                                                  backend_.last_error() + ")");
            }
            return false;
        }

        if (auto const status = video_reader_->fetch_info(video_info_); status != Status::Ok)
        {
            close_readers();
            return false;
        }

        if ((video_info_.width == 0) || (video_info_.height == 0) ||
            (video_info_.stride < v210_line_stride(video_info_.width)) ||
            !video_info_.grain_rate.valid())
        {
            if (logger_)
            {
                logger_->log(LogLevel::Error, "video flow has an unsupported geometry");
            }
            close_readers();
            return false;
        }

        if (logger_)
        {
            logger_->log(LogLevel::Info, "reading MXL video flow " + config_.video_flow_id + " (" +
                                             std::to_string(video_info_.width) + "x" +
                                             std::to_string(video_info_.height) + " @" +
                                             to_string(video_info_.grain_rate) + ")");
        }
    }

    if (config_.enable_audio && !config_.audio_flow_id.empty() && !audio_reader_)
    {
        Ns const now = clock_.host_now_ns();
        if ((now - last_audio_open_attempt_) >= 500'000'000LL)
        {
            last_audio_open_attempt_ = now;

            if (backend_.open_audio_reader(config_.audio_flow_id, audio_reader_) == Status::Ok)
            {
                if (audio_reader_->fetch_info(audio_info_) == Status::Ok)
                {
                    if (logger_)
                    {
                        logger_->log(LogLevel::Info,
                                     "reading MXL audio flow " + config_.audio_flow_id + " (" +
                                         std::to_string(audio_info_.channels) + "ch @" +
                                         std::to_string(audio_info_.sample_rate) + "Hz)");
                    }
                }
                else
                {
                    audio_reader_.reset();
                }
            }
        }
    }

    readers_open_ = true;
    return true;
}

void IngressPipeline::close_readers()
{
    video_reader_.reset();
    audio_reader_.reset();
    video_info_ = {};
    audio_info_ = {};
    readers_open_ = false;
    reset_video_state();
    reset_audio_state();
}

void IngressPipeline::reset_video_state()
{
    last_video_index_ = undefined_index;
    consecutive_misses_ = 0;
    blank_buffer_ = {};
}

void IngressPipeline::reset_audio_state()
{
    last_audio_end_index_ = undefined_index;
    last_audio_host_ns_ = 0;
}

std::optional<IngressPipeline::VideoFrame> IngressPipeline::poll_video(std::uint64_t target_index)
{
    GrainInfo grain;
    std::uint8_t const* payload = nullptr;

    auto const status = video_reader_->get_grain(target_index, grain_valid_slices_all,
                                                 video_read_timeout_ns, grain, payload);

    switch (status)
    {
    case Status::Ok:
    {
        if (!payload || ((grain.flags & 0x1U) != 0) || (grain.valid_slices < grain.total_slices))
        {
            stat_timeouts_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        VideoFrame frame;
        frame.format = PixelFormat::V210;
        frame.width = video_info_.width;
        frame.height = video_info_.height;
        frame.stride = video_info_.stride;
        frame.data = payload;
        frame.host_timestamp_ns = static_cast<std::uint64_t>(
            offset_.to_host(index_to_timestamp(video_info_.grain_rate, grain.index)));
        frame.blank = false;

        stat_video_frames_.fetch_add(1, std::memory_order_relaxed);
        return frame;
    }

    case Status::TooLate:
    {
        stat_dropped_late_.fetch_add(1, std::memory_order_relaxed);
        stat_resyncs_.fetch_add(1, std::memory_order_relaxed);

        std::uint64_t head = undefined_index;
        if ((video_reader_->head_index(head) == Status::Ok) && (head != undefined_index) &&
            (head > 0))
        {
            last_video_index_ = head - 1;
        }
        else
        {
            last_video_index_ = undefined_index;
        }

        if (logger_)
        {
            logger_->log(LogLevel::Warning, "video flow overrun, resynchronising to head");
        }
        return std::nullopt;
    }

    case Status::Timeout:
    case Status::TooEarly:
        stat_timeouts_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;

    case Status::NotFound:
    case Status::FlowInvalid:
        stat_reconnects_.fetch_add(1, std::memory_order_relaxed);
        close_readers();
        return std::nullopt;

    default:
        if (logger_)
        {
            logger_->log(LogLevel::Warning, "video read failed: " + std::string{to_string(status)});
        }
        return std::nullopt;
    }
}

void IngressPipeline::poll_audio(std::uint64_t current_tai, Result& result, Ns& next_poll_host)
{
    if (!config_.enable_audio || !audio_reader_ || (audio_info_.sample_rate == 0) ||
        (audio_info_.channels == 0))
    {
        return;
    }

    Rational const rate{audio_info_.sample_rate, 1};
    Ns const delay_ns = static_cast<Ns>(config_.playout_delay_ms) * 1'000'000LL;
    Ns const target_tai = current_tai - delay_ns;
    if (target_tai <= 0)
    {
        return;
    }

    std::uint64_t const target_end = timestamp_to_index(rate, target_tai);
    if (target_end == undefined_index)
    {
        return;
    }

    if (last_audio_end_index_ == undefined_index)
    {
        last_audio_end_index_ = target_end;
        next_poll_host = offset_.to_host(index_to_timestamp(rate, target_end + 1));
        return;
    }

    if (target_end <= last_audio_end_index_)
    {
        next_poll_host = offset_.to_host(index_to_timestamp(rate, target_end + 1));
        return;
    }

    std::uint64_t count = target_end - last_audio_end_index_;
    std::uint64_t const max_catchup = std::max<std::uint64_t>(
        1, (static_cast<std::uint64_t>(audio_info_.sample_rate) * config_.max_audio_catchup_ms) /
               1000U);

    if (count > max_catchup)
    {
        auto const dropped = count - max_catchup;
        last_audio_end_index_ += dropped;
        count = max_catchup;
        stat_audio_gaps_.fetch_add(1, std::memory_order_relaxed);
    }

    if (count == 0)
    {
        return;
    }

    auto storage = std::make_shared<AudioStorage>();
    storage->channel_count =
        std::min(audio_info_.channels, static_cast<std::uint32_t>(max_audio_channels));
    storage->frames = static_cast<std::uint32_t>(count);
    storage->samples.assign(static_cast<std::size_t>(count) * storage->channel_count, 0.0F);
    for (std::uint32_t channel = 0; channel < storage->channel_count; ++channel)
    {
        storage->channels[channel] =
            storage->samples.data() + static_cast<std::size_t>(channel) * storage->frames;
    }

    std::uint64_t const start_index = last_audio_end_index_;
    std::uint64_t remaining = count;
    std::uint64_t read_end = last_audio_end_index_;
    std::uint32_t write_offset = 0;

    std::uint64_t const max_batch = std::max<std::uint64_t>(
        1, std::min<std::uint64_t>(static_cast<std::uint64_t>(audio_info_.sample_rate) / 50U,
                                   std::max<std::uint64_t>(1, audio_info_.buffer_length / 2U)));

    while (remaining > 0)
    {
        auto const batch = std::min<std::uint64_t>(remaining, max_batch);
        ConstSampleSpan span;
        auto const status = audio_reader_->get_samples(read_end + batch, batch,
                                                       config_.audio_read_timeout_ns, span);

        if (status == Status::Timeout || status == Status::TooEarly)
        {
            break;
        }

        if (status == Status::TooLate)
        {
            stat_audio_gaps_.fetch_add(1, std::memory_order_relaxed);
            stat_resyncs_.fetch_add(1, std::memory_order_relaxed);
            last_audio_end_index_ = target_end;
            return;
        }

        if ((status == Status::NotFound) || (status == Status::FlowInvalid))
        {
            audio_reader_.reset();
            reset_audio_state();
            return;
        }

        if (status != Status::Ok)
        {
            if (logger_)
            {
                logger_->log(LogLevel::Warning,
                             "audio read failed: " + std::string{to_string(status)});
            }
            break;
        }

        std::uint32_t const channels =
            std::min(storage->channel_count, static_cast<std::uint32_t>(span.count));

        for (std::uint32_t channel = 0; channel < channels; ++channel)
        {
            auto* dst = storage->samples.data() +
                        static_cast<std::size_t>(channel) * storage->frames + write_offset;
            for (auto const& fragment : span.fragments)
            {
                if (fragment.size == 0)
                {
                    continue;
                }
                auto const* src =
                    fragment.pointer + static_cast<std::size_t>(channel) * span.stride;
                auto const samples = fragment.size / sizeof(float);
                std::memcpy(dst, src, samples * sizeof(float));
                dst += samples;
            }
        }

        read_end += batch;
        remaining -= batch;
        write_offset += static_cast<std::uint32_t>(batch);
    }

    if (write_offset == 0)
    {
        return;
    }

    storage->frames = write_offset;
    for (std::uint32_t channel = 0; channel < storage->channel_count; ++channel)
    {
        storage->channels[channel] =
            storage->samples.data() + static_cast<std::size_t>(channel) * write_offset;
    }

    auto const timestamp =
        static_cast<std::uint64_t>(offset_.to_host(index_to_timestamp(rate, start_index)));

    if ((last_audio_host_ns_ != 0) && (timestamp <= last_audio_host_ns_))
    {
        // Never emit audio going backwards in time.
        last_audio_end_index_ = read_end;
        return;
    }
    last_audio_host_ns_ = timestamp;
    last_audio_end_index_ = read_end;

    AudioChunk chunk;
    chunk.channel_count = storage->channel_count;
    chunk.frames = storage->frames;
    chunk.sample_rate = audio_info_.sample_rate;
    chunk.host_timestamp_ns = timestamp;
    chunk.channels = storage->channels;
    chunk.keepalive = storage;
    result.audio = std::move(chunk);

    stat_audio_chunks_.fetch_add(1, std::memory_order_relaxed);
    stat_audio_frames_.fetch_add(storage->frames, std::memory_order_relaxed);
}

void IngressPipeline::emit_blank(std::uint64_t target_index, Result& result)
{
    if (!blank_buffer_.matches(video_info_.width, video_info_.height))
    {
        blank_buffer_.reset(video_info_.width, video_info_.height);
        fill_v210_black(blank_buffer_.data(), blank_buffer_.width, blank_buffer_.height,
                        blank_buffer_.stride);
    }

    VideoFrame frame;
    frame.format = PixelFormat::V210;
    frame.width = video_info_.width;
    frame.height = video_info_.height;
    frame.stride = blank_buffer_.stride;
    frame.data = blank_buffer_.data();
    frame.host_timestamp_ns = static_cast<std::uint64_t>(
        offset_.to_host(index_to_timestamp(video_info_.grain_rate, target_index)));
    frame.blank = true;

    result.video = frame;
    stat_blank_frames_.fetch_add(1, std::memory_order_relaxed);
}

IngressPipeline::Result IngressPipeline::poll()
{
    Result result;
    if (stop_requested_.load(std::memory_order_acquire))
    {
        return result;
    }

    offset_.calibrate(clock_, 1'000'000'000LL);

    Ns const host_now = clock_.host_now_ns();
    Ns const current_tai = offset_.to_tai(host_now);

    if (!readers_open_)
    {
        if ((host_now - last_reconnect_host_) < reconnect_interval_ns)
        {
            result.next_poll_host_ns =
                static_cast<std::uint64_t>(host_now + maximum_poll_interval_ns);
            return result;
        }
        last_reconnect_host_ = host_now;

        if (!ensure_readers())
        {
            result.next_poll_host_ns =
                static_cast<std::uint64_t>(host_now + maximum_poll_interval_ns);
            return result;
        }
    }

    Ns const delay_ns = static_cast<Ns>(config_.playout_delay_ms) * 1'000'000LL;
    Ns const target_tai = std::max<Ns>(0, current_tai - delay_ns);

    Ns next_poll = host_now + maximum_poll_interval_ns;
    poll_audio(static_cast<std::uint64_t>(current_tai), result, next_poll);

    if (result.audio)
    {
        auto const rate = Rational{result.audio->sample_rate, 1};
        auto const next_audio =
            offset_.to_host(index_to_timestamp(rate, last_audio_end_index_ + 1));
        next_poll =
            std::min(next_poll, std::max<Ns>(next_audio, host_now + minimum_poll_interval_ns));
    }

    if (video_reader_ && (video_info_.width > 0))
    {
        if ((host_now - last_metadata_check_host_) >= metadata_check_interval_ns)
        {
            last_metadata_check_host_ = host_now;
            VideoStreamInfo latest;
            if (video_reader_->fetch_info(latest) == Status::Ok)
            {
                if (!same_video_info(latest, video_info_))
                {
                    if (logger_)
                    {
                        logger_->log(LogLevel::Info,
                                     "video flow geometry/rate changed, reinitialising reader");
                    }
                    video_info_ = latest;
                    reset_video_state();
                    stat_resyncs_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                close_readers();
                result.next_poll_host_ns =
                    static_cast<std::uint64_t>(host_now + maximum_poll_interval_ns);
                return result;
            }
        }

        std::uint64_t const target_index = timestamp_to_index(video_info_.grain_rate, target_tai);

        if (target_index != undefined_index)
        {
            bool const waiting_for_next =
                (last_video_index_ != undefined_index) && (target_index <= last_video_index_);

            if (!waiting_for_next)
            {
                auto frame = poll_video(target_index);
                if (frame)
                {
                    result.video = std::move(*frame);
                    last_video_index_ = target_index;
                    consecutive_misses_ = 0;
                }
                else if (readers_open_)
                {
                    ++consecutive_misses_;
                    if (config_.allow_blank_on_stall &&
                        (consecutive_misses_ >= config_.stall_frames_before_blank))
                    {
                        emit_blank(target_index, result);
                        last_video_index_ = target_index;
                        consecutive_misses_ = 0;
                    }
                }
            }
        }
    }

    if (video_reader_ && video_info_.grain_rate.valid())
    {
        auto const next_index =
            (last_video_index_ != undefined_index) ? (last_video_index_ + 1) : 0;
        auto const next_video =
            offset_.to_host(index_to_timestamp(video_info_.grain_rate, next_index));
        next_poll =
            std::min(next_poll, std::max<Ns>(next_video, host_now + minimum_poll_interval_ns));
    }

    result.next_poll_host_ns =
        static_cast<std::uint64_t>(std::max<Ns>(next_poll, host_now + minimum_poll_interval_ns));
    return result;
}
} // namespace obsmxl

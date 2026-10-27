// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/egress.hpp>

#include <algorithm>
#include <cstring>

namespace obsmxl
{
namespace
{
constexpr Ns offset_recalibration_ns = 5'000'000'000LL;
constexpr std::uint32_t max_silence_gap_ms = 1000;

void copy_channel_to_span(MutableSampleSpan& span, std::uint32_t channel, float const* source,
                          std::uint32_t samples)
{
    std::uint32_t written = 0;
    for (auto const& fragment : span.fragments)
    {
        if ((fragment.size == 0) || (written >= samples))
        {
            continue;
        }
        auto const count = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(fragment.size / sizeof(float)), samples - written);
        auto* destination = fragment.pointer + static_cast<std::size_t>(channel) * span.stride;
        std::memcpy(destination, source + written, static_cast<std::size_t>(count) * sizeof(float));
        written += count;
    }
}

void zero_channel_in_span(MutableSampleSpan& span, std::uint32_t channel)
{
    for (auto const& fragment : span.fragments)
    {
        if (fragment.size == 0)
        {
            continue;
        }
        auto* destination = fragment.pointer + static_cast<std::size_t>(channel) * span.stride;
        std::memset(destination, 0, fragment.size);
    }
}
} // namespace

void EgressPipeline::write_audio_range(std::uint64_t start_index, PlanarAudioView const& audio,
                                       std::uint32_t sample_offset, std::uint32_t sample_count)
{
    if (!audio_writer_ || (sample_count == 0))
    {
        return;
    }

    auto const channels = std::min(audio.channel_count, config_.channels);
    std::uint64_t const max_batch = std::max<std::uint64_t>(
        1, std::min<std::uint64_t>(config_.audio_max_batch,
                                   std::max<std::uint64_t>(1, audio_info_.buffer_length / 2U)));

    std::uint64_t cursor = start_index;
    std::uint32_t offset = sample_offset;
    std::uint32_t remaining = sample_count;

    while (remaining > 0)
    {
        auto const batch =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, max_batch));

        MutableSampleSpan span;
        if (auto const status = audio_writer_->open_samples(cursor + batch, batch, span);
            status != Status::Ok)
        {
            stat_audio_errors_.fetch_add(1, std::memory_order_relaxed);
            log_throttled(LogLevel::Warning, "cannot open MXL audio samples at " +
                                                 std::to_string(cursor) + ": " +
                                                 std::string{to_string(status)});
            reset_audio_index();
            return;
        }

        for (std::uint32_t channel = 0; channel < config_.channels; ++channel)
        {
            if ((channel < channels) && (audio.channels[channel] != nullptr))
            {
                copy_channel_to_span(span, channel, audio.channels[channel] + offset, batch);
            }
            else
            {
                zero_channel_in_span(span, channel);
            }
        }

        if (auto const status = audio_writer_->commit_samples(); status != Status::Ok)
        {
            audio_writer_->cancel_samples();
            stat_audio_errors_.fetch_add(1, std::memory_order_relaxed);
            log_throttled(LogLevel::Warning, "cannot commit MXL audio samples at " +
                                                 std::to_string(cursor) + ": " +
                                                 std::string{to_string(status)});
            reset_audio_index();
            return;
        }

        cursor += batch;
        offset += batch;
        remaining -= batch;
    }
}

void EgressPipeline::write_audio_silence(std::uint64_t start_index, std::uint32_t sample_count)
{
    if (!audio_writer_ || (sample_count == 0))
    {
        return;
    }

    std::uint64_t const max_batch = std::max<std::uint64_t>(
        1, std::min<std::uint64_t>(config_.audio_max_batch,
                                   std::max<std::uint64_t>(1, audio_info_.buffer_length / 2U)));

    std::uint64_t cursor = start_index;
    std::uint32_t remaining = sample_count;

    while (remaining > 0)
    {
        auto const batch =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, max_batch));

        MutableSampleSpan span;
        if (auto const status = audio_writer_->open_samples(cursor + batch, batch, span);
            status != Status::Ok)
        {
            stat_audio_errors_.fetch_add(1, std::memory_order_relaxed);
            log_throttled(LogLevel::Warning,
                          "cannot open MXL silence samples at " + std::to_string(cursor));
            reset_audio_index();
            return;
        }

        for (std::uint32_t channel = 0; channel < config_.channels; ++channel)
        {
            zero_channel_in_span(span, channel);
        }

        if (auto const status = audio_writer_->commit_samples(); status != Status::Ok)
        {
            audio_writer_->cancel_samples();
            stat_audio_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        cursor += batch;
        remaining -= batch;
        stat_audio_silence_chunks_.fetch_add(1, std::memory_order_relaxed);
    }

    if (remaining == 0)
    {
        last_audio_end_index_ = cursor;
    }
}

void EgressPipeline::push_audio(PlanarAudioView const& audio)
{
    if (!started() || !config_.include_audio || (audio.frames == 0) ||
        (audio.sample_rate != config_.sample_rate))
    {
        return;
    }

    std::lock_guard lock{audio_mutex_};
    if (!audio_writer_)
    {
        return;
    }

    offset_.calibrate(clock_, offset_recalibration_ns);

    Rational const rate{config_.sample_rate, 1};
    auto const host_timestamp = (audio.host_timestamp_ns != 0)
                                    ? static_cast<Ns>(audio.host_timestamp_ns)
                                    : clock_.host_now_ns();
    auto const tai_timestamp = offset_.to_tai(host_timestamp);

    auto const start_index = timestamp_to_index(rate, tai_timestamp);
    if (start_index == undefined_index)
    {
        return;
    }

    auto end_index = start_index + audio.frames;
    std::uint32_t sample_offset = 0;
    std::uint32_t frames = audio.frames;

    if (last_audio_end_index_ == undefined_index)
    {
        last_audio_end_index_ = start_index;
    }

    if (end_index <= last_audio_end_index_)
    {
        stat_audio_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (start_index < last_audio_end_index_)
    {
        auto const skip = static_cast<std::uint32_t>(last_audio_end_index_ - start_index);
        if (skip >= frames)
        {
            stat_audio_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        sample_offset = skip;
        frames -= skip;
        end_index = last_audio_end_index_ + frames;
    }
    else if (start_index > last_audio_end_index_)
    {
        auto const gap = start_index - last_audio_end_index_;
        auto const max_gap =
            static_cast<std::uint64_t>(config_.sample_rate) * max_silence_gap_ms / 1000U;

        if (gap > max_gap)
        {
            // The host was stalled for a long time; resynchronise instead
            // of writing seconds of silence into the flow.
            last_audio_end_index_ = start_index;
        }
        else
        {
            write_audio_silence(last_audio_end_index_, static_cast<std::uint32_t>(gap));
            if (last_audio_end_index_ != start_index)
            {
                return;
            }
        }
    }

    if (last_audio_end_index_ != start_index)
    {
        // Silence insertion failed and reset the index; skip this chunk.
        return;
    }

    write_audio_range(start_index, audio, sample_offset, frames);
    if (last_audio_end_index_ == start_index)
    {
        last_audio_end_index_ = start_index + frames;
        stat_audio_chunks_.fetch_add(1, std::memory_order_relaxed);
        stat_audio_frames_.fetch_add(frames, std::memory_order_relaxed);
    }
}
} // namespace obsmxl

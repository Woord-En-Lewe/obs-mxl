// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <obsmxl/backend.hpp>
#include <obsmxl/flow-definition.hpp>
#include <obsmxl/time.hpp>
#include <obsmxl/video-format.hpp>

namespace obsmxl::test
{
class ManualClock final : public Clock
{
public:
    explicit ManualClock(Ns host = 1'000'000'000LL, Ns tai = 1'700'000'000'000'000'000LL)
        : host_(host), tai_(tai)
    {
    }

    Ns host_now_ns() const noexcept override { return host_; }
    Ns tai_now_ns() const noexcept override { return tai_; }

    void advance(Ns nanoseconds) noexcept
    {
        host_ += nanoseconds;
        tai_ += nanoseconds;
    }

    void set_tai(Ns tai) noexcept
    {
        host_ += tai - tai_;
        tai_ = tai;
    }

    Ns host() const noexcept { return host_; }
    Ns tai() const noexcept { return tai_; }

private:
    Ns host_;
    Ns tai_;
};

inline std::string program_video_uuid()
{
    return "3fa85f64-5717-4562-b3fc-2c963f66afa6";
}

inline std::string program_audio_uuid()
{
    return "7c9e6679-7425-40de-944b-e07fc1f90ae7";
}

inline std::string preview_video_uuid()
{
    return "9b2f4d21-6a1e-4c02-8b9d-2f6a8c1d3e4f";
}

inline VideoFlowSpec make_video_spec(std::uint32_t width = 6, std::uint32_t height = 2,
                                     std::string id = program_video_uuid())
{
    VideoFlowSpec spec;
    spec.id = std::move(id);
    spec.label = "Test Video";
    spec.group_hint = "OBS:Program";
    spec.grain_rate = Rational{60, 1};
    spec.width = width;
    spec.height = height;
    return spec;
}

inline AudioFlowSpec make_audio_spec(std::uint32_t channels = 2,
                                     std::string id = program_audio_uuid())
{
    AudioFlowSpec spec;
    spec.id = std::move(id);
    spec.label = "Test Audio";
    spec.group_hint = "OBS:Program";
    spec.sample_rate = Rational{48000, 1};
    spec.channel_count = channels;
    return spec;
}

/** Writes a fully valid grain and commits it. */
inline Status write_video_grain(VideoWriter& writer, std::uint64_t index, std::uint8_t fill)
{
    GrainInfo grain;
    std::uint8_t* payload = nullptr;
    if (auto const status = writer.open_grain(index, grain, payload); status != Status::Ok)
    {
        return status;
    }
    std::memset(payload, fill, writer.info().stride * writer.info().height);
    grain.flags = 0;
    grain.valid_slices = grain.total_slices;
    return writer.commit_grain(grain);
}

inline Status write_audio_samples(AudioWriter& writer, std::uint64_t end_index,
                                  std::uint32_t frames, float value)
{
    MutableSampleSpan span;
    if (auto const status = writer.open_samples(end_index, frames, span); status != Status::Ok)
    {
        return status;
    }

    auto const channels = writer.info().channels;
    for (std::uint32_t channel = 0; channel < channels; ++channel)
    {
        std::uint32_t written = 0;
        for (auto const& fragment : span.fragments)
        {
            if (fragment.size == 0)
            {
                continue;
            }
            auto* dst = reinterpret_cast<float*>(fragment.pointer + channel * span.stride);
            auto const count = static_cast<std::uint32_t>(fragment.size / sizeof(float));
            for (std::uint32_t i = 0; i < count; ++i)
            {
                dst[i] = value + static_cast<float>(channel) * 100.0F;
            }
            written += count;
        }
        (void)written;
    }

    return writer.commit_samples();
}

inline std::vector<std::uint8_t> make_nv12(std::uint32_t width, std::uint32_t height,
                                           std::uint8_t y_start = 16, std::uint8_t uv = 128)
{
    std::vector<std::uint8_t> data;
    data.resize(static_cast<std::size_t>(width) * height +
                static_cast<std::size_t>(width) * ((height + 1) / 2));
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            data[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint8_t>(y_start + x * 16);
        }
    }
    std::memset(data.data() + static_cast<std::size_t>(width) * height, uv,
                static_cast<std::size_t>(width) * ((height + 1) / 2));
    return data;
}

/** Returns a frame view for a tightly packed NV12 test buffer. */
inline FrameView nv12_view(std::vector<std::uint8_t> const& data, std::uint32_t width,
                           std::uint32_t height, std::uint64_t timestamp = 0)
{
    FrameView view;
    view.format = PixelFormat::NV12;
    view.width = width;
    view.height = height;
    view.planes[0] = {data.data(), width};
    view.planes[1] = {data.data() + static_cast<std::size_t>(width) * height, width};
    view.host_timestamp_ns = timestamp;
    return view;
}
} // namespace obsmxl::test

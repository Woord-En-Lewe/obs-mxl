// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <cstdint>
#include <string>

#include <obsmxl/rational.hpp>

namespace obsmxl
{
/**
 * Description of a V210 video flow as defined by NMOS IS-04 and
 * implemented by the MXL SDK. MXL v1.1 only supports 10 bit 4:2:2
 * (video/v210) for video, so the descriptor is intentionally narrow.
 */
struct VideoFlowSpec
{
    std::string id; // flow UUID (also the directory name)
    std::string label{"OBS MXL Video"};
    std::string group_hint{"OBS:Video"}; // <function>:<role>
    Rational grain_rate{60, 1};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::string colorspace{"BT709"};
};

/** Description of a float32 audio flow (audio/float32). */
struct AudioFlowSpec
{
    std::string id; // flow UUID
    std::string label{"OBS MXL Audio"};
    std::string group_hint{"OBS:Audio"};
    Rational sample_rate{48000, 1};
    std::uint32_t channel_count{2};
    std::string source_id;
    std::string device_id;
};

/** Serialises a video flow to the NMOS flow definition consumed by MXL. */
std::string make_video_flow_json(VideoFlowSpec const& spec);

/** Serialises an audio flow to the NMOS flow definition consumed by MXL. */
std::string make_audio_flow_json(AudioFlowSpec const& spec);

/** MXL V210 line length (a slice is one full line including padding). */
constexpr std::uint32_t v210_line_stride(std::uint32_t width) noexcept
{
    return ((width + 47U) / 48U) * 128U;
}

/** Number of slices (lines) in a V210 grain of the given height. */
constexpr std::uint32_t v210_total_slices(std::uint32_t height) noexcept
{
    return height;
}

/** Payload size in bytes for a V210 grain. */
constexpr std::size_t v210_payload_size(std::uint32_t width, std::uint32_t height) noexcept
{
    return static_cast<std::size_t>(v210_line_stride(width)) * height;
}

/** Options passed to mxlCreateFlowWriter. */
std::string make_writer_options_json(std::uint32_t max_commit_batch_size_hint);

/** Validates the fields that the plugin requires before creating a flow. */
bool validate_video_flow_spec(VideoFlowSpec const& spec, std::string* error = nullptr);
bool validate_audio_flow_spec(AudioFlowSpec const& spec, std::string* error = nullptr);
} // namespace obsmxl

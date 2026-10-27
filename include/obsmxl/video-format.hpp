// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <obsmxl/status.hpp>

namespace obsmxl
{
/**
 * Frame formats that can arrive from OBS on the egress path, plus the MXL
 * V210 wire format used in both directions.
 */
enum class PixelFormat : std::uint8_t
{
    V210,
    NV12,
    P010,
    I420,
    YV12,
    I010,
    BGRA,
};

std::string_view to_string(PixelFormat format) noexcept;

/** True when the format is a packed/planar 10 bit YUV 4:2:2 layout. */
constexpr bool is_mxl_native(PixelFormat format) noexcept
{
    return format == PixelFormat::V210;
}

struct PlaneView
{
    std::uint8_t const* data{nullptr};
    std::uint32_t linesize{0};
};

struct FrameView
{
    PixelFormat format{PixelFormat::V210};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::array<PlaneView, 3> planes{};
    std::uint64_t host_timestamp_ns{0};
};

inline constexpr std::size_t max_audio_channels = 32;

struct PlanarAudioView
{
    std::array<float const*, max_audio_channels> channels{};
    std::uint32_t channel_count{0};
    std::uint32_t frames{0};
    std::uint32_t sample_rate{48000};
    std::uint64_t host_timestamp_ns{0};
};

/** Owned V210 buffer used for blank frames and conversions. */
struct V210Buffer
{
    std::vector<std::uint8_t> bytes;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};

    void reset(std::uint32_t new_width, std::uint32_t new_height);
    [[nodiscard]] bool matches(std::uint32_t new_width, std::uint32_t new_height) const noexcept;
    [[nodiscard]] std::uint8_t* data() noexcept { return bytes.data(); }
    [[nodiscard]] std::uint8_t const* data() const noexcept { return bytes.data(); }
};

/**
 * Converts one OBS frame to a V210 grain payload.
 *
 * dst must be at least dst_stride * height bytes. Only the active bytes of
 * each line are written; padding between v210_line_stride(width) and
 * dst_stride is left untouched (buffers are zeroed once on allocation).
 *
 * For 4:2:0 inputs chroma is upsampled horizontally by sample replication
 * and 8 bit samples are shifted into the 10 bit range. BGRA is converted
 * using the BT.709 limited range matrix.
 */
Status convert_to_v210(FrameView const& frame, std::uint8_t* dst, std::size_t dst_size,
                       std::uint32_t dst_stride);

/** Fills a V210 buffer with limited range black (Y=64, Cb=Cr=512). */
void fill_v210_black(std::uint8_t* dst, std::uint32_t width, std::uint32_t height,
                     std::uint32_t stride);

/** Maps an OBS video format name to a PixelFormat. */
std::optional<PixelFormat> pixel_format_from_name(std::string_view name) noexcept;

/** Byte size of a V210 frame with the given stride. */
constexpr std::size_t v210_buffer_size(std::uint32_t stride, std::uint32_t height) noexcept
{
    return static_cast<std::size_t>(stride) * height;
}
} // namespace obsmxl

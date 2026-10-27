// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/video-format.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

#include <obsmxl/flow-definition.hpp>

namespace obsmxl
{
static_assert(std::endian::native == std::endian::little,
              "V210 packing assumes a little-endian host");

namespace
{
constexpr std::uint16_t black_y_10 = 64;  // 16 << 2
constexpr std::uint16_t black_c_10 = 512; // 128 << 2

inline std::uint16_t clamp_u16(std::int32_t value, std::uint16_t low, std::uint16_t high) noexcept
{
    if (value < static_cast<std::int32_t>(low))
    {
        return low;
    }
    if (value > static_cast<std::int32_t>(high))
    {
        return high;
    }
    return static_cast<std::uint16_t>(value);
}

inline std::uint16_t expand_8_to_10(std::uint32_t value) noexcept
{
    return static_cast<std::uint16_t>(value << 2U);
}

inline std::uint16_t expand_p010(std::uint16_t value) noexcept
{
    return static_cast<std::uint16_t>(value >> 6U);
}

/**
 * Packs 6 luma and 3 chroma samples into one 16 byte V210 group.
 *
 *   word0 = Cb0 | Y0 << 10 | Cr0 << 20
 *   word1 = Y1  | Cb1 << 10 | Y2 << 20
 *   word2 = Cr1 | Y3 << 10 | Cb2 << 20
 *   word3 = Y4  | Cr2 << 10 | Y5 << 20
 */
inline void pack_group(std::uint16_t const y[6], std::uint16_t const cb[3],
                       std::uint16_t const cr[3], std::uint8_t* out) noexcept
{
    std::uint32_t words[4]{};
    words[0] = static_cast<std::uint32_t>(cb[0] & 0x3FFU) |
               (static_cast<std::uint32_t>(y[0] & 0x3FFU) << 10U) |
               (static_cast<std::uint32_t>(cr[0] & 0x3FFU) << 20U);
    words[1] = static_cast<std::uint32_t>(y[1] & 0x3FFU) |
               (static_cast<std::uint32_t>(cb[1] & 0x3FFU) << 10U) |
               (static_cast<std::uint32_t>(y[2] & 0x3FFU) << 20U);
    words[2] = static_cast<std::uint32_t>(cr[1] & 0x3FFU) |
               (static_cast<std::uint32_t>(y[3] & 0x3FFU) << 10U) |
               (static_cast<std::uint32_t>(cb[2] & 0x3FFU) << 20U);
    words[3] = static_cast<std::uint32_t>(y[4] & 0x3FFU) |
               (static_cast<std::uint32_t>(cr[2] & 0x3FFU) << 10U) |
               (static_cast<std::uint32_t>(y[5] & 0x3FFU) << 20U);
    std::memcpy(out, words, sizeof(words));
}

/**
 * Common 4:2:0 -> 4:2:2 packed conversion.
 *
 * get_y(x) returns the 10 bit luma sample for pixel x, get_cb(x) and
 * get_cr(x) return the 10 bit chroma samples for the 2 pixel pair that
 * contains x. The caller clamps x into the active picture.
 */
template<typename LumaFn, typename CbFn, typename CrFn>
void pack_row_420(std::uint32_t width, LumaFn&& get_y, CbFn&& get_cb, CrFn&& get_cr,
                  std::uint8_t* dst)
{
    std::uint32_t const active_width = (width > 0) ? width : 1;
    std::uint8_t* out = dst;

    for (std::uint32_t x = 0; x < width; x += 6)
    {
        std::uint16_t y[6]{};
        std::uint16_t cb[3]{};
        std::uint16_t cr[3]{};

        for (std::uint32_t i = 0; i < 6; ++i)
        {
            std::uint32_t const px = (x + i < width) ? (x + i) : (active_width - 1);
            y[i] = get_y(px);
        }
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            std::uint32_t const px = (x + i * 2 < width) ? (x + i * 2) : (active_width - 1);
            cb[i] = get_cb(px);
            cr[i] = get_cr(px);
        }

        pack_group(y, cb, cr, out);
        out += 16;
    }
}

std::uint16_t read_u16(std::uint8_t const* data, std::size_t offset) noexcept
{
    std::uint16_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

Status convert_nv12(FrameView const& frame, std::uint8_t* dst, std::uint32_t stride)
{
    if (!frame.planes[0].data || !frame.planes[1].data)
    {
        return Status::InvalidArgument;
    }
    if ((frame.planes[0].linesize < frame.width) ||
        (frame.planes[1].linesize < ((frame.width + 1) / 2) * 2))
    {
        return Status::InvalidArgument;
    }

    std::uint32_t const chroma_width = (frame.width + 1) / 2;
    for (std::uint32_t row = 0; row < frame.height; ++row)
    {
        std::uint8_t const* y_row =
            frame.planes[0].data + static_cast<std::size_t>(row) * frame.planes[0].linesize;
        std::uint8_t const* uv_row =
            frame.planes[1].data + static_cast<std::size_t>(row / 2) * frame.planes[1].linesize;
        auto* dst_row = dst + static_cast<std::size_t>(row) * stride;

        pack_row_420(
            frame.width, [&](std::uint32_t x) { return expand_8_to_10(y_row[x]); },
            [&](std::uint32_t x)
            {
                std::uint32_t const c = std::min(x / 2, chroma_width - 1);
                return expand_8_to_10(uv_row[c * 2]);
            },
            [&](std::uint32_t x)
            {
                std::uint32_t const c = std::min(x / 2, chroma_width - 1);
                return expand_8_to_10(uv_row[c * 2 + 1]);
            },
            dst_row);
    }
    return Status::Ok;
}

Status convert_p010(FrameView const& frame, std::uint8_t* dst, std::uint32_t stride)
{
    if (!frame.planes[0].data || !frame.planes[1].data)
    {
        return Status::InvalidArgument;
    }
    if ((frame.planes[0].linesize < frame.width * 2) ||
        (frame.planes[1].linesize < ((frame.width + 1) / 2) * 4))
    {
        return Status::InvalidArgument;
    }

    std::uint32_t const chroma_width = (frame.width + 1) / 2;
    for (std::uint32_t row = 0; row < frame.height; ++row)
    {
        std::uint8_t const* y_row =
            frame.planes[0].data + static_cast<std::size_t>(row) * frame.planes[0].linesize;
        std::uint8_t const* uv_row =
            frame.planes[1].data + static_cast<std::size_t>(row / 2) * frame.planes[1].linesize;
        auto* dst_row = dst + static_cast<std::size_t>(row) * stride;

        pack_row_420(
            frame.width, [&](std::uint32_t x) { return expand_p010(read_u16(y_row, x * 2)); },
            [&](std::uint32_t x)
            {
                std::uint32_t const c = std::min(x / 2, chroma_width - 1);
                return expand_p010(read_u16(uv_row, c * 4));
            },
            [&](std::uint32_t x)
            {
                std::uint32_t const c = std::min(x / 2, chroma_width - 1);
                return expand_p010(read_u16(uv_row, c * 4 + 2));
            },
            dst_row);
    }
    return Status::Ok;
}

Status convert_i010(FrameView const& frame, std::uint8_t* dst, std::uint32_t stride)
{
    if (!frame.planes[0].data || !frame.planes[1].data || !frame.planes[2].data)
    {
        return Status::InvalidArgument;
    }
    if ((frame.planes[0].linesize < frame.width * 2) ||
        (frame.planes[1].linesize < ((frame.width + 1) / 2) * 2) ||
        (frame.planes[2].linesize < ((frame.width + 1) / 2) * 2))
    {
        return Status::InvalidArgument;
    }

    std::uint32_t const chroma_width = (frame.width + 1) / 2;
    for (std::uint32_t row = 0; row < frame.height; ++row)
    {
        std::uint8_t const* y_row =
            frame.planes[0].data + static_cast<std::size_t>(row) * frame.planes[0].linesize;
        std::uint8_t const* u_row =
            frame.planes[1].data + static_cast<std::size_t>(row / 2) * frame.planes[1].linesize;
        std::uint8_t const* v_row =
            frame.planes[2].data + static_cast<std::size_t>(row / 2) * frame.planes[2].linesize;
        auto* dst_row = dst + static_cast<std::size_t>(row) * stride;

        pack_row_420(
            frame.width, [&](std::uint32_t x) { return expand_p010(read_u16(y_row, x * 2)); },
            [&](std::uint32_t x)
            {
                std::uint32_t const c = std::min(x / 2, chroma_width - 1);
                return expand_p010(read_u16(u_row, c * 2));
            },
            [&](std::uint32_t x)
            {
                std::uint32_t const c = std::min(x / 2, chroma_width - 1);
                return expand_p010(read_u16(v_row, c * 2));
            },
            dst_row);
    }
    return Status::Ok;
}

Status convert_planar420(FrameView const& frame, std::uint8_t* dst, std::uint32_t stride,
                         std::size_t u_plane, std::size_t v_plane)
{
    if (!frame.planes[0].data || !frame.planes[u_plane].data || !frame.planes[v_plane].data)
    {
        return Status::InvalidArgument;
    }
    if ((frame.planes[0].linesize < frame.width) ||
        (frame.planes[u_plane].linesize < (frame.width + 1) / 2) ||
        (frame.planes[v_plane].linesize < (frame.width + 1) / 2))
    {
        return Status::InvalidArgument;
    }

    std::uint32_t const chroma_width = (frame.width + 1) / 2;
    for (std::uint32_t row = 0; row < frame.height; ++row)
    {
        std::uint8_t const* y_row =
            frame.planes[0].data + static_cast<std::size_t>(row) * frame.planes[0].linesize;
        std::uint8_t const* u_row = frame.planes[u_plane].data + static_cast<std::size_t>(row / 2) *
                                                                     frame.planes[u_plane].linesize;
        std::uint8_t const* v_row = frame.planes[v_plane].data + static_cast<std::size_t>(row / 2) *
                                                                     frame.planes[v_plane].linesize;
        auto* dst_row = dst + static_cast<std::size_t>(row) * stride;

        pack_row_420(
            frame.width, [&](std::uint32_t x) { return expand_8_to_10(y_row[x]); },
            [&](std::uint32_t x)
            {
                std::uint32_t const c = std::min(x / 2, chroma_width - 1);
                return expand_8_to_10(u_row[c]);
            },
            [&](std::uint32_t x)
            {
                std::uint32_t const c = std::min(x / 2, chroma_width - 1);
                return expand_8_to_10(v_row[c]);
            },
            dst_row);
    }
    return Status::Ok;
}

struct RgbToYuv
{
    std::uint16_t y[6]{};
    std::uint16_t cb[6]{};
    std::uint16_t cr[6]{};

    void add(std::size_t index, std::uint8_t b, std::uint8_t g, std::uint8_t r) noexcept
    {
        std::int32_t const bi = b;
        std::int32_t const gi = g;
        std::int32_t const ri = r;

        // BT.709 limited range, 8 bit -> 10 bit.
        std::int32_t const y8 = 16 + ((47 * ri + 157 * gi + 16 * bi + 128) >> 8);
        std::int32_t const cb8 = 128 + ((112 * bi - 26 * ri - 86 * gi + 128) >> 8);
        std::int32_t const cr8 = 128 + ((112 * ri - 102 * gi - 10 * bi + 128) >> 8);

        y[index] = static_cast<std::uint16_t>(clamp_u16(y8, 16, 235) << 2);
        cb[index] = static_cast<std::uint16_t>(clamp_u16(cb8, 16, 240) << 2);
        cr[index] = static_cast<std::uint16_t>(clamp_u16(cr8, 16, 240) << 2);
    }
};

Status convert_bgra(FrameView const& frame, std::uint8_t* dst, std::uint32_t stride)
{
    if (!frame.planes[0].data)
    {
        return Status::InvalidArgument;
    }
    if (frame.planes[0].linesize < frame.width * 4)
    {
        return Status::InvalidArgument;
    }

    for (std::uint32_t row = 0; row < frame.height; ++row)
    {
        std::uint8_t const* src_row =
            frame.planes[0].data + static_cast<std::size_t>(row) * frame.planes[0].linesize;
        auto* dst_row = dst + static_cast<std::size_t>(row) * stride;

        for (std::uint32_t x = 0; x < frame.width; x += 6)
        {
            RgbToYuv rgb;
            for (std::uint32_t i = 0; i < 6; ++i)
            {
                std::uint32_t const px = (x + i < frame.width) ? (x + i) : (frame.width - 1);
                std::uint8_t const* pixel = src_row + static_cast<std::size_t>(px) * 4;
                rgb.add(i, pixel[0], pixel[1], pixel[2]);
            }

            std::uint16_t cb[3]{};
            std::uint16_t cr[3]{};
            for (std::size_t i = 0; i < 3; ++i)
            {
                cb[i] = static_cast<std::uint16_t>((rgb.cb[i * 2] + rgb.cb[i * 2 + 1] + 1) / 2);
                cr[i] = static_cast<std::uint16_t>((rgb.cr[i * 2] + rgb.cr[i * 2 + 1] + 1) / 2);
            }
            pack_group(rgb.y, cb, cr, dst_row);
            dst_row += 16;
        }
    }
    return Status::Ok;
}

Status copy_v210(FrameView const& frame, std::uint8_t* dst, std::uint32_t stride)
{
    if (!frame.planes[0].data)
    {
        return Status::InvalidArgument;
    }
    if (frame.planes[0].linesize < stride)
    {
        return Status::InvalidArgument;
    }
    for (std::uint32_t row = 0; row < frame.height; ++row)
    {
        std::memcpy(dst + static_cast<std::size_t>(row) * stride,
                    frame.planes[0].data + static_cast<std::size_t>(row) * frame.planes[0].linesize,
                    stride);
    }
    return Status::Ok;
}
} // namespace

std::string_view to_string(PixelFormat format) noexcept
{
    switch (format)
    {
    case PixelFormat::V210:
        return "v210";
    case PixelFormat::NV12:
        return "nv12";
    case PixelFormat::P010:
        return "p010";
    case PixelFormat::I420:
        return "i420";
    case PixelFormat::YV12:
        return "yv12";
    case PixelFormat::I010:
        return "i010";
    case PixelFormat::BGRA:
        return "bgra";
    }
    return "unknown";
}

std::optional<PixelFormat> pixel_format_from_name(std::string_view name) noexcept
{
    if (name == "v210")
        return PixelFormat::V210;
    if (name == "nv12")
        return PixelFormat::NV12;
    if (name == "p010")
        return PixelFormat::P010;
    if (name == "i420")
        return PixelFormat::I420;
    if (name == "yv12")
        return PixelFormat::YV12;
    if (name == "i010")
        return PixelFormat::I010;
    if (name == "bgra")
        return PixelFormat::BGRA;
    return std::nullopt;
}

void V210Buffer::reset(std::uint32_t new_width, std::uint32_t new_height)
{
    width = new_width;
    height = new_height;
    stride = v210_line_stride(new_width);
    bytes.assign(v210_buffer_size(stride, new_height), 0);
}

bool V210Buffer::matches(std::uint32_t new_width, std::uint32_t new_height) const noexcept
{
    return (width == new_width) && (height == new_height) && !bytes.empty();
}

Status convert_to_v210(FrameView const& frame, std::uint8_t* dst, std::size_t dst_size,
                       std::uint32_t dst_stride)
{
    if (!dst || (frame.width == 0) || (frame.height == 0))
    {
        return Status::InvalidArgument;
    }

    std::uint32_t const minimum_stride = v210_line_stride(frame.width);
    if (dst_stride < minimum_stride)
    {
        return Status::InvalidArgument;
    }
    if (dst_size < v210_buffer_size(dst_stride, frame.height))
    {
        return Status::InvalidArgument;
    }

    switch (frame.format)
    {
    case PixelFormat::V210:
        return copy_v210(frame, dst, dst_stride);
    case PixelFormat::NV12:
        return convert_nv12(frame, dst, dst_stride);
    case PixelFormat::P010:
        return convert_p010(frame, dst, dst_stride);
    case PixelFormat::I420:
        return convert_planar420(frame, dst, dst_stride, 1, 2);
    case PixelFormat::YV12:
        return convert_planar420(frame, dst, dst_stride, 2, 1);
    case PixelFormat::I010:
        return convert_i010(frame, dst, dst_stride);
    case PixelFormat::BGRA:
        return convert_bgra(frame, dst, dst_stride);
    }
    return Status::Unsupported;
}

void fill_v210_black(std::uint8_t* dst, std::uint32_t width, std::uint32_t height,
                     std::uint32_t stride)
{
    if (!dst || (width == 0) || (height == 0))
    {
        return;
    }

    std::uint16_t const y[6] = {black_y_10, black_y_10, black_y_10,
                                black_y_10, black_y_10, black_y_10};
    std::uint16_t const cb[3] = {black_c_10, black_c_10, black_c_10};
    std::uint16_t const cr[3] = {black_c_10, black_c_10, black_c_10};

    std::uint32_t const active = ((width + 5) / 6) * 16;
    for (std::uint32_t row = 0; row < height; ++row)
    {
        std::uint8_t* out = dst + static_cast<std::size_t>(row) * stride;
        for (std::uint32_t offset = 0; offset < active; offset += 16)
        {
            pack_group(y, cb, cr, out + offset);
        }
    }
}
} // namespace obsmxl

// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

#include <obsmxl/flow-definition.hpp>
#include <obsmxl/video-format.hpp>

#include "test-helpers.hpp"

using namespace obsmxl;

namespace
{
std::uint32_t read_word(std::uint8_t const* data, std::size_t word)
{
    std::uint32_t value = 0;
    std::memcpy(&value, data + word * 4, sizeof(value));
    return value;
}

std::array<std::uint32_t, 4> expected_group(std::array<std::uint16_t, 6> const& y,
                                            std::array<std::uint16_t, 3> const& cb,
                                            std::array<std::uint16_t, 3> const& cr)
{
    return {
        static_cast<std::uint32_t>(cb[0]) | (static_cast<std::uint32_t>(y[0]) << 10U) |
            (static_cast<std::uint32_t>(cr[0]) << 20U),
        static_cast<std::uint32_t>(y[1]) | (static_cast<std::uint32_t>(cb[1]) << 10U) |
            (static_cast<std::uint32_t>(y[2]) << 20U),
        static_cast<std::uint32_t>(cr[1]) | (static_cast<std::uint32_t>(y[3]) << 10U) |
            (static_cast<std::uint32_t>(cb[2]) << 20U),
        static_cast<std::uint32_t>(y[4]) | (static_cast<std::uint32_t>(cr[2]) << 10U) |
            (static_cast<std::uint32_t>(y[5]) << 20U),
    };
}
} // namespace

TEST_CASE("NV12 converts to V210 with 10 bit expansion")
{
    auto const data = test::make_nv12(6, 2);
    auto const frame = test::nv12_view(data, 6, 2);

    std::uint32_t const stride = v210_line_stride(6);
    std::vector<std::uint8_t> output(v210_buffer_size(stride, 2), 0);

    REQUIRE(convert_to_v210(frame, output.data(), output.size(), stride) == Status::Ok);

    auto const expected =
        expected_group({64, 128, 192, 256, 320, 384}, {512, 512, 512}, {512, 512, 512});
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CHECK(read_word(output.data(), i) == expected[i]);
    }
}

TEST_CASE("P010 converts to the same 10 bit samples")
{
    constexpr std::uint32_t width = 6;
    constexpr std::uint32_t height = 2;

    std::vector<std::uint16_t> luma(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            luma[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint16_t>((64 + x * 64) << 6);
        }
    }
    std::vector<std::uint16_t> chroma(static_cast<std::size_t>(width) * ((height + 1) / 2));
    for (auto& value : chroma)
    {
        value = static_cast<std::uint16_t>(512 << 6);
    }

    FrameView frame;
    frame.format = PixelFormat::P010;
    frame.width = width;
    frame.height = height;
    frame.planes[0] = {reinterpret_cast<std::uint8_t const*>(luma.data()), width * 2};
    frame.planes[1] = {reinterpret_cast<std::uint8_t const*>(chroma.data()), width * 2};

    std::uint32_t const stride = v210_line_stride(width);
    std::vector<std::uint8_t> output(v210_buffer_size(stride, height), 0);
    REQUIRE(convert_to_v210(frame, output.data(), output.size(), stride) == Status::Ok);

    auto const expected =
        expected_group({64, 128, 192, 256, 320, 384}, {512, 512, 512}, {512, 512, 512});
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CHECK(read_word(output.data(), i) == expected[i]);
    }
}

TEST_CASE("I010 converts like P010 with separate chroma planes")
{
    constexpr std::uint32_t width = 6;
    constexpr std::uint32_t height = 2;
    constexpr std::uint32_t chroma_height = 1;
    constexpr std::uint32_t chroma_width = width / 2;

    std::vector<std::uint16_t> luma(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            luma[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint16_t>((64 + x * 64) << 6);
        }
    }
    std::vector<std::uint16_t> u(static_cast<std::size_t>(chroma_width) * chroma_height,
                                 static_cast<std::uint16_t>(512 << 6));
    std::vector<std::uint16_t> v(static_cast<std::size_t>(chroma_width) * chroma_height,
                                 static_cast<std::uint16_t>(512 << 6));

    FrameView frame;
    frame.format = PixelFormat::I010;
    frame.width = width;
    frame.height = height;
    frame.planes[0] = {reinterpret_cast<std::uint8_t const*>(luma.data()), width * 2};
    frame.planes[1] = {reinterpret_cast<std::uint8_t const*>(u.data()), chroma_width * 2};
    frame.planes[2] = {reinterpret_cast<std::uint8_t const*>(v.data()), chroma_width * 2};

    auto const stride = v210_line_stride(width);
    std::vector<std::uint8_t> output(v210_buffer_size(stride, height), 0);
    REQUIRE(convert_to_v210(frame, output.data(), output.size(), stride) == Status::Ok);

    auto const expected =
        expected_group({64, 128, 192, 256, 320, 384}, {512, 512, 512}, {512, 512, 512});
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CHECK(read_word(output.data(), i) == expected[i]);
    }
}

TEST_CASE("I420 and YV12 use the correct chroma planes")
{
    constexpr std::uint32_t width = 6;
    constexpr std::uint32_t height = 2;
    constexpr std::uint32_t chroma_height = 1;

    std::vector<std::uint8_t> i420;
    i420.resize(width * height + 2 * (width / 2) * chroma_height);
    std::memset(i420.data(), 64, width * height);
    std::memset(i420.data() + width * height, 100, (width / 2) * chroma_height); // U
    std::memset(i420.data() + width * height + (width / 2) * chroma_height, 200,
                (width / 2) * chroma_height); // V

    FrameView frame;
    frame.format = PixelFormat::I420;
    frame.width = width;
    frame.height = height;
    frame.planes[0] = {i420.data(), width};
    frame.planes[1] = {i420.data() + width * height, width / 2};
    frame.planes[2] = {i420.data() + width * height + (width / 2) * chroma_height, width / 2};

    auto const stride = v210_line_stride(width);
    std::vector<std::uint8_t> output(v210_buffer_size(stride, height), 0);
    REQUIRE(convert_to_v210(frame, output.data(), output.size(), stride) == Status::Ok);
    CHECK((read_word(output.data(), 0) & 0x3FFU) == 400);          // U 100 << 2
    CHECK(((read_word(output.data(), 0) >> 20U) & 0x3FFU) == 800); // V 200 << 2

    // YV12 swaps the chroma plane order.
    FrameView yv12 = frame;
    yv12.format = PixelFormat::YV12;
    yv12.planes[1] = frame.planes[2];
    yv12.planes[2] = frame.planes[1];
    std::vector<std::uint8_t> output_yv12(v210_buffer_size(stride, height), 0);
    REQUIRE(convert_to_v210(yv12, output_yv12.data(), output_yv12.size(), stride) == Status::Ok);
    CHECK((read_word(output_yv12.data(), 0) & 0x3FFU) == 400);
    CHECK(((read_word(output_yv12.data(), 0) >> 20U) & 0x3FFU) == 800);
}

TEST_CASE("BGRA white becomes limited range white with neutral chroma")
{
    constexpr std::uint32_t width = 6;
    constexpr std::uint32_t height = 2;
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4, 255);

    FrameView frame;
    frame.format = PixelFormat::BGRA;
    frame.width = width;
    frame.height = height;
    frame.planes[0] = {bgra.data(), width * 4};

    auto const stride = v210_line_stride(width);
    std::vector<std::uint8_t> output(v210_buffer_size(stride, height), 0);
    REQUIRE(convert_to_v210(frame, output.data(), output.size(), stride) == Status::Ok);

    auto const word = read_word(output.data(), 0);
    CHECK(((word >> 10U) & 0x3FFU) == 940); // 235 << 2
    CHECK((word & 0x3FFU) == 512);
    CHECK(((word >> 20U) & 0x3FFU) == 512);
}

TEST_CASE("Partial V210 groups replicate the edge pixels")
{
    constexpr std::uint32_t width = 8;
    auto const data = test::make_nv12(width, 2);
    auto const frame = test::nv12_view(data, width, 2);

    auto const stride = v210_line_stride(width);
    std::vector<std::uint8_t> output(v210_buffer_size(stride, 2), 0xAA);
    REQUIRE(convert_to_v210(frame, output.data(), output.size(), stride) == Status::Ok);

    // Second group of row 0 only contains two active pixels; the remaining
    // four pixels replicate pixel 7 (Y = 16 + 6*16 = 112 -> 448 for pixel 6).
    auto const word = read_word(output.data(), 4);
    CHECK(((word >> 10U) & 0x3FFU) == 448);

    // Bytes between the active groups and the 128 byte stride stay untouched.
    for (std::size_t i = 32; i < 128; ++i)
    {
        CHECK(output[i] == 0xAA);
    }
}

TEST_CASE("V210 to V210 copy preserves the payload")
{
    constexpr std::uint32_t width = 8;
    auto const stride = v210_line_stride(width);
    std::vector<std::uint8_t> source(v210_buffer_size(stride, 2));
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        source[i] = static_cast<std::uint8_t>(i * 7);
    }

    FrameView frame;
    frame.format = PixelFormat::V210;
    frame.width = width;
    frame.height = 2;
    frame.planes[0] = {source.data(), stride};

    std::vector<std::uint8_t> output(source.size(), 0);
    REQUIRE(convert_to_v210(frame, output.data(), output.size(), stride) == Status::Ok);
    CHECK(output == source);
}

TEST_CASE("Blank V210 frames use studio black")
{
    constexpr std::uint32_t width = 6;
    constexpr std::uint32_t height = 2;
    auto const stride = v210_line_stride(width);
    std::vector<std::uint8_t> output(v210_buffer_size(stride, height), 0);

    fill_v210_black(output.data(), width, height, stride);

    auto const expected =
        expected_group({64, 64, 64, 64, 64, 64}, {512, 512, 512}, {512, 512, 512});
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CHECK(read_word(output.data(), i) == expected[i]);
    }
}

TEST_CASE("Invalid conversions are rejected")
{
    FrameView frame;
    frame.format = PixelFormat::NV12;
    frame.width = 6;
    frame.height = 2;

    std::vector<std::uint8_t> output(1024);
    CHECK(convert_to_v210(frame, output.data(), output.size(), 128) == Status::InvalidArgument);
    CHECK(convert_to_v210(frame, nullptr, output.size(), 128) == Status::InvalidArgument);
}

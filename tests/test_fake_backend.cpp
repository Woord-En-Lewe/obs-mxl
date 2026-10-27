// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include <obsmxl/fake-backend.hpp>
#include <obsmxl/flow-definition.hpp>

#include "test-helpers.hpp"

using namespace obsmxl;

TEST_CASE("Fake backend enforces single writer grain ordering")
{
    auto domain = make_fake_domain("test://grain-ordering");
    auto writer_backend = create_fake_backend(domain);
    REQUIRE(writer_backend != nullptr);

    auto const spec = test::make_video_spec();
    std::unique_ptr<VideoWriter> writer;
    VideoStreamInfo info;
    bool created = false;
    REQUIRE(writer_backend->create_video_writer(make_video_flow_json(spec), "{}", writer, info,
                                                &created) == Status::Ok);
    CHECK(created);
    CHECK(info.width == 6);
    CHECK(info.height == 2);
    CHECK(info.stride == v210_line_stride(6));
    CHECK(info.grain_count >= 2);

    std::unique_ptr<VideoWriter> second_writer;
    bool second_created = true;
    REQUIRE(writer_backend->create_video_writer(make_video_flow_json(spec), "{}", second_writer,
                                                info, &second_created) == Status::Ok);
    CHECK_FALSE(second_created);

    REQUIRE(test::write_video_grain(*writer, 100, 0x42) == Status::Ok);

    std::unique_ptr<VideoReader> reader;
    REQUIRE(writer_backend->open_video_reader(spec.id, reader) == Status::Ok);

    GrainInfo grain;
    std::uint8_t const* payload = nullptr;
    REQUIRE(reader->get_grain(100, 0xFFFF, 0, grain, payload) == Status::Ok);
    CHECK(grain.index == 100);
    CHECK(grain.valid_slices == grain.total_slices);
    CHECK(payload[0] == 0x42);

    // A writer may not rewrite an already committed grain.
    GrainInfo ignored;
    std::uint8_t* ignored_payload = nullptr;
    CHECK(writer->open_grain(100, ignored, ignored_payload) == Status::InvalidArgument);

    // Rewinding the reader behind the ring buffer reports TooLate.
    CHECK(reader->get_grain(1, 0xFFFF, 0, grain, payload) == Status::TooLate);
}

TEST_CASE("Fake backend invalidates skipped grains")
{
    auto domain = make_fake_domain("test://invalid-grains");
    domain->set_history_duration_ms(50);

    auto writer_backend = create_fake_backend(domain);
    auto const spec = test::make_video_spec();
    std::unique_ptr<VideoWriter> writer;
    VideoStreamInfo info;
    REQUIRE(writer_backend->create_video_writer(make_video_flow_json(spec), "{}", writer, info,
                                                nullptr) == Status::Ok);

    REQUIRE(test::write_video_grain(*writer, 10, 0x11) == Status::Ok);
    REQUIRE(test::write_video_grain(*writer, 12, 0x22) == Status::Ok);

    std::unique_ptr<VideoReader> reader;
    REQUIRE(writer_backend->open_video_reader(spec.id, reader) == Status::Ok);

    GrainInfo grain;
    std::uint8_t const* payload = nullptr;
    REQUIRE(reader->get_grain(11, 0, 0, grain, payload) == Status::Ok);
    CHECK((grain.flags & 1U) != 0);
}

TEST_CASE("Fake backend continuous flow wraps around the ring")
{
    auto domain = make_fake_domain("test://audio-wrap");
    domain->set_history_duration_ms(100);

    auto backend = create_fake_backend(domain);
    auto const spec = test::make_audio_spec(2);

    std::unique_ptr<AudioWriter> writer;
    AudioStreamInfo info;
    REQUIRE(backend->create_audio_writer(make_audio_flow_json(spec), "{}", writer, info, nullptr) ==
            Status::Ok);
    CHECK(info.channels == 2);
    CHECK(info.buffer_length >= 2);
    CHECK(info.max_batch > 0);

    auto const buffer_length = info.buffer_length;
    std::uint64_t const start = buffer_length - 100;

    REQUIRE(test::write_audio_samples(*writer, start + 200, 200, 1.0F) == Status::Ok);

    std::unique_ptr<AudioReader> reader;
    REQUIRE(backend->open_audio_reader(spec.id, reader) == Status::Ok);

    ConstSampleSpan span;
    REQUIRE(reader->get_samples(start + 200, 200, 0, span) == Status::Ok);
    REQUIRE(span.count == 2);

    std::size_t copied = 0;
    for (auto const& fragment : span.fragments)
    {
        if (fragment.size == 0)
        {
            continue;
        }
        auto const* samples = reinterpret_cast<float const*>(fragment.pointer);
        auto const count = fragment.size / sizeof(float);
        for (std::size_t i = 0; i < count; ++i)
        {
            CHECK(samples[i] == 1.0F);
        }
        copied += count;
    }
    CHECK(copied == 200);

    // Channel 1 is stored at the stride offset.
    auto const* channel_one =
        reinterpret_cast<float const*>(span.fragments[0].pointer + span.stride);
    CHECK(channel_one[0] == 101.0F);

    // The second channel is contiguous in the fake implementation.
    CHECK(reader->get_samples(start + 200, buffer_length, 0, span) == Status::InvalidArgument);
}

TEST_CASE("Fake audio writer rejects non contiguous overwrites")
{
    auto domain = make_fake_domain("test://audio-order");
    auto backend = create_fake_backend(domain);
    auto const spec = test::make_audio_spec();

    std::unique_ptr<AudioWriter> writer;
    AudioStreamInfo info;
    REQUIRE(backend->create_audio_writer(make_audio_flow_json(spec), "{}", writer, info, nullptr) ==
            Status::Ok);

    REQUIRE(test::write_audio_samples(*writer, 1000, 100, 1.0F) == Status::Ok);
    REQUIRE(test::write_audio_samples(*writer, 1100, 100, 2.0F) == Status::Ok);

    MutableSampleSpan span;
    CHECK(writer->open_samples(1200, 100, span) == Status::Ok);
    CHECK(writer->commit_samples() == Status::Ok);

    CHECK(writer->open_samples(1200, 10, span) == Status::InvalidArgument);
}

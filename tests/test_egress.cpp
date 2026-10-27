// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

#include <obsmxl/egress.hpp>
#include <obsmxl/fake-backend.hpp>
#include <obsmxl/flow-definition.hpp>

#include "test-helpers.hpp"

using namespace obsmxl;

namespace
{
struct Fixture
{
    std::shared_ptr<FakeDomain> domain = make_fake_domain("test://egress");
    std::unique_ptr<Backend> backend = create_fake_backend(domain);
    test::ManualClock clock;
    EgressPipeline::Config config;

    Fixture()
    {
        config.domain = "test://egress";
        config.label = "Program";
        config.group_hint = "OBS:Program";
        config.video_flow_id = test::program_video_uuid();
        config.audio_flow_id = test::program_audio_uuid();
        config.width = 6;
        config.height = 2;
        config.frame_rate = Rational{60, 1};
        config.sample_rate = 48000;
        config.channels = 2;
        config.include_audio = true;
        config.audio_max_batch = 480;
    }

    std::uint32_t read_word(std::vector<std::uint8_t> const& data, std::size_t word) const
    {
        std::uint32_t value = 0;
        std::memcpy(&value, data.data() + word * 4, sizeof(value));
        return value;
    }
};
} // namespace

TEST_CASE("Egress rejects invalid configuration")
{
    Fixture fixture;
    fixture.config.video_flow_id = "invalid";

    EgressPipeline pipeline{*fixture.backend, fixture.config, fixture.clock};
    std::string error;
    CHECK_FALSE(pipeline.start(&error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Egress writes a V210 grain on the TAI grid")
{
    Fixture fixture;
    EgressPipeline pipeline{*fixture.backend, fixture.config, fixture.clock};

    std::string error;
    REQUIRE(pipeline.start(&error));
    CHECK(pipeline.started());

    std::unique_ptr<VideoReader> reader;
    REQUIRE(fixture.backend->open_video_reader(fixture.config.video_flow_id, reader) == Status::Ok);

    auto const data = test::make_nv12(6, 2);
    auto frame = test::nv12_view(data, 6, 2, static_cast<std::uint64_t>(fixture.clock.host()));

    pipeline.push_video(frame);

    auto const index = timestamp_to_index(fixture.config.frame_rate, fixture.clock.tai());

    GrainInfo grain;
    std::uint8_t const* payload = nullptr;
    REQUIRE(reader->get_grain(index, 0xFFFF, 0, grain, payload) == Status::Ok);
    CHECK(grain.index == index);
    CHECK(grain.valid_slices == grain.total_slices);

    std::vector<std::uint8_t> copied(payload, payload + grain.grain_size);
    CHECK((fixture.read_word(copied, 0) & 0x3FFU) == 512);
    CHECK(((fixture.read_word(copied, 0) >> 10U) & 0x3FFU) == 64);
    CHECK(((fixture.read_word(copied, 3) >> 20U) & 0x3FFU) == 384);

    auto stats = pipeline.stats();
    CHECK(stats.video_frames == 1);
    CHECK(stats.video_errors == 0);
}

TEST_CASE("Egress drops frames that would rewind the flow head")
{
    Fixture fixture;
    EgressPipeline pipeline{*fixture.backend, fixture.config, fixture.clock};
    REQUIRE(pipeline.start());

    auto const data = test::make_nv12(6, 2);
    auto const old_timestamp = static_cast<std::uint64_t>(fixture.clock.host());

    fixture.clock.advance(5'000'000'000LL);

    auto frame = test::nv12_view(data, 6, 2, old_timestamp);
    pipeline.push_video(frame);

    auto const stats = pipeline.stats();
    CHECK(stats.video_frames == 0);
    CHECK(stats.video_dropped_late == 1);
}

TEST_CASE("Egress clamps timestamps ahead of the facility clock")
{
    Fixture fixture;
    EgressPipeline pipeline{*fixture.backend, fixture.config, fixture.clock};
    REQUIRE(pipeline.start());

    auto const data = test::make_nv12(6, 2);
    auto const future = static_cast<std::uint64_t>(fixture.clock.host() + 500'000'000LL);

    auto frame = test::nv12_view(data, 6, 2, future);
    pipeline.push_video(frame);

    auto const stats = pipeline.stats();
    CHECK(stats.video_frames == 1);
    CHECK(stats.video_dropped_clamped == 1);
}

TEST_CASE("Egress writes planar float32 audio")
{
    Fixture fixture;
    EgressPipeline pipeline{*fixture.backend, fixture.config, fixture.clock};
    REQUIRE(pipeline.start());

    std::unique_ptr<AudioReader> reader;
    REQUIRE(fixture.backend->open_audio_reader(fixture.config.audio_flow_id, reader) == Status::Ok);

    constexpr std::uint32_t frames = 480;
    std::array<float, frames> left{};
    std::array<float, frames> right{};
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        left[i] = static_cast<float>(i) / 1000.0F;
        right[i] = -static_cast<float>(i) / 1000.0F;
    }

    PlanarAudioView audio;
    audio.channel_count = 2;
    audio.frames = frames;
    audio.sample_rate = 48000;
    audio.host_timestamp_ns = static_cast<std::uint64_t>(fixture.clock.host());
    audio.channels[0] = left.data();
    audio.channels[1] = right.data();

    pipeline.push_audio(audio);

    auto const start = timestamp_to_index(Rational{48000, 1}, fixture.clock.tai());

    ConstSampleSpan span;
    REQUIRE(reader->get_samples(start + frames, frames, 0, span) == Status::Ok);
    REQUIRE(span.count == 2);

    auto const* channel0 = reinterpret_cast<float const*>(span.fragments[0].pointer);
    auto const* channel1 = reinterpret_cast<float const*>(span.fragments[0].pointer + span.stride);
    CHECK(channel0[0] == left[0]);
    CHECK(channel0[479] == left[479]);
    CHECK(channel1[0] == right[0]);
    CHECK(channel1[479] == right[479]);

    auto const stats = pipeline.stats();
    CHECK(stats.audio_chunks == 1);
    CHECK(stats.audio_frames == frames);
}

TEST_CASE("Egress fills short audio gaps with silence")
{
    Fixture fixture;
    EgressPipeline pipeline{*fixture.backend, fixture.config, fixture.clock};
    REQUIRE(pipeline.start());

    std::array<float, 480> samples{};
    for (auto& sample : samples)
    {
        sample = 0.5F;
    }

    PlanarAudioView audio;
    audio.channel_count = 2;
    audio.frames = 480;
    audio.sample_rate = 48000;
    audio.host_timestamp_ns = static_cast<std::uint64_t>(fixture.clock.host());
    audio.channels[0] = samples.data();
    audio.channels[1] = samples.data();
    pipeline.push_audio(audio);

    // Advance past the end of the first chunk with a two sample hole.
    fixture.clock.advance((480 + 2) * 1'000'000'000LL / 48000);
    audio.host_timestamp_ns = static_cast<std::uint64_t>(fixture.clock.host());
    pipeline.push_audio(audio);

    std::unique_ptr<AudioReader> reader;
    REQUIRE(fixture.backend->open_audio_reader(fixture.config.audio_flow_id, reader) == Status::Ok);

    auto const start = timestamp_to_index(Rational{48000, 1}, fixture.clock.tai());
    ConstSampleSpan span;
    REQUIRE(reader->get_samples(start + 480, 482, 0, span) == Status::Ok);
    auto const* channel0 = reinterpret_cast<float const*>(span.fragments[0].pointer);

    // The two missing samples are silence, the rest is the second chunk.
    CHECK(channel0[0] == 0.0F);
    CHECK(channel0[1] == 0.0F);
    CHECK(channel0[2] == 0.5F);
    CHECK(channel0[481] == 0.5F);
}

TEST_CASE("Egress refuses to reuse a flow with different geometry")
{
    Fixture fixture;

    auto const spec = test::make_video_spec(8, 2);
    std::unique_ptr<VideoWriter> writer;
    VideoStreamInfo info;
    REQUIRE(fixture.backend->create_video_writer(make_video_flow_json(spec), "{}", writer, info,
                                                 nullptr) == Status::Ok);

    EgressPipeline pipeline{*fixture.backend, fixture.config, fixture.clock};
    std::string error;
    CHECK_FALSE(pipeline.start(&error));
    CHECK(error.find("different geometry") != std::string::npos);
}

// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>

#include <obsmxl/fake-backend.hpp>
#include <obsmxl/flow-definition.hpp>
#include <obsmxl/ingress.hpp>

#include "test-helpers.hpp"

using namespace obsmxl;

namespace
{
struct Fixture
{
    std::shared_ptr<FakeDomain> domain = make_fake_domain("test://ingress");
    std::unique_ptr<Backend> backend = create_fake_backend(domain);
    test::ManualClock clock;

    VideoFlowSpec video_spec = test::make_video_spec();
    AudioFlowSpec audio_spec = test::make_audio_spec();

    std::unique_ptr<VideoWriter> video_writer;
    std::unique_ptr<AudioWriter> audio_writer;

    Fixture()
    {
        VideoStreamInfo info;
        REQUIRE(backend->create_video_writer(make_video_flow_json(video_spec), "{}", video_writer,
                                             info, nullptr) == Status::Ok);
        AudioStreamInfo audio_info;
        REQUIRE(backend->create_audio_writer(make_audio_flow_json(audio_spec), "{}", audio_writer,
                                             audio_info, nullptr) == Status::Ok);
    }

    IngressPipeline::Config config(std::uint32_t delay_ms = 50)
    {
        IngressPipeline::Config value;
        value.domain = "test://ingress";
        value.video_flow_id = video_spec.id;
        value.audio_flow_id = audio_spec.id;
        value.playout_delay_ms = static_cast<int>(delay_ms);
        value.enable_audio = true;
        return value;
    }
};
} // namespace

TEST_CASE("Ingress returns V210 grains with host timestamps")
{
    Fixture fixture;
    auto config = fixture.config();
    IngressPipeline pipeline{*fixture.backend, config, fixture.clock};

    auto const delay_ns = static_cast<Ns>(config.playout_delay_ms) * 1'000'000LL;
    auto const target = fixture.clock.tai() - delay_ns;
    auto const index = timestamp_to_index(fixture.video_spec.grain_rate, target);

    REQUIRE(test::write_video_grain(*fixture.video_writer, index, 0x5A) == Status::Ok);

    auto const result = pipeline.poll();
    REQUIRE(result.video.has_value());
    CHECK_FALSE(result.video->blank);
    CHECK(result.video->format == PixelFormat::V210);
    CHECK(result.video->width == 6);
    CHECK(result.video->height == 2);
    CHECK(result.video->stride == v210_line_stride(6));
    CHECK(result.video->data[0] == 0x5A);
    CHECK(result.video->host_timestamp_ns ==
          static_cast<std::uint64_t>(fixture.clock.host() - delay_ns));

    CHECK(pipeline.stats().video_frames == 1);
    CHECK(result.next_poll_host_ns > static_cast<std::uint64_t>(fixture.clock.host()));
}

TEST_CASE("Ingress emits black frames after a stall")
{
    Fixture fixture;
    auto config = fixture.config(0);
    config.stall_frames_before_blank = 3;
    IngressPipeline pipeline{*fixture.backend, config, fixture.clock};

    // No grain is ever written. Advance one frame per poll so the target index
    // keeps moving and the reader timeouts accumulate.
    for (int i = 0; i < 2; ++i)
    {
        auto const result = pipeline.poll();
        CHECK_FALSE(result.video.has_value());
        fixture.clock.advance(index_duration_ns(fixture.video_spec.grain_rate));
    }

    auto const result = pipeline.poll();
    REQUIRE(result.video.has_value());
    CHECK(result.video->blank);

    std::uint32_t first_word = 0;
    std::memcpy(&first_word, result.video->data, sizeof(first_word));
    CHECK((first_word & 0x3FFU) == 512);
    CHECK(((first_word >> 10U) & 0x3FFU) == 64);
    CHECK(((first_word >> 20U) & 0x3FFU) == 512);
    CHECK(pipeline.stats().blank_frames == 1);
}

TEST_CASE("Ingress resynchronises after a flow overrun")
{
    Fixture fixture;
    auto config = fixture.config(0);
    IngressPipeline pipeline{*fixture.backend, config, fixture.clock};

    auto const first_index = timestamp_to_index(fixture.video_spec.grain_rate, fixture.clock.tai());
    REQUIRE(test::write_video_grain(*fixture.video_writer, first_index, 0x10) == Status::Ok);

    // Jump the writer far ahead of the clock, so the index the reader asks for
    // has already been overwritten in the ring buffer.
    REQUIRE(test::write_video_grain(*fixture.video_writer, first_index + 1000, 0x20) == Status::Ok);

    auto const result = pipeline.poll();
    CHECK_FALSE(result.video.has_value());
    CHECK(pipeline.stats().dropped_late == 1);
    CHECK(pipeline.stats().resyncs == 1);

    // Move the clock to the new head and write the next grain there.
    fixture.clock.advance(1001 * index_duration_ns(fixture.video_spec.grain_rate));
    REQUIRE(test::write_video_grain(*fixture.video_writer, first_index + 1001, 0x30) == Status::Ok);

    auto const next = pipeline.poll();
    REQUIRE(next.video.has_value());
    CHECK(next.video->data[0] == 0x30);
}

TEST_CASE("Ingress delivers audio chunks on the sample grid")
{
    Fixture fixture;
    auto config = fixture.config(0);
    IngressPipeline pipeline{*fixture.backend, config, fixture.clock};

    auto const target_end = timestamp_to_index(Rational{48000, 1}, fixture.clock.tai());
    constexpr std::uint32_t frames = 480;

    REQUIRE(test::write_audio_samples(*fixture.audio_writer, target_end + frames, frames, 3.0F) ==
            Status::Ok);

    // The first poll only anchors the audio timeline.
    auto result = pipeline.poll();
    CHECK_FALSE(result.audio.has_value());

    fixture.clock.advance(static_cast<Ns>(frames) * 1'000'000'000LL / 48000);

    // Write no video at all: the reader may block for up to 25ms first.
    result = pipeline.poll();
    REQUIRE(result.audio.has_value());
    CHECK(result.audio->frames == frames);
    CHECK(result.audio->channel_count == 2);
    CHECK(result.audio->sample_rate == 48000);
    REQUIRE(result.audio->channels[0] != nullptr);
    CHECK(result.audio->channels[0][0] == 3.0F);
    CHECK(result.audio->channels[1][0] == 103.0F);
    CHECK(result.audio->host_timestamp_ns ==
          static_cast<std::uint64_t>(fixture.clock.host() -
                                     static_cast<Ns>(frames) * 1'000'000'000LL / 48000));

    auto const stats = pipeline.stats();
    CHECK(stats.audio_chunks == 1);
    CHECK(stats.audio_frames == frames);
}

TEST_CASE("Ingress keeps polling when the flow does not exist")
{
    Fixture fixture;
    auto config = fixture.config();
    config.video_flow_id = test::preview_video_uuid();
    config.audio_flow_id.clear();

    IngressPipeline pipeline{*fixture.backend, config, fixture.clock};
    auto const result = pipeline.poll();

    CHECK_FALSE(result.video.has_value());
    CHECK_FALSE(result.audio.has_value());
    CHECK(result.next_poll_host_ns > 0);
}

TEST_CASE("Ingress stop is prompt and idempotent")
{
    Fixture fixture;
    auto config = fixture.config();
    IngressPipeline pipeline{*fixture.backend, config, fixture.clock};

    pipeline.stop();
    pipeline.stop();

    auto const result = pipeline.poll();
    CHECK_FALSE(result.video.has_value());
    CHECK(result.next_poll_host_ns == 0);
}

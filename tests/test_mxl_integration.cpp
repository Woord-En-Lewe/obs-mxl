// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include <unistd.h>

#include <obsmxl/backend.hpp>
#include <obsmxl/egress.hpp>
#include <obsmxl/flow-definition.hpp>
#include <obsmxl/ingress.hpp>
#include <obsmxl/uuid.hpp>

#include "test-helpers.hpp"

using namespace obsmxl;

namespace
{
class TempDomain
{
public:
    TempDomain()
        : path_(std::filesystem::temp_directory_path() /
                ("obs-mxl-mxl-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++)))
    {
        std::filesystem::create_directories(path_);
    }

    ~TempDomain()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::string str() const { return path_.string(); }
    std::filesystem::path const& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline int counter_{0};
};

std::unique_ptr<Backend> wait_for_backend(TempDomain const& domain)
{
    std::string error;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        auto backend = create_mxl_backend(domain.str(), &error);
        if (backend)
        {
            return backend;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return nullptr;
}

template<typename Fn> Status retry(Fn&& fn)
{
    Status status = Status::NotFound;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        status = fn();
        if (status == Status::Ok)
        {
            return status;
        }
        if ((status != Status::NotFound) && (status != Status::FlowInvalid))
        {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return status;
}
} // namespace

TEST_CASE("Real MXL backend writes a V210 grain readable by another instance")
{
    TempDomain domain;

    std::string error;
    auto writer_backend = create_mxl_backend(domain.str(), &error);
    REQUIRE(writer_backend != nullptr);

    auto reader_backend = wait_for_backend(domain);
    REQUIRE(reader_backend != nullptr);

    test::ManualClock clock;

    EgressPipeline::Config config;
    config.domain = domain.str();
    config.label = "Integration Program";
    config.group_hint = "OBS:Integration";
    config.video_flow_id = test::program_video_uuid();
    config.audio_flow_id = test::program_audio_uuid();
    config.width = 64;
    config.height = 2;
    config.frame_rate = Rational{60, 1};
    config.sample_rate = 48000;
    config.channels = 2;
    config.include_audio = true;
    config.audio_max_batch = 480;

    EgressPipeline egress{*writer_backend, config, clock};
    REQUIRE(egress.start(&error));

    auto const nv12 = test::make_nv12(64, 2);
    auto const frame = test::nv12_view(nv12, 64, 2, static_cast<std::uint64_t>(clock.host()));
    egress.push_video(frame);
    REQUIRE(egress.stats().video_frames == 1);

    auto const video_index = timestamp_to_index(config.frame_rate, clock.tai());

    std::unique_ptr<VideoReader> reader;
    REQUIRE(
        retry([&] { return reader_backend->open_video_reader(config.video_flow_id, reader); }) ==
        Status::Ok);

    GrainInfo grain;
    std::uint8_t const* payload = nullptr;
    REQUIRE(retry(
                [&]
                {
                    return reader->get_grain(video_index, 0xFFFF, 1'000'000'000ULL, grain, payload);
                }) == Status::Ok);
    REQUIRE(payload != nullptr);
    CHECK(grain.index == video_index);
    CHECK(grain.valid_slices == grain.total_slices);

    std::uint32_t first_word = 0;
    std::memcpy(&first_word, payload, sizeof(first_word));
    CHECK((first_word & 0x3FFU) == 512);
    CHECK(((first_word >> 10U) & 0x3FFU) == 64);

    // Audio: planar float32 written through MXL and read back by index.
    std::array<float, 480> left{};
    std::array<float, 480> right{};
    for (std::uint32_t i = 0; i < 480; ++i)
    {
        left[i] = 0.25F;
        right[i] = -0.25F;
    }

    PlanarAudioView audio;
    audio.channel_count = 2;
    audio.frames = 480;
    audio.sample_rate = 48000;
    audio.host_timestamp_ns = static_cast<std::uint64_t>(clock.host());
    audio.channels[0] = left.data();
    audio.channels[1] = right.data();
    egress.push_audio(audio);
    REQUIRE(egress.stats().audio_chunks == 1);

    std::unique_ptr<AudioReader> audio_reader;
    REQUIRE(
        retry([&]
              { return reader_backend->open_audio_reader(config.audio_flow_id, audio_reader); }) ==
        Status::Ok);

    auto const audio_start = timestamp_to_index(Rational{48000, 1}, clock.tai());
    ConstSampleSpan span;
    REQUIRE(retry(
                [&]
                {
                    return audio_reader->get_samples(audio_start + 480, 480, 1'000'000'000ULL,
                                                     span);
                }) == Status::Ok);

    auto const* channel0 = reinterpret_cast<float const*>(span.fragments[0].pointer);
    auto const* channel1 = reinterpret_cast<float const*>(span.fragments[0].pointer + span.stride);
    CHECK(channel0[0] == 0.25F);
    CHECK(channel0[479] == 0.25F);
    CHECK(channel1[0] == -0.25F);

    egress.stop();
    CHECK_FALSE(egress.started());
}

TEST_CASE("Real MXL ingress reads a flow written by egress")
{
    TempDomain domain;

    auto writer_backend = create_mxl_backend(domain.str());
    REQUIRE(writer_backend != nullptr);
    auto reader_backend = wait_for_backend(domain);
    REQUIRE(reader_backend != nullptr);

    test::ManualClock clock;

    EgressPipeline::Config config;
    config.domain = domain.str();
    config.label = "Ingress Integration";
    config.group_hint = "OBS:Integration";
    config.video_flow_id = test::program_video_uuid();
    config.audio_flow_id.clear();
    config.width = 64;
    config.height = 2;
    config.frame_rate = Rational{60, 1};
    config.include_audio = false;

    EgressPipeline egress{*writer_backend, config, clock};
    REQUIRE(egress.start());
    REQUIRE(egress.stats().video_frames == 0);

    auto const nv12 = test::make_nv12(64, 2);
    auto const host_timestamp = static_cast<std::uint64_t>(clock.host());
    auto frame = test::nv12_view(nv12, 64, 2, host_timestamp);
    egress.push_video(frame);
    REQUIRE(egress.stats().video_frames == 1);

    IngressPipeline::Config ingress_config;
    ingress_config.domain = domain.str();
    ingress_config.video_flow_id = config.video_flow_id;
    ingress_config.enable_audio = false;
    ingress_config.playout_delay_ms = 0;

    IngressPipeline ingress{*reader_backend, ingress_config, clock};

    IngressPipeline::Result result;
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        result = ingress.poll();
        if (result.video)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    REQUIRE(result.video.has_value());
    CHECK(result.video->width == 64);
    CHECK(result.video->height == 2);
    CHECK(result.video->host_timestamp_ns == host_timestamp);

    std::uint32_t word = 0;
    std::memcpy(&word, result.video->data, sizeof(word));
    CHECK(((word >> 10U) & 0x3FFU) == 64);

    ingress.stop();
    egress.stop();
}

TEST_CASE("MXL backend reports invalid domains")
{
    std::string error;
    auto backend = create_mxl_backend("/dev/null/obs-mxl-invalid", &error);
    CHECK(backend == nullptr);
    CHECK_FALSE(error.empty());
}

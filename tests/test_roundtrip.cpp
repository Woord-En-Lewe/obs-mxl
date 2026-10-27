// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

#include <obsmxl/egress.hpp>
#include <obsmxl/fake-backend.hpp>
#include <obsmxl/ingress.hpp>

#include "test-helpers.hpp"

using namespace obsmxl;

TEST_CASE("A frame written by egress is returned byte identical by ingress")
{
    auto const domain_name = std::string{"test://roundtrip"};
    auto domain = make_fake_domain(domain_name);
    auto writer_backend = create_fake_backend(domain);
    auto reader_backend = create_fake_backend(domain);

    test::ManualClock clock;

    EgressPipeline::Config egress_config;
    egress_config.domain = domain_name;
    egress_config.label = "Program";
    egress_config.group_hint = "OBS:Program";
    egress_config.video_flow_id = test::program_video_uuid();
    egress_config.audio_flow_id = test::program_audio_uuid();
    egress_config.width = 6;
    egress_config.height = 2;
    egress_config.frame_rate = Rational{60, 1};
    egress_config.sample_rate = 48000;
    egress_config.channels = 2;
    egress_config.include_audio = true;

    EgressPipeline egress{*writer_backend, egress_config, clock};
    REQUIRE(egress.start());

    IngressPipeline::Config ingress_config;
    ingress_config.domain = domain_name;
    ingress_config.video_flow_id = egress_config.video_flow_id;
    ingress_config.audio_flow_id = egress_config.audio_flow_id;
    ingress_config.playout_delay_ms = 0;
    ingress_config.enable_audio = true;

    IngressPipeline ingress{*reader_backend, ingress_config, clock};

    auto const nv12 = test::make_nv12(6, 2);
    auto frame = test::nv12_view(nv12, 6, 2, static_cast<std::uint64_t>(clock.host()));

    // Anchor the ingress audio timeline.
    REQUIRE_FALSE(ingress.poll().audio.has_value());

    egress.push_video(frame);
    REQUIRE(egress.stats().video_frames == 1);

    auto const result = ingress.poll();
    REQUIRE(result.video.has_value());
    CHECK_FALSE(result.video->blank);
    CHECK(result.video->width == 6);
    CHECK(result.video->height == 2);
    CHECK(result.video->stride == v210_line_stride(6));
    CHECK(result.video->host_timestamp_ns == static_cast<std::uint64_t>(clock.host()));

    std::vector<std::uint8_t> written(v210_payload_size(6, 2));
    std::memcpy(written.data(), result.video->data, written.size());

    // The first group encodes Y=64,128,192,256,320,384 with neutral chroma.
    std::uint32_t first_word = 0;
    std::memcpy(&first_word, written.data(), sizeof(first_word));
    CHECK((first_word & 0x3FFU) == 512);
    CHECK(((first_word >> 10U) & 0x3FFU) == 64);
    CHECK(((first_word >> 20U) & 0x3FFU) == 512);

    std::uint32_t last_word = 0;
    std::memcpy(&last_word, written.data() + 12, sizeof(last_word));
    CHECK(((last_word >> 10U) & 0x3FFU) == 512);
    CHECK(((last_word >> 20U) & 0x3FFU) == 384);
}

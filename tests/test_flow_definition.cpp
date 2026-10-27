// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <obsmxl/flow-definition.hpp>
#include <obsmxl/json.hpp>
#include <obsmxl/uuid.hpp>

#include "test-helpers.hpp"

using namespace obsmxl;

TEST_CASE("V210 strides match the MXL and OBS alignment rules")
{
    CHECK(v210_line_stride(6) == 128);
    CHECK(v210_line_stride(48) == 128);
    CHECK(v210_line_stride(1920) == 5120);
    CHECK(v210_line_stride(1000) == 2688);
    CHECK(v210_payload_size(1920, 1080) == static_cast<std::size_t>(5120) * 1080);
}

TEST_CASE("Video flow definitions contain the required NMOS fields")
{
    auto const spec = test::make_video_spec(1920, 1080);
    auto const json = make_video_flow_json(spec);

    auto const document = JsonValue::parse(json);
    REQUIRE(document.has_value());
    CHECK(document->string_or("id") == spec.id);
    CHECK(document->string_or("format") == "urn:x-nmos:format:video");
    CHECK(document->string_or("media_type") == "video/v210");
    CHECK(document->string_or("label") == spec.label);
    CHECK(document->string_or("interlace_mode") == "progressive");
    CHECK(document->string_or("colorspace") == "BT709");
    CHECK(document->number_or("frame_width") == 1920.0);
    CHECK(document->number_or("frame_height") == 1080.0);

    auto const* tags = document->find("tags");
    REQUIRE(tags != nullptr);
    auto const* group_hint = tags->find("urn:x-nmos:tag:grouphint/v1.0");
    REQUIRE(group_hint != nullptr);
    REQUIRE(group_hint->as_array() != nullptr);
    CHECK((*group_hint->as_array())[0].as_string() == "OBS:Program");

    auto const* rate = document->find("grain_rate");
    REQUIRE(rate != nullptr);
    CHECK(rate->number_or("numerator") == 60.0);
    CHECK(rate->number_or("denominator") == 1.0);

    auto const* components = document->find("components");
    REQUIRE(components != nullptr);
    REQUIRE(components->as_array() != nullptr);
    REQUIRE(components->as_array()->size() == 3);
    CHECK((*components->as_array())[0].string_or("name") == "Y");
    CHECK((*components->as_array())[1].string_or("name") == "Cb");
    CHECK((*components->as_array())[2].string_or("name") == "Cr");
    CHECK((*components->as_array())[1].number_or("width") == 960.0);
}

TEST_CASE("Audio flow definitions derive source and device ids")
{
    auto const spec = test::make_audio_spec(8);
    auto const json = make_audio_flow_json(spec);

    auto const document = JsonValue::parse(json);
    REQUIRE(document.has_value());
    CHECK(document->string_or("format") == "urn:x-nmos:format:audio");
    CHECK(document->string_or("media_type") == "audio/float32");
    CHECK(document->number_or("channel_count") == 8.0);
    CHECK(document->number_or("bit_depth") == 32.0);
    CHECK(is_valid_uuid(document->string_or("source_id")));
    CHECK(is_valid_uuid(document->string_or("device_id")));

    auto const* rate = document->find("sample_rate");
    REQUIRE(rate != nullptr);
    CHECK(rate->number_or("numerator") == 48000.0);
}

TEST_CASE("Flow definition validation rejects broken specs")
{
    std::string error;

    auto spec = test::make_video_spec();
    spec.id = "not-a-uuid";
    CHECK_FALSE(validate_video_flow_spec(spec, &error));
    CHECK_FALSE(error.empty());

    spec = test::make_video_spec();
    spec.width = 3;
    CHECK_FALSE(validate_video_flow_spec(spec, &error));

    spec = test::make_video_spec();
    spec.group_hint = "invalid";
    CHECK_FALSE(validate_video_flow_spec(spec, &error));

    auto audio = test::make_audio_spec(0);
    CHECK_FALSE(validate_audio_flow_spec(audio, &error));

    audio = test::make_audio_spec();
    CHECK(validate_audio_flow_spec(audio, &error));
}

TEST_CASE("Writer options serialise the commit batch hint")
{
    auto const options = make_writer_options_json(480);
    auto const document = JsonValue::parse(options);
    REQUIRE(document.has_value());
    CHECK(document->number_or("maxCommitBatchSizeHint") == 480.0);

    CHECK(JsonValue::parse(make_writer_options_json(0))->is_object());
}

// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <obsmxl/settings.hpp>
#include <obsmxl/uuid.hpp>

using namespace obsmxl;

TEST_CASE("Default settings are deterministic and valid")
{
    auto const settings = make_default_settings();

    CHECK(is_valid_uuid(settings.program.video_flow_uuid));
    CHECK(is_valid_uuid(settings.program.audio_flow_uuid));
    CHECK(is_valid_uuid(settings.preview.video_flow_uuid));
    CHECK(settings.program.video_flow_uuid != settings.preview.video_flow_uuid);
    CHECK(settings.program.video_flow_uuid != settings.program.audio_flow_uuid);

    std::string error;
    CHECK(validate_settings(settings, &error, false) == Status::Ok);
    CHECK(error.empty());

    auto const other = make_default_settings();
    CHECK(other.program.video_flow_uuid == settings.program.video_flow_uuid);
}

TEST_CASE("Settings round trip through the key/value map")
{
    auto settings = make_default_settings();
    settings.domain_path = "/tmp/mxl-test";
    settings.history_ms = 500;
    settings.program.enabled = true;
    settings.program.flow_name = "Program A";
    settings.preview.enabled = true;
    settings.preview.include_audio = true;

    auto const map = settings_to_map(settings);
    auto const restored = settings_from_map(map);

    CHECK(restored.domain_path == settings.domain_path);
    CHECK(restored.history_ms == settings.history_ms);
    CHECK(restored.program.enabled);
    CHECK(restored.program.flow_name == "Program A");
    CHECK(restored.program.video_flow_uuid == settings.program.video_flow_uuid);
    CHECK(restored.preview.include_audio);

    std::string error;
    CHECK(validate_settings(restored, &error, true) == Status::Ok);
}

TEST_CASE("Validation rejects duplicate and invalid flow ids")
{
    auto settings = make_default_settings();
    settings.program.enabled = true;
    settings.preview.enabled = true;

    settings.preview.video_flow_uuid = settings.program.video_flow_uuid;

    std::string error;
    CHECK(validate_settings(settings, &error, true) == Status::InvalidArgument);
    CHECK_FALSE(error.empty());

    settings = make_default_settings();
    settings.program.enabled = true;
    settings.program.video_flow_uuid = "nope";
    CHECK(validate_settings(settings, &error, true) == Status::InvalidArgument);

    settings = make_default_settings();
    settings.program.enabled = true;
    settings.domain_path.clear();
    CHECK(validate_settings(settings, &error, true) == Status::InvalidArgument);

    settings = make_default_settings();
    settings.history_ms = 10;
    CHECK(validate_settings(settings, &error, false) == Status::InvalidArgument);
}

TEST_CASE("Disabled endpoints are skipped when validating the active profile")
{
    auto settings = make_default_settings();
    settings.preview.enabled = false;
    settings.preview.video_flow_uuid = "broken";

    std::string error;
    CHECK(validate_settings(settings, &error, true) == Status::Ok);

    settings.program.enabled = false;
    CHECK(validate_settings(settings, &error, false) == Status::InvalidArgument);
}

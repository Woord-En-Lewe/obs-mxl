// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include <unistd.h>

#include <obsmxl/discovery.hpp>
#include <obsmxl/flow-definition.hpp>

#include "test-helpers.hpp"

using namespace obsmxl;

namespace
{
class TempDirectory
{
public:
    TempDirectory()
    {
        path_ = std::filesystem::temp_directory_path() /
                ("obs-mxl-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::filesystem::path const& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline int counter_{0};
};

void write_flow(std::filesystem::path const& domain, std::string const& id,
                std::string const& definition)
{
    auto const directory = domain / (id + std::string{flow_directory_suffix});
    std::filesystem::create_directories(directory);
    std::ofstream stream{directory / "flow_def.json", std::ios::binary};
    stream << definition;
}
} // namespace

TEST_CASE("Flow discovery reads video and audio descriptors")
{
    TempDirectory temp;
    auto video = test::make_video_spec(1920, 1080);
    auto audio = test::make_audio_spec(8);

    write_flow(temp.path(), video.id, make_video_flow_json(video));
    write_flow(temp.path(), audio.id, make_audio_flow_json(audio));

    auto const all = discover_flows(temp.path());
    REQUIRE(all.size() == 2);

    auto const videos = discover_video_flows(temp.path());
    REQUIRE(videos.size() == 1);
    CHECK(videos[0].id == video.id);
    CHECK(videos[0].width == 1920);
    CHECK(videos[0].height == 1080);
    CHECK(videos[0].audio == false);
    CHECK(videos[0].rate == Rational{60, 1});

    auto const audios = discover_audio_flows(temp.path());
    REQUIRE(audios.size() == 1);
    CHECK(audios[0].id == audio.id);
    CHECK(audios[0].audio);
    CHECK(audios[0].channel_count == 8);
    CHECK(audios[0].rate == Rational{48000, 1});
    CHECK(audios[0].display_name().find("audio/float32") != std::string::npos);
}

TEST_CASE("Flow discovery ignores invalid and unrelated directories")
{
    TempDirectory temp;

    std::filesystem::create_directories(temp.path() / "not-a-flow");
    std::filesystem::create_directories(temp.path() /
                                        ("broken" + std::string{flow_directory_suffix}));
    {
        std::ofstream stream{temp.path() / ("broken" + std::string{flow_directory_suffix}) /
                             "flow_def.json"};
        stream << "{ not valid json";
    }

    CHECK(discover_flows(temp.path()).empty());
    CHECK(discover_flows(temp.path() / "missing").empty());

    auto video = test::make_video_spec();
    write_flow(temp.path(), video.id, make_video_flow_json(video));
    CHECK(discover_flows(temp.path()).size() == 1);
}

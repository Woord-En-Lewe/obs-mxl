// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <obsmxl/time.hpp>

#include "test-helpers.hpp"

using namespace obsmxl;

TEST_CASE("timestamp and index conversions follow the MXL rounding rules")
{
    Rational const video{30000, 1001};

    CHECK(timestamp_to_index(video, 0) == 0);
    CHECK(timestamp_to_index(video, 1'000'000'000LL) == 30);
    CHECK(index_to_timestamp(video, 30) == 1'001'000'000LL);

    for (std::uint64_t index = 1; index < 500; ++index)
    {
        auto const timestamp = index_to_timestamp(video, index);
        CHECK(timestamp_to_index(video, timestamp) == index);
    }

    Rational const audio{48000, 1};
    CHECK(timestamp_to_index(audio, 1'000'000'000LL) == 48000);
    CHECK(index_to_timestamp(audio, 48000) == 1'000'000'000LL);
    CHECK(index_duration_ns(Rational{60, 1}) == 16'666'666LL);

    CHECK(timestamp_to_index(Rational{0, 1}, 1'000'000'000LL) == undefined_index);
    CHECK(index_duration_ns(Rational{0, 1}) == 0);
}

TEST_CASE("ClockOffset maps between host and TAI time")
{
    test::ManualClock clock{1'000, 5'000};
    ClockOffset offset;

    CHECK_FALSE(offset.calibrated());
    REQUIRE(offset.calibrate(clock, 0));
    CHECK(offset.calibrated());
    CHECK(offset.offset_ns() == -4'000);
    CHECK(offset.to_host(5'000) == 1'000);
    CHECK(offset.to_tai(1'000) == 5'000);

    clock.advance(1'000);
    CHECK(offset.to_host(6'000) == 2'000);

    offset.reset();
    CHECK_FALSE(offset.calibrated());
}

TEST_CASE("Rational comparison is exact")
{
    CHECK(Rational{50, 1} == Rational{100, 2});
    CHECK(Rational{60000, 1001} != Rational{30000, 1001});
    CHECK(Rational{50, 1} != Rational{60, 1});
}

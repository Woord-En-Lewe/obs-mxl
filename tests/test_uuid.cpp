// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>

#include <obsmxl/uuid.hpp>

using namespace obsmxl;

TEST_CASE("UUID parsing and formatting round trip")
{
    auto const uuid = parse_uuid("3fa85f64-5717-4562-b3fc-2c963f66afa6");
    REQUIRE(uuid.has_value());
    CHECK(format_uuid(*uuid) == "3fa85f64-5717-4562-b3fc-2c963f66afa6");
    CHECK(is_valid_uuid("3FA85F64-5717-4562-B3FC-2C963F66AFA6"));

    CHECK_FALSE(parse_uuid("3fa85f64-5717-4562-b3fc-2c963f66afa").has_value());
    CHECK_FALSE(parse_uuid("3fa85f64x5717-4562-b3fc-2c963f66afa6").has_value());
    CHECK_FALSE(parse_uuid("").has_value());
}

TEST_CASE("UUID v4 sets the version and variant bits")
{
    for (int i = 0; i < 16; ++i)
    {
        auto const uuid = generate_uuid_v4();
        CHECK((uuid[6] & 0xF0U) == 0x40U);
        CHECK((uuid[8] & 0xC0U) == 0x80U);
    }

    CHECK(generate_uuid_v4() != generate_uuid_v4());
}

TEST_CASE("SHA-1 matches the reference vectors")
{
    auto const digest = sha1("abc");
    char buffer[41]{};
    for (std::size_t i = 0; i < digest.size(); ++i)
    {
        std::snprintf(buffer + i * 2, 3, "%02x", digest[i]);
    }
    CHECK(std::string{buffer} == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST_CASE("UUID v5 matches the RFC 4122 test vector")
{
    auto const dns = parse_uuid(uuid_namespace_dns);
    REQUIRE(dns.has_value());

    auto const uuid = uuid_v5(*dns, "python.org");
    CHECK(format_uuid(uuid) == "886313e1-3b8a-5372-9b90-0c9aee199e5d");

    CHECK(derive_uuid(uuid_namespace_dns, "python.org").has_value());
    CHECK_FALSE(derive_uuid("not-a-uuid", "python.org").has_value());
}

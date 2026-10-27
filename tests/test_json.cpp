// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <obsmxl/json.hpp>

using namespace obsmxl;

TEST_CASE("JSON parser reads nested documents")
{
    auto const document = JsonValue::parse(R"({
        "id": "abc",
        "frame_width": 1920,
        "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["OBS:Program"]},
        "sample_rate": {"numerator": 48000, "denominator": 1},
        "components": [{"name": "Y"}, {"name": "Cb"}],
        "enabled": true
    })");

    REQUIRE(document.has_value());
    CHECK(document->string_or("id") == "abc");
    CHECK(document->number_or("frame_width") == 1920.0);
    CHECK(document->bool_or("enabled"));

    auto const* tags = document->find("tags");
    REQUIRE(tags != nullptr);
    auto const* tag = tags->find("urn:x-nmos:tag:grouphint/v1.0");
    REQUIRE(tag != nullptr);
    REQUIRE(tag->as_array() != nullptr);
    CHECK((*tag->as_array())[0].as_string() == "OBS:Program");

    auto const* components = document->find("components");
    REQUIRE(components != nullptr);
    REQUIRE(components->as_array() != nullptr);
    CHECK((*components->as_array()).size() == 2);
}

TEST_CASE("JSON parser decodes escapes and unicode")
{
    auto const document = JsonValue::parse(R"({"label": "a\"b\\c\n\u00e9\ud83c\udfac"})");
    REQUIRE(document.has_value());
    CHECK(document->string_or("label") == "a\"b\\c\n\xc3\xa9\xf0\x9f\x8e\xac");
}

TEST_CASE("JSON parser rejects malformed documents")
{
    std::string error;
    CHECK_FALSE(JsonValue::parse("{", &error).has_value());
    CHECK_FALSE(error.empty());

    CHECK_FALSE(JsonValue::parse(R"({"a": 1} trailing)", &error).has_value());
    CHECK_FALSE(JsonValue::parse(R"({"a": })", &error).has_value());
    CHECK_FALSE(JsonValue::parse("", &error).has_value());
}

TEST_CASE("JsonWriter emits parseable documents")
{
    JsonWriter writer;
    writer.begin_object();
    writer.key("name").value("OBS \"Program\"");
    writer.key("width").value(1920);
    writer.key("enabled").value(true);
    writer.key("tags").begin_array().value("a").value("b").end_array();
    writer.key("nested").begin_object();
    writer.key("rate").begin_object();
    writer.key("numerator").value(60000);
    writer.key("denominator").value(1001);
    writer.end_object();
    writer.end_object();
    writer.end_object();

    auto const document = JsonValue::parse(writer.str());
    REQUIRE(document.has_value());
    CHECK(document->string_or("name") == "OBS \"Program\"");
    CHECK(document->number_or("width") == 1920.0);
    CHECK(document->bool_or("enabled"));

    auto const* tags = document->find("tags");
    REQUIRE(tags != nullptr);
    REQUIRE(tags->as_array() != nullptr);
    CHECK(tags->as_array()->size() == 2);

    auto const* nested = document->find("nested");
    REQUIRE(nested != nullptr);
    CHECK(nested->number_or("nope", -1.0) == -1.0);
    auto const* rate = nested->find("rate");
    REQUIRE(rate != nullptr);
    CHECK(rate->number_or("numerator") == 60000.0);
}

TEST_CASE("JsonWriter escapes control characters")
{
    JsonWriter writer;
    writer.begin_object();
    writer.key("value").value(std::string_view{"a\x01"
                                               "b"});
    writer.end_object();

    CHECK(writer.str().find("\\u0001") != std::string::npos);
    CHECK(JsonValue::parse(writer.str()).has_value());
}

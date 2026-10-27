// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace obsmxl
{
/**
 * Small dependency-free JSON value used to read NMOS flow definitions.
 * The plugin only needs to read discovery metadata and emit flow
 * definitions, so a full featured JSON library would be overkill.
 */
class JsonValue
{
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    JsonValue() noexcept;
    explicit JsonValue(bool value);
    explicit JsonValue(double value);
    explicit JsonValue(std::string value);
    explicit JsonValue(Array value);
    explicit JsonValue(Object value);

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;

    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;
    [[nodiscard]] double as_number(double fallback = 0.0) const noexcept;
    [[nodiscard]] std::string_view as_string(std::string_view fallback = {}) const noexcept;
    [[nodiscard]] Array const* as_array() const noexcept;
    [[nodiscard]] Object const* as_object() const noexcept;

    /** Looks up a direct member of an object; nullptr otherwise. */
    [[nodiscard]] JsonValue const* find(std::string_view key) const noexcept;

    /** Convenience typed lookups for nested objects. */
    [[nodiscard]] std::string string_or(std::string_view key, std::string fallback = {}) const;
    [[nodiscard]] double number_or(std::string_view key, double fallback = 0.0) const;
    [[nodiscard]] bool bool_or(std::string_view key, bool fallback = false) const;

    /** Parses a JSON document. Returns nullopt and fills error on failure. */
    static std::optional<JsonValue> parse(std::string_view text, std::string* error = nullptr);

private:
    Storage storage_;
};

/**
 * Streaming JSON writer with deterministic formatting (2 space indent).
 * Builds the NMOS flow definitions written into the MXL domain.
 */
class JsonWriter
{
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view name);
    JsonWriter& value(std::string_view value);
    JsonWriter& value(char const* value);
    JsonWriter& value(bool value);
    JsonWriter& value(std::int64_t value);
    JsonWriter& value(std::uint64_t value);
    JsonWriter& value(int value);
    JsonWriter& value(unsigned int value);
    JsonWriter& value(double value);

    [[nodiscard]] std::string const& str() const noexcept;

private:
    struct Frame
    {
        enum class Kind
        {
            Object,
            Array
        } kind;
        bool first{true};
    };

    void prepare_value();
    void newline();
    void append_escaped(std::string_view value);

    std::string out_;
    std::vector<Frame> stack_;
    bool pending_key_{false};
};
} // namespace obsmxl

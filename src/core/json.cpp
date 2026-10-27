// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/json.hpp>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace obsmxl
{
JsonValue::JsonValue() noexcept : storage_(nullptr) {}

JsonValue::JsonValue(bool value) : storage_(value) {}

JsonValue::JsonValue(double value) : storage_(value) {}

JsonValue::JsonValue(std::string value) : storage_(std::move(value)) {}

JsonValue::JsonValue(Array value) : storage_(std::move(value)) {}

JsonValue::JsonValue(Object value) : storage_(std::move(value)) {}

bool JsonValue::is_null() const noexcept
{
    return std::holds_alternative<std::nullptr_t>(storage_);
}

bool JsonValue::is_bool() const noexcept
{
    return std::holds_alternative<bool>(storage_);
}

bool JsonValue::is_number() const noexcept
{
    return std::holds_alternative<double>(storage_);
}

bool JsonValue::is_string() const noexcept
{
    return std::holds_alternative<std::string>(storage_);
}

bool JsonValue::is_array() const noexcept
{
    return std::holds_alternative<Array>(storage_);
}

bool JsonValue::is_object() const noexcept
{
    return std::holds_alternative<Object>(storage_);
}

bool JsonValue::as_bool(bool fallback) const noexcept
{
    auto const* value = std::get_if<bool>(&storage_);
    return value ? *value : fallback;
}

double JsonValue::as_number(double fallback) const noexcept
{
    auto const* value = std::get_if<double>(&storage_);
    return value ? *value : fallback;
}

std::string_view JsonValue::as_string(std::string_view fallback) const noexcept
{
    auto const* value = std::get_if<std::string>(&storage_);
    return value ? std::string_view{*value} : fallback;
}

JsonValue::Array const* JsonValue::as_array() const noexcept
{
    return std::get_if<Array>(&storage_);
}

JsonValue::Object const* JsonValue::as_object() const noexcept
{
    return std::get_if<Object>(&storage_);
}

JsonValue const* JsonValue::find(std::string_view key) const noexcept
{
    auto const* object = as_object();
    if (!object)
    {
        return nullptr;
    }
    auto const it = object->find(std::string{key});
    return (it != object->end()) ? &it->second : nullptr;
}

std::string JsonValue::string_or(std::string_view key, std::string fallback) const
{
    auto const* value = find(key);
    if (value && value->is_string())
    {
        return std::string{value->as_string()};
    }
    return fallback;
}

double JsonValue::number_or(std::string_view key, double fallback) const
{
    auto const* value = find(key);
    return (value && value->is_number()) ? value->as_number() : fallback;
}

bool JsonValue::bool_or(std::string_view key, bool fallback) const
{
    auto const* value = find(key);
    return (value && value->is_bool()) ? value->as_bool() : fallback;
}

namespace
{
constexpr int max_depth = 32;

void append_utf8(std::string& out, std::uint32_t codepoint)
{
    if (codepoint <= 0x7FU)
    {
        out.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FFU)
    {
        out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
    else if (codepoint <= 0xFFFFU)
    {
        out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

class Parser
{
public:
    Parser(std::string_view text, std::string* error) : text_(text), error_(error) {}

    std::optional<JsonValue> parse_document()
    {
        auto value = parse_value(0);
        if (!value)
        {
            return std::nullopt;
        }
        skip_whitespace();
        if (position_ != text_.size())
        {
            fail("trailing characters after JSON value");
            return std::nullopt;
        }
        return value;
    }

private:
    void skip_whitespace()
    {
        while (position_ < text_.size())
        {
            char const c = text_[position_];
            if ((c == ' ') || (c == '\t') || (c == '\n') || (c == '\r'))
            {
                ++position_;
            }
            else
            {
                break;
            }
        }
    }

    void fail(std::string message)
    {
        if (error_ && error_->empty())
        {
            *error_ = std::move(message) + " at offset " + std::to_string(position_);
        }
    }

    bool consume(char expected)
    {
        skip_whitespace();
        if ((position_ < text_.size()) && (text_[position_] == expected))
        {
            ++position_;
            return true;
        }
        return false;
    }

    std::optional<JsonValue> parse_value(int depth)
    {
        if (depth > max_depth)
        {
            fail("maximum JSON nesting depth exceeded");
            return std::nullopt;
        }

        skip_whitespace();
        if (position_ >= text_.size())
        {
            fail("unexpected end of input");
            return std::nullopt;
        }

        switch (text_[position_])
        {
        case '{':
            return parse_object(depth);
        case '[':
            return parse_array(depth);
        case '"':
        {
            auto string = parse_string();
            if (!string)
            {
                return std::nullopt;
            }
            return JsonValue{std::move(*string)};
        }
        case 't':
            if (text_.substr(position_, 4) == "true")
            {
                position_ += 4;
                return JsonValue{true};
            }
            break;
        case 'f':
            if (text_.substr(position_, 5) == "false")
            {
                position_ += 5;
                return JsonValue{false};
            }
            break;
        case 'n':
            if (text_.substr(position_, 4) == "null")
            {
                position_ += 4;
                return JsonValue{};
            }
            break;
        default:
            break;
        }

        if ((text_[position_] == '-') || ((text_[position_] >= '0') && (text_[position_] <= '9')))
        {
            return parse_number();
        }

        fail("invalid JSON value");
        return std::nullopt;
    }

    std::optional<JsonValue> parse_object(int depth)
    {
        ++position_; // '{'
        JsonValue::Object object;

        skip_whitespace();
        if (consume('}'))
        {
            return JsonValue{std::move(object)};
        }

        while (true)
        {
            skip_whitespace();
            if ((position_ >= text_.size()) || (text_[position_] != '"'))
            {
                fail("expected object key string");
                return std::nullopt;
            }

            auto key = parse_string();
            if (!key)
            {
                return std::nullopt;
            }

            if (!consume(':'))
            {
                fail("expected ':' after object key");
                return std::nullopt;
            }

            auto value = parse_value(depth + 1);
            if (!value)
            {
                return std::nullopt;
            }
            object.insert_or_assign(std::move(*key), std::move(*value));

            skip_whitespace();
            if (consume('}'))
            {
                break;
            }
            if (!consume(','))
            {
                fail("expected ',' or '}' in object");
                return std::nullopt;
            }
        }

        return JsonValue{std::move(object)};
    }

    std::optional<JsonValue> parse_array(int depth)
    {
        ++position_; // '['
        JsonValue::Array array;

        skip_whitespace();
        if (consume(']'))
        {
            return JsonValue{std::move(array)};
        }

        while (true)
        {
            auto value = parse_value(depth + 1);
            if (!value)
            {
                return std::nullopt;
            }
            array.push_back(std::move(*value));

            skip_whitespace();
            if (consume(']'))
            {
                break;
            }
            if (!consume(','))
            {
                fail("expected ',' or ']' in array");
                return std::nullopt;
            }
        }

        return JsonValue{std::move(array)};
    }

    std::optional<std::string> parse_string()
    {
        if ((position_ >= text_.size()) || (text_[position_] != '"'))
        {
            fail("expected string");
            return std::nullopt;
        }
        ++position_;

        std::string out;
        while (position_ < text_.size())
        {
            char const c = text_[position_++];
            if (c == '"')
            {
                return out;
            }
            if (c != '\\')
            {
                if (static_cast<unsigned char>(c) < 0x20U)
                {
                    fail("unescaped control character in string");
                    return std::nullopt;
                }
                out.push_back(c);
                continue;
            }

            if (position_ >= text_.size())
            {
                fail("unterminated escape sequence");
                return std::nullopt;
            }

            char const escape = text_[position_++];
            switch (escape)
            {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
            {
                if (position_ + 4 > text_.size())
                {
                    fail("truncated unicode escape");
                    return std::nullopt;
                }
                std::uint32_t codepoint = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char const digit = text_[position_++];
                    int value = 0;
                    if ((digit >= '0') && (digit <= '9'))
                    {
                        value = digit - '0';
                    }
                    else if ((digit >= 'a') && (digit <= 'f'))
                    {
                        value = digit - 'a' + 10;
                    }
                    else if ((digit >= 'A') && (digit <= 'F'))
                    {
                        value = digit - 'A' + 10;
                    }
                    else
                    {
                        fail("invalid unicode escape");
                        return std::nullopt;
                    }
                    codepoint = (codepoint << 4U) | static_cast<std::uint32_t>(value);
                }

                if ((codepoint >= 0xD800U) && (codepoint <= 0xDBFFU))
                {
                    if ((position_ + 6 > text_.size()) || (text_[position_] != '\\') ||
                        (text_[position_ + 1] != 'u'))
                    {
                        fail("missing low surrogate");
                        return std::nullopt;
                    }
                    position_ += 2;
                    std::uint32_t low = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        char const digit = text_[position_++];
                        int value = 0;
                        if ((digit >= '0') && (digit <= '9'))
                        {
                            value = digit - '0';
                        }
                        else if ((digit >= 'a') && (digit <= 'f'))
                        {
                            value = digit - 'a' + 10;
                        }
                        else if ((digit >= 'A') && (digit <= 'F'))
                        {
                            value = digit - 'A' + 10;
                        }
                        else
                        {
                            fail("invalid low surrogate");
                            return std::nullopt;
                        }
                        low = (low << 4U) | static_cast<std::uint32_t>(value);
                    }
                    if ((low < 0xDC00U) || (low > 0xDFFFU))
                    {
                        fail("invalid low surrogate");
                        return std::nullopt;
                    }
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
                }

                append_utf8(out, codepoint);
                break;
            }
            default:
                fail("invalid escape sequence");
                return std::nullopt;
            }
        }

        fail("unterminated string");
        return std::nullopt;
    }

    std::optional<JsonValue> parse_number()
    {
        std::size_t const start = position_;
        if (text_[position_] == '-')
        {
            ++position_;
        }
        while ((position_ < text_.size()) && (text_[position_] >= '0') && (text_[position_] <= '9'))
        {
            ++position_;
        }
        if ((position_ < text_.size()) && (text_[position_] == '.'))
        {
            ++position_;
            while ((position_ < text_.size()) && (text_[position_] >= '0') &&
                   (text_[position_] <= '9'))
            {
                ++position_;
            }
        }
        if ((position_ < text_.size()) && ((text_[position_] == 'e') || (text_[position_] == 'E')))
        {
            ++position_;
            if ((position_ < text_.size()) &&
                ((text_[position_] == '+') || (text_[position_] == '-')))
            {
                ++position_;
            }
            while ((position_ < text_.size()) && (text_[position_] >= '0') &&
                   (text_[position_] <= '9'))
            {
                ++position_;
            }
        }

        std::string const token{text_.substr(start, position_ - start)};
        if (token.empty() || (token == "-"))
        {
            fail("invalid number");
            return std::nullopt;
        }

        char* end = nullptr;
        errno = 0;
        double const value = std::strtod(token.c_str(), &end);
        if ((errno != 0) || (end == nullptr) || (*end != '\0'))
        {
            fail("invalid number");
            return std::nullopt;
        }
        return JsonValue{value};
    }

    std::string_view text_;
    std::string* error_;
    std::size_t position_{0};
};
} // namespace

std::optional<JsonValue> JsonValue::parse(std::string_view text, std::string* error)
{
    Parser parser{text, error};
    return parser.parse_document();
}

// ------------------------------------------------------------------
// JsonWriter
// ------------------------------------------------------------------

void JsonWriter::newline()
{
    out_.push_back('\n');
    out_.append(stack_.size() * 2, ' ');
}

void JsonWriter::prepare_value()
{
    if (pending_key_)
    {
        pending_key_ = false;
        return;
    }

    if (stack_.empty())
    {
        return;
    }

    auto& frame = stack_.back();
    if (!frame.first)
    {
        out_.push_back(',');
    }
    frame.first = false;
    newline();
}

JsonWriter& JsonWriter::begin_object()
{
    prepare_value();
    out_.push_back('{');
    stack_.push_back(Frame{Frame::Kind::Object, true});
    return *this;
}

JsonWriter& JsonWriter::end_object()
{
    if (!stack_.empty())
    {
        bool const empty = stack_.back().first;
        stack_.pop_back();
        if (!empty)
        {
            newline();
        }
        out_.push_back('}');
    }
    return *this;
}

JsonWriter& JsonWriter::begin_array()
{
    prepare_value();
    out_.push_back('[');
    stack_.push_back(Frame{Frame::Kind::Array, true});
    return *this;
}

JsonWriter& JsonWriter::end_array()
{
    if (!stack_.empty())
    {
        bool const empty = stack_.back().first;
        stack_.pop_back();
        if (!empty)
        {
            newline();
        }
        out_.push_back(']');
    }
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name)
{
    if (stack_.empty() || (stack_.back().kind != Frame::Kind::Object))
    {
        return *this;
    }

    auto& frame = stack_.back();
    if (!frame.first)
    {
        out_.push_back(',');
    }
    frame.first = false;
    newline();

    out_.push_back('"');
    append_escaped(name);
    out_.append("\": ");
    pending_key_ = true;
    return *this;
}

void JsonWriter::append_escaped(std::string_view value)
{
    for (char const c : value)
    {
        switch (c)
        {
        case '"':
            out_.append("\\\"");
            break;
        case '\\':
            out_.append("\\\\");
            break;
        case '\b':
            out_.append("\\b");
            break;
        case '\f':
            out_.append("\\f");
            break;
        case '\n':
            out_.append("\\n");
            break;
        case '\r':
            out_.append("\\r");
            break;
        case '\t':
            out_.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U)
            {
                char buffer[7]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                              static_cast<unsigned int>(static_cast<unsigned char>(c)));
                out_.append(buffer);
            }
            else
            {
                out_.push_back(c);
            }
            break;
        }
    }
}

JsonWriter& JsonWriter::value(std::string_view value)
{
    prepare_value();
    out_.push_back('"');
    append_escaped(value);
    out_.push_back('"');
    return *this;
}

JsonWriter& JsonWriter::value(char const* value)
{
    return this->value(std::string_view{value});
}

JsonWriter& JsonWriter::value(bool value)
{
    prepare_value();
    out_.append(value ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t value)
{
    prepare_value();
    out_.append(std::to_string(value));
    return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t value)
{
    prepare_value();
    out_.append(std::to_string(value));
    return *this;
}

JsonWriter& JsonWriter::value(int value)
{
    return this->value(static_cast<std::int64_t>(value));
}

JsonWriter& JsonWriter::value(unsigned int value)
{
    return this->value(static_cast<std::uint64_t>(value));
}

JsonWriter& JsonWriter::value(double value)
{
    prepare_value();
    if (!std::isfinite(value))
    {
        out_.append("0");
        return *this;
    }
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    out_.append(buffer);
    return *this;
}

std::string const& JsonWriter::str() const noexcept
{
    return out_;
}
} // namespace obsmxl

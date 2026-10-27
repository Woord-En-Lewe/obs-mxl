// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/uuid.hpp>

#include <algorithm>
#include <cstring>
#include <random>

namespace obsmxl
{
namespace
{
constexpr char hex_digits[] = "0123456789abcdef";

int hex_value(char c) noexcept
{
    if ((c >= '0') && (c <= '9'))
    {
        return c - '0';
    }
    if ((c >= 'a') && (c <= 'f'))
    {
        return c - 'a' + 10;
    }
    if ((c >= 'A') && (c <= 'F'))
    {
        return c - 'A' + 10;
    }
    return -1;
}

constexpr bool is_hyphen_position(std::size_t index) noexcept
{
    return (index == 8) || (index == 13) || (index == 18) || (index == 23);
}

std::uint32_t rotate_left(std::uint32_t value, unsigned int bits) noexcept
{
    return (value << bits) | (value >> (32U - bits));
}

void sha1_block(std::uint32_t state[5], std::uint8_t const block[64])
{
    std::uint32_t words[80]{};
    for (std::size_t i = 0; i < 16; ++i)
    {
        words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]));
    }
    for (std::size_t i = 16; i < 80; ++i)
    {
        words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];

    for (std::size_t i = 0; i < 80; ++i)
    {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (i < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999U;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }

        std::uint32_t const temp = rotate_left(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}
} // namespace

std::optional<Uuid> parse_uuid(std::string_view text) noexcept
{
    if (text.size() != 36)
    {
        return std::nullopt;
    }

    Uuid result{};
    std::size_t byte_index = 0;
    for (std::size_t i = 0; i < text.size();)
    {
        if (is_hyphen_position(i))
        {
            if (text[i] != '-')
            {
                return std::nullopt;
            }
            ++i;
            continue;
        }

        if ((i + 1) >= text.size())
        {
            return std::nullopt;
        }
        int const high = hex_value(text[i]);
        int const low = hex_value(text[i + 1]);
        if ((high < 0) || (low < 0) || (byte_index >= result.size()))
        {
            return std::nullopt;
        }
        result[byte_index++] = static_cast<std::uint8_t>((high << 4) | low);
        i += 2;
    }

    return (byte_index == result.size()) ? std::optional<Uuid>{result} : std::nullopt;
}

std::string format_uuid(Uuid const& uuid)
{
    std::string result;
    result.reserve(36);
    for (std::size_t i = 0; i < uuid.size(); ++i)
    {
        if (is_hyphen_position(result.size()))
        {
            result.push_back('-');
        }
        result.push_back(hex_digits[(uuid[i] >> 4U) & 0x0FU]);
        result.push_back(hex_digits[uuid[i] & 0x0FU]);
    }
    return result;
}

bool is_valid_uuid(std::string_view text) noexcept
{
    return parse_uuid(text).has_value();
}

Uuid generate_uuid_v4()
{
    Uuid result{};
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<int> distribution(0, 255);

    for (auto& byte : result)
    {
        byte = static_cast<std::uint8_t>(distribution(generator));
    }

    // RFC 4122: version 4 and variant 1 (10xx).
    result[6] = static_cast<std::uint8_t>((result[6] & 0x0FU) | 0x40U);
    result[8] = static_cast<std::uint8_t>((result[8] & 0x3FU) | 0x80U);
    return result;
}

std::array<std::uint8_t, 20> sha1(std::string_view data)
{
    std::uint32_t state[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};

    std::size_t const full_blocks = data.size() / 64;
    for (std::size_t block = 0; block < full_blocks; ++block)
    {
        sha1_block(state, reinterpret_cast<std::uint8_t const*>(data.data() + block * 64));
    }

    std::size_t const remainder = data.size() - full_blocks * 64;
    std::uint8_t tail[128]{};
    if (remainder > 0)
    {
        std::memcpy(tail, data.data() + full_blocks * 64, remainder);
    }
    tail[remainder] = 0x80U;

    std::size_t const tail_blocks = (remainder + 1 + 8 <= 64) ? 1 : 2;
    std::uint64_t const bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i)
    {
        tail[tail_blocks * 64 - 1 - i] =
            static_cast<std::uint8_t>((bit_length >> (i * 8U)) & 0xFFU);
    }

    for (std::size_t block = 0; block < tail_blocks; ++block)
    {
        sha1_block(state, tail + block * 64);
    }

    std::array<std::uint8_t, 20> digest{};
    for (std::size_t i = 0; i < 5; ++i)
    {
        digest[i * 4] = static_cast<std::uint8_t>((state[i] >> 24U) & 0xFFU);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 16U) & 0xFFU);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 8U) & 0xFFU);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i] & 0xFFU);
    }
    return digest;
}

Uuid uuid_v5(Uuid const& namespace_uuid, std::string_view name)
{
    std::string input;
    input.reserve(namespace_uuid.size() + name.size());
    input.append(reinterpret_cast<char const*>(namespace_uuid.data()), namespace_uuid.size());
    input.append(name.data(), name.size());

    auto const digest = sha1(input);
    Uuid result{};
    std::copy_n(digest.begin(), result.size(), result.begin());

    // RFC 4122: version 5 and variant 1 (10xx).
    result[6] = static_cast<std::uint8_t>((result[6] & 0x0FU) | 0x50U);
    result[8] = static_cast<std::uint8_t>((result[8] & 0x3FU) | 0x80U);
    return result;
}

std::optional<Uuid> derive_uuid(std::string_view namespace_text, std::string_view name)
{
    auto const base = parse_uuid(namespace_text);
    if (!base)
    {
        return std::nullopt;
    }
    return uuid_v5(*base, name);
}
} // namespace obsmxl

// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace obsmxl
{
using Uuid = std::array<std::uint8_t, 16>;

/** Parses the canonical 8-4-4-4-12 representation. */
std::optional<Uuid> parse_uuid(std::string_view text) noexcept;

std::string format_uuid(Uuid const& uuid);

bool is_valid_uuid(std::string_view text) noexcept;

/** Generates an RFC 4122 version 4 UUID using the OS CSPRNG. */
Uuid generate_uuid_v4();

/**
 * Derives an RFC 4122 version 5 (SHA-1) UUID.
 * namespace_uuid must be a valid UUID, name an arbitrary byte string.
 */
Uuid uuid_v5(Uuid const& namespace_uuid, std::string_view name);

/** Parses a UUID and derives a child; returns nullopt for invalid input. */
std::optional<Uuid> derive_uuid(std::string_view namespace_text, std::string_view name);

// Well-known RFC 4122 namespaces.
inline constexpr std::string_view uuid_namespace_dns = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
inline constexpr std::string_view uuid_namespace_url = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";

/** Exposed for tests; computes SHA-1 over the given bytes. */
std::array<std::uint8_t, 20> sha1(std::string_view data);
} // namespace obsmxl

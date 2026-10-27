// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <string>

#include <obsmxl/status.hpp>

namespace obsmxl
{
/** MXL key for the domain wide history/buffer duration. */
inline constexpr char const* domain_history_option = "urn:x-mxl:option:history_duration/v1.0";

/**
 * Writes <domain>/options.json so that newly created flows use the given
 * history duration. MXL applies this option at domain level.
 *
 * This modifies shared facility state, so the caller must only invoke it
 * when the user explicitly opted in.
 */
Status write_domain_history(std::string const& domain, int history_ms,
                            std::string* error = nullptr);

/** Reads the history duration currently configured in the domain. */
int read_domain_history(std::string const& domain, int fallback_ms = 200);
} // namespace obsmxl

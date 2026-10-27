// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <obsmxl/rational.hpp>

namespace obsmxl
{
/** Metadata read from <domain>/<uuid>.mxl-flow/flow_def.json. */
struct FlowSummary
{
    std::string id;
    std::string label;
    std::string media_type;
    std::string format; // "video" | "audio" | "data" | ""
    bool audio{false};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t channel_count{0};
    Rational rate{0, 1};

    [[nodiscard]] std::string display_name() const;
};

/** Directory suffix used by the MXL SDK for flow directories. */
inline constexpr std::string_view flow_directory_suffix = ".mxl-flow";

/** Reads a single flow descriptor; returns nullopt when unreadable/invalid. */
std::optional<FlowSummary> read_flow_summary(std::filesystem::path const& flow_directory);

/** Scans a domain, sorted by label then UUID. Missing domains yield {}. */
std::vector<FlowSummary> discover_flows(std::filesystem::path const& domain);

/** Returns only video flows. */
std::vector<FlowSummary> discover_video_flows(std::filesystem::path const& domain);

/** Returns only audio flows. */
std::vector<FlowSummary> discover_audio_flows(std::filesystem::path const& domain);
} // namespace obsmxl

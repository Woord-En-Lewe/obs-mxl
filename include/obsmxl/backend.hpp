// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <obsmxl/rational.hpp>
#include <obsmxl/status.hpp>
#include <obsmxl/time.hpp>

namespace obsmxl
{
/** Immutable description of a discrete (video/data) flow. */
struct VideoStreamInfo
{
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};      // bytes per slice/line
    std::uint32_t grain_count{0}; // ring buffer depth
    std::uint16_t total_slices{0};
    Rational grain_rate{0, 1};
};

/** Immutable description of a continuous (audio) flow. */
struct AudioStreamInfo
{
    std::uint32_t sample_rate{0};
    std::uint32_t channels{0};
    std::uint32_t buffer_length{0}; // samples per channel
    std::uint32_t max_batch{0};     // maxCommitBatchSizeHint
};

struct GrainInfo
{
    std::uint64_t index{0};
    std::uint32_t flags{0};
    std::uint32_t grain_size{0};
    std::uint16_t total_slices{0};
    std::uint16_t valid_slices{0};
};

struct MutableSampleSpan
{
    struct Fragment
    {
        std::uint8_t* pointer{nullptr};
        std::size_t size{0};
    };
    Fragment fragments[2]{};
    std::size_t stride{0}; // bytes between the same sample of consecutive channels
    std::size_t count{0};  // channels
};

struct ConstSampleSpan
{
    struct Fragment
    {
        std::uint8_t const* pointer{nullptr};
        std::size_t size{0};
    };
    Fragment fragments[2]{};
    std::size_t stride{0};
    std::size_t count{0};
};

/**
 * Write side of a discrete flow. Exactly one writer may exist per flow;
 * the plugin only writes from a single OBS thread per output.
 */
class VideoWriter
{
public:
    virtual ~VideoWriter() = default;

    virtual Status open_grain(std::uint64_t index, GrainInfo& info, std::uint8_t*& payload) = 0;
    virtual Status commit_grain(GrainInfo const& info) = 0;
    virtual Status cancel_grain() = 0;

    [[nodiscard]] virtual VideoStreamInfo const& info() const noexcept = 0;
};

/** Write side of a continuous (audio) flow. */
class AudioWriter
{
public:
    virtual ~AudioWriter() = default;

    /**
     * Opens count samples ending at end_index (exclusive), i.e. the range
     * [end_index - count, end_index). Samples must be contiguous with the
     * previous commit.
     */
    virtual Status open_samples(std::uint64_t end_index, std::size_t count,
                                MutableSampleSpan& span) = 0;
    virtual Status commit_samples() = 0;
    virtual Status cancel_samples() = 0;

    [[nodiscard]] virtual AudioStreamInfo const& info() const noexcept = 0;
};

struct VideoReader
{
public:
    virtual ~VideoReader() = default;

    /** Waits up to timeout_ns for the grain to have at least min_valid_slices. */
    virtual Status get_grain(std::uint64_t index, std::uint16_t min_valid_slices,
                             std::uint64_t timeout_ns, GrainInfo& info,
                             std::uint8_t const*& payload) = 0;

    [[nodiscard]] virtual Status fetch_info(VideoStreamInfo& info) = 0;
    [[nodiscard]] virtual Status head_index(std::uint64_t& index) = 0;
};

class AudioReader
{
public:
    virtual ~AudioReader() = default;

    /** Returns count samples ending at end_index. */
    virtual Status get_samples(std::uint64_t end_index, std::size_t count, std::uint64_t timeout_ns,
                               ConstSampleSpan& span) = 0;

    [[nodiscard]] virtual Status fetch_info(AudioStreamInfo& info) = 0;
    [[nodiscard]] virtual Status head_index(std::uint64_t& index) = 0;
};

/** A domain (directory) containing MXL flows. */
class Backend
{
public:
    virtual ~Backend() = default;

    virtual Status create_video_writer(std::string const& flow_def, std::string const& options,
                                       std::unique_ptr<VideoWriter>& writer, VideoStreamInfo& info,
                                       bool* created = nullptr) = 0;

    virtual Status create_audio_writer(std::string const& flow_def, std::string const& options,
                                       std::unique_ptr<AudioWriter>& writer, AudioStreamInfo& info,
                                       bool* created = nullptr) = 0;

    virtual Status open_video_reader(std::string const& flow_id,
                                     std::unique_ptr<VideoReader>& reader) = 0;

    virtual Status open_audio_reader(std::string const& flow_id,
                                     std::unique_ptr<AudioReader>& reader) = 0;

    [[nodiscard]] virtual std::string const& last_error() const noexcept = 0;
    [[nodiscard]] virtual std::string const& domain() const noexcept = 0;
};

enum class BackendKind
{
    Mxl,
    Fake,
};

/**
 * Creates a backend for a domain. Returns nullptr and fills error when the
 * domain cannot be opened. The fake backend is intended for tests and for
 * macOS development without the MXL SDK.
 */
std::unique_ptr<Backend> create_backend(BackendKind kind, std::string domain,
                                        std::string* error = nullptr);

/**
 * Creates the libmxl backed implementation. Only available when the plugin
 * is linked with the MXL SDK (obsmxl_backend_mxl).
 */
std::unique_ptr<Backend> create_mxl_backend(std::string domain, std::string* error = nullptr);
} // namespace obsmxl

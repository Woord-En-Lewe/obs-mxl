// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <obsmxl/backend.hpp>
#include <obsmxl/log.hpp>
#include <obsmxl/time.hpp>
#include <obsmxl/video-format.hpp>

namespace obsmxl
{
/**
 * Host agnostic MXL ingress engine.
 *
 * Reads V210 grains from a video flow and float32 samples from an optional
 * audio flow on the TAI timeline, maps them onto the host clock and hands
 * them to the host (OBS) without blocking the host's render thread.
 *
 * poll() must be called from a single dedicated reader thread. It never
 * sleeps for longer than the grain timeout (25ms).
 */
class IngressPipeline
{
public:
    struct Config
    {
        std::string domain;
        std::string video_flow_id;
        std::string audio_flow_id; // empty disables audio
        int playout_delay_ms{50};
        bool enable_audio{true};
        bool allow_blank_on_stall{true};
        std::uint32_t stall_frames_before_blank{3};
        std::uint32_t max_audio_catchup_ms{200};
        std::uint64_t audio_read_timeout_ns{5'000'000};
    };

    struct VideoFrame
    {
        PixelFormat format{PixelFormat::V210};
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::uint32_t stride{0}; // V210 slice/line length
        std::uint8_t const* data{nullptr};
        std::uint64_t host_timestamp_ns{0};
        bool blank{false};
    };

    struct AudioChunk
    {
        std::array<float const*, max_audio_channels> channels{};
        std::uint32_t channel_count{0};
        std::uint32_t frames{0};
        std::uint32_t sample_rate{0};
        std::uint64_t host_timestamp_ns{0};
        std::shared_ptr<void const> keepalive;
    };

    struct Result
    {
        std::optional<VideoFrame> video;
        std::optional<AudioChunk> audio;
        /** Host timestamp at which poll() should be called again (0 = host decides). */
        std::uint64_t next_poll_host_ns{0};
    };

    struct Stats
    {
        std::uint64_t video_frames{0};
        std::uint64_t blank_frames{0};
        std::uint64_t dropped_late{0};
        std::uint64_t timeouts{0};
        std::uint64_t resyncs{0};
        std::uint64_t audio_chunks{0};
        std::uint64_t audio_frames{0};
        std::uint64_t audio_gaps{0};
        std::uint64_t reconnects{0};
    };

    IngressPipeline(Backend& backend, Config config, Clock const& clock, Logger* logger = nullptr);
    ~IngressPipeline();

    IngressPipeline(IngressPipeline const&) = delete;
    IngressPipeline(IngressPipeline&&) = delete;
    IngressPipeline& operator=(IngressPipeline const&) = delete;
    IngressPipeline& operator=(IngressPipeline&&) = delete;

    /** Requests the next poll() to return promptly (thread-safe). */
    void stop() noexcept;

    Result poll();

    [[nodiscard]] Stats stats() const noexcept;
    [[nodiscard]] Config const& config() const noexcept;

private:
    struct AudioStorage;

    bool ensure_readers();
    void close_readers();
    void reset_video_state();
    void reset_audio_state();

    std::optional<VideoFrame> poll_video(std::uint64_t target_index);
    void poll_audio(std::uint64_t current_tai, Result& result, Ns& next_poll_host);
    void emit_blank(std::uint64_t target_index, Result& result);

    Backend& backend_;
    Config config_;
    Clock const& clock_;
    Logger* logger_;

    std::unique_ptr<VideoReader> video_reader_;
    std::unique_ptr<AudioReader> audio_reader_;
    VideoStreamInfo video_info_{};
    AudioStreamInfo audio_info_{};

    ClockOffset offset_;
    std::uint64_t last_video_index_{undefined_index};
    std::uint64_t last_audio_end_index_{undefined_index};
    std::uint32_t consecutive_misses_{0};

    V210Buffer blank_buffer_;
    std::shared_ptr<AudioStorage> audio_storage_;
    std::uint64_t last_audio_host_ns_{0};

    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> stat_video_frames_{0};
    std::atomic<std::uint64_t> stat_blank_frames_{0};
    std::atomic<std::uint64_t> stat_dropped_late_{0};
    std::atomic<std::uint64_t> stat_timeouts_{0};
    std::atomic<std::uint64_t> stat_resyncs_{0};
    std::atomic<std::uint64_t> stat_audio_chunks_{0};
    std::atomic<std::uint64_t> stat_audio_frames_{0};
    std::atomic<std::uint64_t> stat_audio_gaps_{0};
    std::atomic<std::uint64_t> stat_reconnects_{0};

    bool readers_open_{false};
    Ns last_reconnect_host_{0};
    Ns last_audio_open_attempt_{0};
    Ns last_metadata_check_host_{0};
};
} // namespace obsmxl

// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <obsmxl/backend.hpp>
#include <obsmxl/log.hpp>
#include <obsmxl/rational.hpp>
#include <obsmxl/time.hpp>
#include <obsmxl/video-format.hpp>

namespace obsmxl
{
/**
 * Host agnostic MXL egress engine.
 *
 * Creates a V210 video flow and (optionally) a float32 audio flow and
 * writes every OBS frame/sample chunk into them on the TAI grid. Writes
 * never block: late frames are dropped, gaps are invalidated by the SDK,
 * and audio gaps are filled with silence.
 */
class EgressPipeline
{
public:
    struct Config
    {
        std::string domain;
        std::string label{"OBS MXL Output"};
        std::string group_hint{"OBS:Program"};
        std::string video_flow_id;
        std::string audio_flow_id; // empty disables audio
        std::uint32_t width{1920};
        std::uint32_t height{1080};
        Rational frame_rate{60, 1};
        std::uint32_t sample_rate{48000};
        std::uint32_t channels{2};
        bool include_audio{true};
        std::string colorspace{"BT709"};
        /** Maximum samples per audio commit; MXL defaults to 10ms. */
        std::uint32_t audio_max_batch{480};
    };

    struct Stats
    {
        std::uint64_t video_frames{0};
        std::uint64_t video_dropped_late{0};
        std::uint64_t video_dropped_clamped{0};
        std::uint64_t video_errors{0};
        std::uint64_t audio_chunks{0};
        std::uint64_t audio_frames{0};
        std::uint64_t audio_silence_chunks{0};
        std::uint64_t audio_dropped{0};
        std::uint64_t audio_errors{0};
    };

    EgressPipeline(Backend& backend, Config config, Clock const& clock, Logger* logger = nullptr);
    ~EgressPipeline();

    EgressPipeline(EgressPipeline const&) = delete;
    EgressPipeline(EgressPipeline&&) = delete;
    EgressPipeline& operator=(EgressPipeline const&) = delete;
    EgressPipeline& operator=(EgressPipeline&&) = delete;

    /** Creates the flows. Returns false and fills error on failure. */
    bool start(std::string* error = nullptr);
    void stop() noexcept;
    [[nodiscard]] bool started() const noexcept;

    /** Called from the host's raw video callback. Never blocks on readers. */
    void push_video(FrameView const& frame);

    /** Called from the host's raw audio callback. */
    void push_audio(PlanarAudioView const& audio);

    [[nodiscard]] Stats stats() const noexcept;
    [[nodiscard]] VideoStreamInfo const& video_info() const noexcept;
    [[nodiscard]] AudioStreamInfo const& audio_info() const noexcept;
    [[nodiscard]] Config const& config() const noexcept;

private:
    void write_audio_range(std::uint64_t start_index, PlanarAudioView const& audio,
                           std::uint32_t sample_offset, std::uint32_t sample_count);
    void write_audio_silence(std::uint64_t start_index, std::uint32_t sample_count);
    void reset_video_index();
    void reset_audio_index();
    void log_throttled(LogLevel level, std::string const& message);

    bool create_video_writer(std::string* error);
    bool create_audio_writer(std::string* error);

    Backend& backend_;
    Config config_;
    Clock const& clock_;
    Logger* logger_;

    std::unique_ptr<VideoWriter> video_writer_;
    std::unique_ptr<AudioWriter> audio_writer_;
    VideoStreamInfo video_info_{};
    AudioStreamInfo audio_info_{};

    ClockOffset offset_;

    std::uint64_t last_video_index_{undefined_index};
    std::uint64_t last_audio_end_index_{undefined_index};

    std::vector<std::uint8_t> video_buffer_;

    mutable std::mutex video_mutex_;
    mutable std::mutex audio_mutex_;
    mutable std::mutex lifecycle_mutex_;

    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> stat_video_frames_{0};
    std::atomic<std::uint64_t> stat_video_dropped_late_{0};
    std::atomic<std::uint64_t> stat_video_dropped_clamped_{0};
    std::atomic<std::uint64_t> stat_video_errors_{0};
    std::atomic<std::uint64_t> stat_audio_chunks_{0};
    std::atomic<std::uint64_t> stat_audio_frames_{0};
    std::atomic<std::uint64_t> stat_audio_silence_chunks_{0};
    std::atomic<std::uint64_t> stat_audio_dropped_{0};
    std::atomic<std::uint64_t> stat_audio_errors_{0};

    Ns last_log_host_{0};
};
} // namespace obsmxl

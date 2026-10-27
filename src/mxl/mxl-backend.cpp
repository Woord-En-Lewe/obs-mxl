// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/backend.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>

#include <obsmxl/fake-backend.hpp>
#include <obsmxl/flow-definition.hpp>
#include <obsmxl/json.hpp>
#include <obsmxl/video-format.hpp>

namespace obsmxl
{
namespace
{
Status map_status(mxlStatus status) noexcept
{
    switch (status)
    {
    case MXL_STATUS_OK:
        return Status::Ok;
    case MXL_ERR_TIMEOUT:
        return Status::Timeout;
    case MXL_ERR_OUT_OF_RANGE_TOO_EARLY:
        return Status::TooEarly;
    case MXL_ERR_OUT_OF_RANGE_TOO_LATE:
        return Status::TooLate;
    case MXL_ERR_FLOW_NOT_FOUND:
        return Status::NotFound;
    case MXL_ERR_FLOW_INVALID:
        return Status::FlowInvalid;
    case MXL_ERR_CONFLICT:
        return Status::Conflict;
    case MXL_ERR_INVALID_ARG:
        return Status::InvalidArgument;
    case MXL_ERR_UNSUPPORTED_OPERATION:
        return Status::Unsupported;
    default:
        return Status::Error;
    }
}

struct FlowMeta
{
    std::string id;
    std::string media_type;
    bool audio{false};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t channels{0};
    Rational rate{0, 1};
};

Rational read_rational(JsonValue const& root, std::string_view key)
{
    Rational value{0, 1};
    if (auto const* object = root.find(key); object && object->is_object())
    {
        value.numerator = static_cast<std::uint32_t>(object->number_or("numerator", 0.0));
        value.denominator = static_cast<std::uint32_t>(object->number_or("denominator", 1.0));
        if (value.denominator == 0)
        {
            value.denominator = 1;
        }
    }
    return value;
}

FlowMeta parse_flow_meta(std::string const& definition)
{
    FlowMeta meta;
    auto const parsed = JsonValue::parse(definition);
    if (!parsed || !parsed->is_object())
    {
        return meta;
    }

    meta.id = parsed->string_or("id");
    meta.media_type = parsed->string_or("media_type");
    meta.audio = meta.media_type.rfind("audio/", 0) == 0;

    if (meta.audio)
    {
        meta.channels = static_cast<std::uint32_t>(parsed->number_or("channel_count", 0.0));
        meta.rate = read_rational(*parsed, "sample_rate");
    }
    else
    {
        meta.width = static_cast<std::uint32_t>(parsed->number_or("frame_width", 0.0));
        meta.height = static_cast<std::uint32_t>(parsed->number_or("frame_height", 0.0));
        meta.rate = read_rational(*parsed, "grain_rate");
    }
    return meta;
}

GrainInfo convert(mxlGrainInfo const& grain) noexcept
{
    GrainInfo result;
    result.index = grain.index;
    result.flags = grain.flags;
    result.grain_size = grain.grainSize;
    result.total_slices = grain.totalSlices;
    result.valid_slices = grain.validSlices;
    return result;
}

struct InstanceState
{
    mxlInstance instance{nullptr};
    std::mutex mutex;
    bool alive{false};
};

using InstanceStatePtr = std::shared_ptr<InstanceState>;

/** Base class that releases an MXL handle exactly once, safely. */
template<typename Handle, typename ReleaseFn> class OwnedHandle
{
public:
    OwnedHandle(InstanceStatePtr state, Handle handle, ReleaseFn release)
        : state_(std::move(state)), handle_(handle), release_(std::move(release))
    {
    }

    ~OwnedHandle()
    {
        if (auto state = state_.lock())
        {
            std::lock_guard lock{state->mutex};
            if (state->alive && (handle_ != nullptr))
            {
                release_(state->instance, handle_);
                handle_ = nullptr;
            }
        }
    }

    OwnedHandle(OwnedHandle const&) = delete;
    OwnedHandle& operator=(OwnedHandle const&) = delete;

    [[nodiscard]] Handle handle() const noexcept { return handle_; }

private:
    std::weak_ptr<InstanceState> state_;
    Handle handle_{nullptr};
    ReleaseFn release_;
};

using WriterHandle = OwnedHandle<mxlFlowWriter, mxlStatus (*)(mxlInstance, mxlFlowWriter)>;
using ReaderHandle = OwnedHandle<mxlFlowReader, mxlStatus (*)(mxlInstance, mxlFlowReader)>;

class MxlVideoWriter final : public VideoWriter
{
public:
    MxlVideoWriter(InstanceStatePtr state, mxlFlowWriter writer, VideoStreamInfo info)
        : handle_(std::move(state), writer, &mxlReleaseFlowWriter), info_(info)
    {
    }

    Status open_grain(std::uint64_t index, GrainInfo& info, std::uint8_t*& payload) override
    {
        mxlGrainInfo grain{};
        std::uint8_t* raw = nullptr;
        if (auto const status = mxlFlowWriterOpenGrain(handle_.handle(), index, &grain, &raw);
            status != MXL_STATUS_OK)
        {
            return map_status(status);
        }

        last_open_ = grain;
        last_open_valid_ = true;
        info = convert(grain);
        payload = raw;
        return Status::Ok;
    }

    Status commit_grain(GrainInfo const& info) override
    {
        if (!last_open_valid_)
        {
            return Status::InvalidArgument;
        }

        last_open_.index = info.index;
        last_open_.flags = info.flags;
        last_open_.grainSize = info.grain_size;
        last_open_.validSlices = info.valid_slices;

        auto const status = mxlFlowWriterCommitGrain(handle_.handle(), &last_open_);
        if ((status == MXL_STATUS_OK) && (last_open_.validSlices == last_open_.totalSlices))
        {
            last_open_valid_ = false;
        }
        return map_status(status);
    }

    Status cancel_grain() override
    {
        last_open_valid_ = false;
        return map_status(mxlFlowWriterCancelGrain(handle_.handle()));
    }

    VideoStreamInfo const& info() const noexcept override { return info_; }

private:
    WriterHandle handle_;
    VideoStreamInfo info_;
    mxlGrainInfo last_open_{};
    bool last_open_valid_{false};
};

class MxlAudioWriter final : public AudioWriter
{
public:
    MxlAudioWriter(InstanceStatePtr state, mxlFlowWriter writer, AudioStreamInfo info)
        : handle_(std::move(state), writer, &mxlReleaseFlowWriter), info_(info)
    {
    }

    Status open_samples(std::uint64_t end_index, std::size_t count,
                        MutableSampleSpan& span) override
    {
        mxlMutableWrappedMultiBufferSlice slices{};
        if (auto const status =
                mxlFlowWriterOpenSamples(handle_.handle(), end_index, count, &slices);
            status != MXL_STATUS_OK)
        {
            return map_status(status);
        }

        for (std::size_t i = 0; i < 2; ++i)
        {
            span.fragments[i].pointer =
                static_cast<std::uint8_t*>(slices.base.fragments[i].pointer);
            span.fragments[i].size = slices.base.fragments[i].size;
        }
        span.stride = slices.stride;
        span.count = slices.count;
        return Status::Ok;
    }

    Status commit_samples() override
    {
        return map_status(mxlFlowWriterCommitSamples(handle_.handle()));
    }

    Status cancel_samples() override
    {
        return map_status(mxlFlowWriterCancelSamples(handle_.handle()));
    }

    AudioStreamInfo const& info() const noexcept override { return info_; }

private:
    WriterHandle handle_;
    AudioStreamInfo info_;
};

class MxlVideoReader final : public VideoReader
{
public:
    MxlVideoReader(InstanceStatePtr state, mxlFlowReader reader, VideoStreamInfo info)
        : handle_(std::move(state), reader, &mxlReleaseFlowReader), info_(info)
    {
    }

    Status get_grain(std::uint64_t index, std::uint16_t min_valid_slices, std::uint64_t timeout_ns,
                     GrainInfo& info, std::uint8_t const*& payload) override
    {
        mxlGrainInfo grain{};
        std::uint8_t* raw = nullptr;
        mxlStatus status = MXL_STATUS_OK;
        if (min_valid_slices == MXL_GRAIN_VALID_SLICES_ALL)
        {
            status = mxlFlowReaderGetGrain(handle_.handle(), index, timeout_ns, &grain, &raw);
        }
        else
        {
            status = mxlFlowReaderGetGrainSlice(handle_.handle(), index, min_valid_slices,
                                                timeout_ns, &grain, &raw);
        }

        if (status != MXL_STATUS_OK)
        {
            return map_status(status);
        }

        info = convert(grain);
        payload = raw;
        return Status::Ok;
    }

    Status fetch_info(VideoStreamInfo& info) override
    {
        mxlFlowConfigInfo config{};
        if (auto const status = mxlFlowReaderGetConfigInfo(handle_.handle(), &config);
            status != MXL_STATUS_OK)
        {
            return map_status(status);
        }

        info = info_;
        info.stride = config.discrete.sliceSizes[0];
        info.grain_count = config.discrete.grainCount;
        info.grain_rate = Rational{static_cast<std::uint32_t>(config.common.grainRate.numerator),
                                   static_cast<std::uint32_t>(config.common.grainRate.denominator)};
        return Status::Ok;
    }

    Status head_index(std::uint64_t& index) override
    {
        mxlFlowRuntimeInfo runtime{};
        if (auto const status = mxlFlowReaderGetRuntimeInfo(handle_.handle(), &runtime);
            status != MXL_STATUS_OK)
        {
            return map_status(status);
        }
        index = runtime.headIndex;
        return Status::Ok;
    }

private:
    ReaderHandle handle_;
    VideoStreamInfo info_;
};

class MxlAudioReader final : public AudioReader
{
public:
    MxlAudioReader(InstanceStatePtr state, mxlFlowReader reader, AudioStreamInfo info)
        : handle_(std::move(state), reader, &mxlReleaseFlowReader), info_(info)
    {
    }

    Status get_samples(std::uint64_t end_index, std::size_t count, std::uint64_t timeout_ns,
                       ConstSampleSpan& span) override
    {
        mxlWrappedMultiBufferSlice slices{};
        if (auto const status =
                mxlFlowReaderGetSamples(handle_.handle(), end_index, count, timeout_ns, &slices);
            status != MXL_STATUS_OK)
        {
            return map_status(status);
        }

        for (std::size_t i = 0; i < 2; ++i)
        {
            span.fragments[i].pointer =
                static_cast<std::uint8_t const*>(slices.base.fragments[i].pointer);
            span.fragments[i].size = slices.base.fragments[i].size;
        }
        span.stride = slices.stride;
        span.count = slices.count;
        return Status::Ok;
    }

    Status fetch_info(AudioStreamInfo& info) override
    {
        mxlFlowConfigInfo config{};
        if (auto const status = mxlFlowReaderGetConfigInfo(handle_.handle(), &config);
            status != MXL_STATUS_OK)
        {
            return map_status(status);
        }

        info = info_;
        info.channels = config.continuous.channelCount;
        info.buffer_length = config.continuous.bufferLength;
        info.sample_rate = config.common.grainRate.numerator;
        info.max_batch = config.common.maxCommitBatchSizeHint;
        return Status::Ok;
    }

    Status head_index(std::uint64_t& index) override
    {
        mxlFlowRuntimeInfo runtime{};
        if (auto const status = mxlFlowReaderGetRuntimeInfo(handle_.handle(), &runtime);
            status != MXL_STATUS_OK)
        {
            return map_status(status);
        }
        index = runtime.headIndex;
        return Status::Ok;
    }

private:
    ReaderHandle handle_;
    AudioStreamInfo info_;
};

class MxlBackend final : public Backend
{
public:
    explicit MxlBackend(std::string domain) : domain_(std::move(domain))
    {
        std::error_code ec;
        std::filesystem::create_directories(domain_, ec);
        if (ec && !std::filesystem::is_directory(domain_))
        {
            error_ = "cannot create MXL domain " + domain_ + ": " + ec.message();
            return;
        }

        state_ = std::make_shared<InstanceState>();
        state_->instance = mxlCreateInstance(domain_.c_str(), "");
        if (state_->instance == nullptr)
        {
            error_ = "mxlCreateInstance failed for domain " + domain_;
            return;
        }
        state_->alive = true;
    }

    ~MxlBackend() override
    {
        if (state_)
        {
            std::lock_guard lock{state_->mutex};
            state_->alive = false;
            if (state_->instance != nullptr)
            {
                mxlDestroyInstance(state_->instance);
                state_->instance = nullptr;
            }
        }
    }

    Status create_video_writer(std::string const& flow_def, std::string const& options,
                               std::unique_ptr<VideoWriter>& writer, VideoStreamInfo& info,
                               bool* created) override
    {
        auto const meta = parse_flow_meta(flow_def);
        if (meta.audio || (meta.media_type != "video/v210") || (meta.width == 0) ||
            (meta.height == 0) || !meta.rate.valid())
        {
            error_ = "invalid video flow definition";
            return Status::InvalidArgument;
        }

        mxlFlowWriter handle = nullptr;
        mxlFlowConfigInfo config{};
        bool was_created = false;
        auto const status = mxlCreateFlowWriter(state_->instance, flow_def.c_str(),
                                                options.empty() ? nullptr : options.c_str(),
                                                &handle, &config, &was_created);
        if (status != MXL_STATUS_OK)
        {
            error_ = "mxlCreateFlowWriter failed: " + std::string{to_string(map_status(status))};
            return map_status(status);
        }

        if (!was_created)
        {
            auto const existing = parse_flow_meta(get_flow_definition(meta.id));
            if (existing.media_type != "video/v210")
            {
                mxlReleaseFlowWriter(state_->instance, handle);
                error_ = "flow " + meta.id + " already exists with a different media type";
                return Status::Conflict;
            }
        }

        info.width = meta.width;
        info.height = meta.height;
        info.stride = config.discrete.sliceSizes[0];
        info.grain_count = config.discrete.grainCount;
        info.total_slices = static_cast<std::uint16_t>(meta.height);
        info.grain_rate = Rational{static_cast<std::uint32_t>(config.common.grainRate.numerator),
                                   static_cast<std::uint32_t>(config.common.grainRate.denominator)};

        if (created)
        {
            *created = was_created;
        }
        writer = std::make_unique<MxlVideoWriter>(state_, handle, info);
        return Status::Ok;
    }

    Status create_audio_writer(std::string const& flow_def, std::string const& options,
                               std::unique_ptr<AudioWriter>& writer, AudioStreamInfo& info,
                               bool* created) override
    {
        auto const meta = parse_flow_meta(flow_def);
        if (!meta.audio || (meta.media_type != "audio/float32") || (meta.channels == 0) ||
            (meta.rate.numerator == 0))
        {
            error_ = "invalid audio flow definition";
            return Status::InvalidArgument;
        }

        mxlFlowWriter handle = nullptr;
        mxlFlowConfigInfo config{};
        bool was_created = false;
        auto const status = mxlCreateFlowWriter(state_->instance, flow_def.c_str(),
                                                options.empty() ? nullptr : options.c_str(),
                                                &handle, &config, &was_created);
        if (status != MXL_STATUS_OK)
        {
            error_ = "mxlCreateFlowWriter failed: " + std::string{to_string(map_status(status))};
            return map_status(status);
        }

        info.channels = config.continuous.channelCount;
        info.buffer_length = config.continuous.bufferLength;
        info.sample_rate = config.common.grainRate.numerator;
        info.max_batch = config.common.maxCommitBatchSizeHint;

        if (!was_created)
        {
            auto const existing = parse_flow_meta(get_flow_definition(meta.id));
            if (existing.media_type != "audio/float32")
            {
                mxlReleaseFlowWriter(state_->instance, handle);
                error_ = "flow " + meta.id + " already exists with a different media type";
                return Status::Conflict;
            }
        }

        if (created)
        {
            *created = was_created;
        }
        writer = std::make_unique<MxlAudioWriter>(state_, handle, info);
        return Status::Ok;
    }

    Status open_video_reader(std::string const& flow_id,
                             std::unique_ptr<VideoReader>& reader) override
    {
        mxlFlowReader handle = nullptr;
        auto const status =
            mxlCreateFlowReader(state_->instance, flow_id.c_str(), nullptr, &handle);
        if (status != MXL_STATUS_OK)
        {
            error_ = "mxlCreateFlowReader(" + flow_id +
                     ") failed: " + std::string{to_string(map_status(status))};
            return map_status(status);
        }

        auto const definition = get_flow_definition(flow_id);
        if (definition.empty())
        {
            mxlReleaseFlowReader(state_->instance, handle);
            return Status::NotFound;
        }

        auto const meta = parse_flow_meta(definition);
        if (meta.audio || (meta.media_type != "video/v210") || (meta.width == 0) ||
            (meta.height == 0) || !meta.rate.valid())
        {
            mxlReleaseFlowReader(state_->instance, handle);
            error_ = "flow " + flow_id + " is not a valid V210 video flow";
            return Status::Unsupported;
        }

        VideoStreamInfo info;
        info.width = meta.width;
        info.height = meta.height;
        info.stride = v210_line_stride(meta.width);
        info.total_slices = static_cast<std::uint16_t>(meta.height);
        info.grain_rate = meta.rate;

        reader = std::make_unique<MxlVideoReader>(state_, handle, info);
        if (auto const fetch = reader->fetch_info(info); fetch != Status::Ok)
        {
            reader.reset();
            error_ = "cannot read flow configuration for " + flow_id;
            return fetch;
        }
        return Status::Ok;
    }

    Status open_audio_reader(std::string const& flow_id,
                             std::unique_ptr<AudioReader>& reader) override
    {
        mxlFlowReader handle = nullptr;
        auto const status =
            mxlCreateFlowReader(state_->instance, flow_id.c_str(), nullptr, &handle);
        if (status != MXL_STATUS_OK)
        {
            error_ = "mxlCreateFlowReader(" + flow_id +
                     ") failed: " + std::string{to_string(map_status(status))};
            return map_status(status);
        }

        auto const definition = get_flow_definition(flow_id);
        if (definition.empty())
        {
            mxlReleaseFlowReader(state_->instance, handle);
            return Status::NotFound;
        }

        auto const meta = parse_flow_meta(definition);
        if (!meta.audio || (meta.media_type != "audio/float32") || (meta.channels == 0) ||
            (meta.rate.numerator == 0))
        {
            mxlReleaseFlowReader(state_->instance, handle);
            error_ = "flow " + flow_id + " is not a valid audio flow";
            return Status::Unsupported;
        }

        AudioStreamInfo info;
        info.channels = meta.channels;
        info.sample_rate = meta.rate.numerator;

        reader = std::make_unique<MxlAudioReader>(state_, handle, info);
        if (auto const fetch = reader->fetch_info(info); fetch != Status::Ok)
        {
            reader.reset();
            error_ = "cannot read flow configuration for " + flow_id;
            return fetch;
        }
        return Status::Ok;
    }

    std::string const& last_error() const noexcept override { return error_; }

    std::string const& domain() const noexcept override { return domain_; }

private:
    std::string get_flow_definition(std::string const& flow_id)
    {
        std::size_t size = 0;
        auto status = mxlGetFlowDef(state_->instance, flow_id.c_str(), nullptr, &size);
        if ((status != MXL_ERR_INVALID_ARG) && (status != MXL_STATUS_OK))
        {
            error_ = "mxlGetFlowDef failed: " + std::string{to_string(map_status(status))};
            return {};
        }
        if (size == 0)
        {
            return {};
        }

        std::string definition(size, '\0');
        status = mxlGetFlowDef(state_->instance, flow_id.c_str(), definition.data(), &size);
        if (status != MXL_STATUS_OK)
        {
            error_ = "mxlGetFlowDef failed: " + std::string{to_string(map_status(status))};
            return {};
        }

        // mxlGetFlowDef reports the size including the null terminator.
        if ((size > 0) && (definition[size - 1] == '\0'))
        {
            definition.resize(size - 1);
        }
        else if (size <= definition.size())
        {
            definition.resize(size);
        }
        return definition;
    }

    std::string domain_;
    std::string error_;
    InstanceStatePtr state_;
};
} // namespace

std::unique_ptr<Backend> create_mxl_backend(std::string domain, std::string* error)
{
    auto backend = std::make_unique<MxlBackend>(std::move(domain));
    if (!backend->last_error().empty())
    {
        if (error)
        {
            *error = backend->last_error();
        }
        return nullptr;
    }
    return backend;
}

std::unique_ptr<Backend> create_backend(BackendKind kind, std::string domain, std::string* error)
{
    switch (kind)
    {
    case BackendKind::Mxl:
        return create_mxl_backend(std::move(domain), error);
    case BackendKind::Fake:
        return create_fake_backend(std::move(domain));
    }
    return nullptr;
}
} // namespace obsmxl

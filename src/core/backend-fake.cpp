// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/fake-backend.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <obsmxl/flow-definition.hpp>
#include <obsmxl/json.hpp>
#include <obsmxl/uuid.hpp>
#include <obsmxl/video-format.hpp>

namespace obsmxl
{
namespace
{
constexpr std::uint32_t grain_flag_invalid = 1U;

struct ParsedFlow
{
    bool audio{false};
    std::string id;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t channels{0};
    Rational rate{0, 1};
};

ParsedFlow parse_flow(std::string const& definition, std::string* error)
{
    ParsedFlow result;
    auto const parsed = JsonValue::parse(definition, error);
    if (!parsed || !parsed->is_object())
    {
        if (error && error->empty())
        {
            *error = "invalid flow definition JSON";
        }
        return result;
    }

    result.id = parsed->string_or("id");
    auto const media_type = parsed->string_or("media_type");
    result.audio = media_type.rfind("audio/", 0) == 0;

    auto const read_rational = [&parsed](std::string_view key)
    {
        Rational value{0, 1};
        auto const* object = parsed->find(key);
        if (object && object->is_object())
        {
            value.numerator = static_cast<std::uint32_t>(object->number_or("numerator", 0.0));
            value.denominator = static_cast<std::uint32_t>(object->number_or("denominator", 1.0));
        }
        return value;
    };

    if (result.audio)
    {
        result.channels = static_cast<std::uint32_t>(parsed->number_or("channel_count", 0.0));
        result.rate = read_rational("sample_rate");
    }
    else
    {
        result.width = static_cast<std::uint32_t>(parsed->number_or("frame_width", 0.0));
        result.height = static_cast<std::uint32_t>(parsed->number_or("frame_height", 0.0));
        result.rate = read_rational("grain_rate");
    }

    if (!is_valid_uuid(result.id) || !result.rate.valid())
    {
        if (error)
        {
            *error = "flow definition is missing a valid id or rate";
        }
    }
    return result;
}
} // namespace

struct FakeFlow
{
    std::string id;
    bool audio{false};
    std::string definition;

    VideoStreamInfo video{};
    AudioStreamInfo audio_info{};

    std::vector<std::vector<std::uint8_t>> grains;
    std::vector<GrainInfo> grain_infos;
    std::vector<float> samples; // channels * buffer_length, contiguous

    std::uint64_t head{0};
    std::uint64_t last_committed{undefined_index};

    bool open{false};
    std::uint64_t open_index{undefined_index};
    std::size_t open_count{0};

    std::mutex mutex;
    std::condition_variable cv;
};

struct FakeDomain::Impl
{
    std::string path;
    int history_ms{200};
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<FakeFlow>> flows;
};

FakeDomain::FakeDomain(std::string path) : impl_(std::make_shared<Impl>())
{
    impl_->path = std::move(path);
}

FakeDomain::~FakeDomain() = default;

std::string const& FakeDomain::path() const noexcept
{
    return impl_->path;
}

std::size_t FakeDomain::flow_count() const
{
    std::lock_guard lock{impl_->mutex};
    return impl_->flows.size();
}

void FakeDomain::clear()
{
    std::lock_guard lock{impl_->mutex};
    impl_->flows.clear();
}

void FakeDomain::set_history_duration_ms(int milliseconds)
{
    std::lock_guard lock{impl_->mutex};
    impl_->history_ms = std::max(50, milliseconds);
}

int FakeDomain::history_duration_ms() const
{
    std::lock_guard lock{impl_->mutex};
    return impl_->history_ms;
}

namespace
{
std::map<std::string, std::weak_ptr<FakeDomain>>& fake_registry()
{
    static std::map<std::string, std::weak_ptr<FakeDomain>> registry;
    return registry;
}

std::mutex& fake_registry_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<FakeDomain> domain_for_path(std::string const& path)
{
    std::lock_guard lock{fake_registry_mutex()};
    auto& registry = fake_registry();
    auto it = registry.find(path);
    if (it != registry.end())
    {
        if (auto existing = it->second.lock())
        {
            return existing;
        }
    }
    auto domain = std::make_shared<FakeDomain>(path);
    registry[path] = domain;
    return domain;
}

class FakeVideoWriter final : public VideoWriter
{
public:
    FakeVideoWriter(std::shared_ptr<FakeFlow> flow, VideoStreamInfo info)
        : flow_(std::move(flow)), info_(info)
    {
    }

    Status open_grain(std::uint64_t index, GrainInfo& info, std::uint8_t*& payload) override
    {
        std::lock_guard lock{flow_->mutex};

        if (flow_->last_committed != undefined_index && index <= flow_->last_committed)
        {
            return Status::InvalidArgument;
        }

        if (index > flow_->head + 1)
        {
            auto const grain_count = flow_->grain_infos.size();
            auto const skipped = index - flow_->head - 1;
            auto const invalidated = std::min<std::uint64_t>(skipped, grain_count);
            auto const first = index - invalidated;
            for (auto missing = first; missing < index; ++missing)
            {
                auto& slot = flow_->grain_infos[missing % grain_count];
                slot = GrainInfo{};
                slot.index = missing;
                slot.flags = grain_flag_invalid;
                slot.total_slices = static_cast<std::uint16_t>(info_.height);
            }
        }

        auto const slot = index % flow_->grain_infos.size();
        auto& slot_info = flow_->grain_infos[slot];
        slot_info = GrainInfo{};
        slot_info.index = index;
        slot_info.grain_size = static_cast<std::uint32_t>(flow_->grains[slot].size());
        slot_info.total_slices = static_cast<std::uint16_t>(info_.height);

        flow_->open = true;
        flow_->open_index = index;
        info = slot_info;
        payload = flow_->grains[slot].data();
        return Status::Ok;
    }

    Status commit_grain(GrainInfo const& info) override
    {
        std::lock_guard lock{flow_->mutex};
        if (!flow_->open || (info.index != flow_->open_index))
        {
            return Status::InvalidArgument;
        }

        auto const slot = info.index % flow_->grain_infos.size();
        flow_->grain_infos[slot] = info;
        flow_->head = info.index;
        if (info.valid_slices == info.total_slices)
        {
            flow_->last_committed = info.index;
            flow_->open = false;
            flow_->open_index = undefined_index;
        }
        flow_->cv.notify_all();
        return Status::Ok;
    }

    Status cancel_grain() override
    {
        std::lock_guard lock{flow_->mutex};
        flow_->open = false;
        flow_->open_index = undefined_index;
        return Status::Ok;
    }

    VideoStreamInfo const& info() const noexcept override { return info_; }

private:
    std::shared_ptr<FakeFlow> flow_;
    VideoStreamInfo info_;
};

class FakeAudioWriter final : public AudioWriter
{
public:
    FakeAudioWriter(std::shared_ptr<FakeFlow> flow, AudioStreamInfo info)
        : flow_(std::move(flow)), info_(info)
    {
    }

    Status open_samples(std::uint64_t end_index, std::size_t count,
                        MutableSampleSpan& span) override
    {
        std::lock_guard lock{flow_->mutex};
        auto const buffer_length = info_.buffer_length;
        if ((count == 0) || (count > buffer_length / 2) || (end_index < count))
        {
            return Status::InvalidArgument;
        }

        auto const start_index = end_index - count;
        if (flow_->last_committed != undefined_index &&
            ((end_index <= flow_->last_committed) || (start_index < flow_->last_committed)))
        {
            return Status::InvalidArgument;
        }

        auto const start_offset = start_index % buffer_length;
        auto const first_length = std::min<std::size_t>(count, buffer_length - start_offset);
        auto const second_length = count - first_length;

        auto const* base = flow_->samples.data() + static_cast<std::size_t>(start_offset);
        span.fragments[0].pointer =
            const_cast<std::uint8_t*>(reinterpret_cast<std::uint8_t const*>(base));
        span.fragments[0].size = first_length * sizeof(float);
        span.fragments[1].pointer =
            const_cast<std::uint8_t*>(reinterpret_cast<std::uint8_t const*>(flow_->samples.data()));
        span.fragments[1].size = second_length * sizeof(float);
        span.stride = static_cast<std::size_t>(buffer_length) * sizeof(float);
        span.count = info_.channels;

        flow_->open = true;
        flow_->open_index = end_index;
        flow_->open_count = count;
        return Status::Ok;
    }

    Status commit_samples() override
    {
        std::lock_guard lock{flow_->mutex};
        if (!flow_->open)
        {
            return Status::InvalidArgument;
        }
        flow_->head = flow_->open_index;
        flow_->last_committed = flow_->open_index;
        flow_->open = false;
        flow_->open_index = undefined_index;
        flow_->cv.notify_all();
        return Status::Ok;
    }

    Status cancel_samples() override
    {
        std::lock_guard lock{flow_->mutex};
        flow_->open = false;
        flow_->open_index = undefined_index;
        return Status::Ok;
    }

    AudioStreamInfo const& info() const noexcept override { return info_; }

private:
    std::shared_ptr<FakeFlow> flow_;
    AudioStreamInfo info_;
};

class FakeVideoReader final : public VideoReader
{
public:
    explicit FakeVideoReader(std::shared_ptr<FakeFlow> flow) : flow_(std::move(flow)) {}

    Status get_grain(std::uint64_t index, std::uint16_t min_valid_slices, std::uint64_t timeout_ns,
                     GrainInfo& info, std::uint8_t const*& payload) override
    {
        std::unique_lock lock{flow_->mutex};
        auto const grain_count = flow_->grain_infos.size();
        if (grain_count == 0)
        {
            return Status::NotFound;
        }

        auto const deadline =
            std::chrono::steady_clock::now() + std::chrono::nanoseconds{timeout_ns};

        while (true)
        {
            if (index <= flow_->head)
            {
                auto const min_index = (flow_->head >= grain_count)
                                           ? (flow_->head - grain_count + 2U)
                                           : std::uint64_t{0};
                if (index < min_index)
                {
                    return Status::TooLate;
                }

                auto const& slot = flow_->grain_infos[index % grain_count];
                auto const enough =
                    (slot.valid_slices >=
                     std::min<std::uint16_t>(min_valid_slices, slot.total_slices)) ||
                    ((slot.flags & grain_flag_invalid) != 0) || (min_valid_slices == 0);
                if (slot.index == index)
                {
                    if (enough)
                    {
                        info = slot;
                        payload = flow_->grains[index % grain_count].data();
                        return Status::Ok;
                    }
                    return Status::TooEarly;
                }
            }

            if (flow_->cv.wait_until(lock, deadline) == std::cv_status::timeout)
            {
                return Status::Timeout;
            }
        }
    }

    Status fetch_info(VideoStreamInfo& info) override
    {
        std::lock_guard lock{flow_->mutex};
        info = flow_->video;
        return Status::Ok;
    }

    Status head_index(std::uint64_t& index) override
    {
        std::lock_guard lock{flow_->mutex};
        index = flow_->head;
        return Status::Ok;
    }

private:
    std::shared_ptr<FakeFlow> flow_;
};

class FakeAudioReader final : public AudioReader
{
public:
    explicit FakeAudioReader(std::shared_ptr<FakeFlow> flow) : flow_(std::move(flow)) {}

    Status get_samples(std::uint64_t end_index, std::size_t count, std::uint64_t timeout_ns,
                       ConstSampleSpan& span) override
    {
        std::unique_lock lock{flow_->mutex};
        auto const buffer_length = flow_->audio_info.buffer_length;
        if ((buffer_length == 0) || (count == 0) || (count > buffer_length / 2) ||
            (end_index < count))
        {
            return Status::InvalidArgument;
        }

        auto const deadline =
            std::chrono::steady_clock::now() + std::chrono::nanoseconds{timeout_ns};

        while (flow_->head < end_index)
        {
            if (flow_->cv.wait_until(lock, deadline) == std::cv_status::timeout)
            {
                return Status::Timeout;
            }
        }

        auto const start_index = end_index - count;
        auto const earliest = (flow_->head > buffer_length / 2) ? (flow_->head - buffer_length / 2)
                                                                : std::uint64_t{0};
        if (start_index < earliest)
        {
            return Status::TooLate;
        }

        auto const start_offset = start_index % buffer_length;
        auto const first_length = std::min<std::size_t>(count, buffer_length - start_offset);
        auto const second_length = count - first_length;

        auto const* base = flow_->samples.data() + static_cast<std::size_t>(start_offset);
        span.fragments[0].pointer = reinterpret_cast<std::uint8_t const*>(base);
        span.fragments[0].size = first_length * sizeof(float);
        span.fragments[1].pointer = reinterpret_cast<std::uint8_t const*>(flow_->samples.data());
        span.fragments[1].size = second_length * sizeof(float);
        span.stride = static_cast<std::size_t>(buffer_length) * sizeof(float);
        span.count = flow_->audio_info.channels;
        return Status::Ok;
    }

    Status fetch_info(AudioStreamInfo& info) override
    {
        std::lock_guard lock{flow_->mutex};
        info = flow_->audio_info;
        return Status::Ok;
    }

    Status head_index(std::uint64_t& index) override
    {
        std::lock_guard lock{flow_->mutex};
        index = flow_->head;
        return Status::Ok;
    }

private:
    std::shared_ptr<FakeFlow> flow_;
};

} // namespace

class FakeBackendImpl final : public Backend
{
public:
    explicit FakeBackendImpl(std::shared_ptr<FakeDomain> domain) : domain_(std::move(domain)) {}

    Status create_video_writer(std::string const& flow_def, std::string const&,
                               std::unique_ptr<VideoWriter>& writer, VideoStreamInfo& info,
                               bool* created) override
    {
        auto const parsed = parse_flow(flow_def, &error_);
        if (!parsed.id.empty() && parsed.audio)
        {
            error_ = "flow definition is not a video flow";
            return Status::InvalidArgument;
        }
        if (parsed.id.empty() || (parsed.width < 2) || (parsed.height < 2))
        {
            error_ = "invalid video flow definition";
            return Status::InvalidArgument;
        }

        auto flow = get_or_create(parsed, flow_def, &info, nullptr, created);
        if (!flow)
        {
            return Status::Error;
        }

        writer = std::make_unique<FakeVideoWriter>(flow, info);
        return Status::Ok;
    }

    Status create_audio_writer(std::string const& flow_def, std::string const&,
                               std::unique_ptr<AudioWriter>& writer, AudioStreamInfo& info,
                               bool* created) override
    {
        auto const parsed = parse_flow(flow_def, &error_);
        if (parsed.id.empty() || !parsed.audio || (parsed.channels == 0))
        {
            error_ = "invalid audio flow definition";
            return Status::InvalidArgument;
        }

        auto flow = get_or_create(parsed, flow_def, nullptr, &info, created);
        if (!flow)
        {
            return Status::Error;
        }

        writer = std::make_unique<FakeAudioWriter>(flow, info);
        return Status::Ok;
    }

    Status open_video_reader(std::string const& flow_id,
                             std::unique_ptr<VideoReader>& reader) override
    {
        auto flow = find(flow_id);
        if (!flow || flow->audio)
        {
            error_ = "video flow not found: " + flow_id;
            return Status::NotFound;
        }
        reader = std::make_unique<FakeVideoReader>(flow);
        return Status::Ok;
    }

    Status open_audio_reader(std::string const& flow_id,
                             std::unique_ptr<AudioReader>& reader) override
    {
        auto flow = find(flow_id);
        if (!flow || !flow->audio)
        {
            error_ = "audio flow not found: " + flow_id;
            return Status::NotFound;
        }
        reader = std::make_unique<FakeAudioReader>(flow);
        return Status::Ok;
    }

    std::string const& last_error() const noexcept override { return error_; }

    std::string const& domain() const noexcept override { return domain_->path(); }

private:
    std::shared_ptr<FakeFlow> find(std::string const& id)
    {
        std::lock_guard lock{domain_->impl_->mutex};
        auto const it = domain_->impl_->flows.find(id);
        return (it != domain_->impl_->flows.end()) ? it->second : nullptr;
    }

    std::shared_ptr<FakeFlow> get_or_create(ParsedFlow const& parsed, std::string const& definition,
                                            VideoStreamInfo* video_info,
                                            AudioStreamInfo* audio_info, bool* created)
    {
        std::lock_guard lock{domain_->impl_->mutex};
        auto& flows = domain_->impl_->flows;
        if (auto const it = flows.find(parsed.id); it != flows.end())
        {
            if (it->second->audio != parsed.audio)
            {
                error_ = "flow " + parsed.id + " already exists with a different media type";
                return nullptr;
            }
            if (created)
            {
                *created = false;
            }
            if (video_info)
            {
                *video_info = it->second->video;
            }
            if (audio_info)
            {
                *audio_info = it->second->audio_info;
            }
            return it->second;
        }

        auto flow = std::make_shared<FakeFlow>();
        flow->id = parsed.id;
        flow->audio = parsed.audio;
        flow->definition = definition;

        auto const history_ns =
            static_cast<std::uint64_t>(domain_->impl_->history_ms) * 1'000'000ULL;

        if (parsed.audio)
        {
            auto const buffer_length = std::max<std::uint32_t>(
                2, static_cast<std::uint32_t>((history_ns * parsed.rate.numerator) /
                                              (500'000'000ULL * parsed.rate.denominator)));
            auto const page = 4096U / static_cast<std::uint32_t>(sizeof(float));
            auto const aligned = ((buffer_length + page - 1U) / page) * page;

            flow->audio_info.sample_rate = parsed.rate.numerator;
            flow->audio_info.channels = parsed.channels;
            flow->audio_info.buffer_length = aligned;
            flow->audio_info.max_batch = std::max<std::uint32_t>(1, parsed.rate.numerator / 100U);
            flow->samples.assign(static_cast<std::size_t>(aligned) * parsed.channels, 0.0F);
        }
        else
        {
            auto const grain_count = std::max<std::uint32_t>(
                2, static_cast<std::uint32_t>((history_ns * parsed.rate.numerator) /
                                              (1'000'000'000ULL * parsed.rate.denominator)));
            auto const stride = v210_line_stride(parsed.width);

            flow->video.width = parsed.width;
            flow->video.height = parsed.height;
            flow->video.stride = stride;
            flow->video.grain_count = grain_count;
            flow->video.total_slices = static_cast<std::uint16_t>(parsed.height);
            flow->video.grain_rate = parsed.rate;

            flow->grains.resize(grain_count);
            flow->grain_infos.resize(grain_count);
            for (auto& grain : flow->grains)
            {
                grain.assign(v210_buffer_size(stride, parsed.height), 0);
            }
            for (auto& info : flow->grain_infos)
            {
                info.total_slices = static_cast<std::uint16_t>(parsed.height);
                info.grain_size =
                    static_cast<std::uint32_t>(v210_buffer_size(stride, parsed.height));
            }
        }

        if (created)
        {
            *created = true;
        }
        if (video_info)
        {
            *video_info = flow->video;
        }
        if (audio_info)
        {
            *audio_info = flow->audio_info;
        }

        flows.emplace(parsed.id, flow);
        return flow;
    }

    std::shared_ptr<FakeDomain> domain_;
    std::string error_;
};

std::shared_ptr<FakeDomain> make_fake_domain(std::string path)
{
    return std::make_shared<FakeDomain>(std::move(path));
}

std::unique_ptr<Backend> create_fake_backend(std::shared_ptr<FakeDomain> domain)
{
    if (!domain)
    {
        return nullptr;
    }
    return std::make_unique<FakeBackendImpl>(std::move(domain));
}

std::unique_ptr<Backend> create_fake_backend(std::string path)
{
    return std::make_unique<FakeBackendImpl>(domain_for_path(path));
}
} // namespace obsmxl

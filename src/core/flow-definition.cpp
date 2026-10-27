// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obsmxl/flow-definition.hpp>

#include <obsmxl/json.hpp>
#include <obsmxl/uuid.hpp>

namespace obsmxl
{
namespace
{
constexpr char const* flow_copyright = "obs-mxl";
constexpr char const* flow_license = "Apache-2.0";

bool validate_common(std::string const& id, std::string const& label, std::string const& group_hint,
                     std::string* error)
{
    if (!is_valid_uuid(id))
    {
        if (error)
        {
            *error = "flow UUID is not a valid RFC 4122 UUID: '" + id + "'";
        }
        return false;
    }

    if (label.empty())
    {
        if (error)
        {
            *error = "flow label must not be empty";
        }
        return false;
    }

    // MXL requires a well formed grouphint tag: <function>:<role>.
    auto const colon = group_hint.find(':');
    if ((colon == std::string::npos) || (colon == 0) || (colon + 1 >= group_hint.size()))
    {
        if (error)
        {
            *error = "group hint must use the '<function>:<role>' form";
        }
        return false;
    }

    return true;
}

void write_common(JsonWriter& writer, std::string const& id, std::string const& label,
                  std::string const& group_hint, std::string const& format_urn,
                  std::string const& media_type)
{
    writer.key("$copyright").value(flow_copyright);
    writer.key("$license").value(flow_license);
    writer.key("description").value(label);
    writer.key("id").value(id);
    writer.key("tags").begin_object();
    writer.key("urn:x-nmos:tag:grouphint/v1.0").begin_array().value(group_hint).end_array();
    writer.end_object();
    writer.key("format").value(format_urn);
    writer.key("label").value(label);
    writer.key("parents").begin_array().end_array();
    writer.key("media_type").value(media_type);
}
} // namespace

bool validate_video_flow_spec(VideoFlowSpec const& spec, std::string* error)
{
    if (!validate_common(spec.id, spec.label, spec.group_hint, error))
    {
        return false;
    }

    if (!spec.grain_rate.valid())
    {
        if (error)
        {
            *error = "invalid grain rate";
        }
        return false;
    }

    if ((spec.width < 2) || (spec.height < 2) || ((spec.width % 2) != 0) ||
        ((spec.height % 2) != 0))
    {
        if (error)
        {
            *error = "V210 frames require even dimensions of at least 2x2";
        }
        return false;
    }

    if (spec.colorspace.empty())
    {
        if (error)
        {
            *error = "colorspace must not be empty";
        }
        return false;
    }

    return true;
}

bool validate_audio_flow_spec(AudioFlowSpec const& spec, std::string* error)
{
    if (!validate_common(spec.id, spec.label, spec.group_hint, error))
    {
        return false;
    }

    if (!spec.sample_rate.valid())
    {
        if (error)
        {
            *error = "invalid sample rate";
        }
        return false;
    }

    if ((spec.channel_count == 0) || (spec.channel_count > 64))
    {
        if (error)
        {
            *error = "audio channel count must be between 1 and 64";
        }
        return false;
    }

    if (!spec.source_id.empty() && !is_valid_uuid(spec.source_id))
    {
        if (error)
        {
            *error = "source_id is not a valid UUID";
        }
        return false;
    }

    if (!spec.device_id.empty() && !is_valid_uuid(spec.device_id))
    {
        if (error)
        {
            *error = "device_id is not a valid UUID";
        }
        return false;
    }

    return true;
}

std::string make_video_flow_json(VideoFlowSpec const& spec)
{
    JsonWriter writer;
    writer.begin_object();
    write_common(writer, spec.id, spec.label, spec.group_hint, "urn:x-nmos:format:video",
                 "video/v210");

    writer.key("grain_rate").begin_object();
    writer.key("numerator").value(spec.grain_rate.numerator);
    writer.key("denominator").value(spec.grain_rate.denominator);
    writer.end_object();

    writer.key("frame_width").value(spec.width);
    writer.key("frame_height").value(spec.height);
    writer.key("interlace_mode").value("progressive");
    writer.key("colorspace").value(spec.colorspace);

    auto const chroma_width = spec.width / 2;
    writer.key("components").begin_array();

    writer.begin_object();
    writer.key("name").value("Y");
    writer.key("width").value(spec.width);
    writer.key("height").value(spec.height);
    writer.key("bit_depth").value(10);
    writer.end_object();

    writer.begin_object();
    writer.key("name").value("Cb");
    writer.key("width").value(chroma_width);
    writer.key("height").value(spec.height);
    writer.key("bit_depth").value(10);
    writer.end_object();

    writer.begin_object();
    writer.key("name").value("Cr");
    writer.key("width").value(chroma_width);
    writer.key("height").value(spec.height);
    writer.key("bit_depth").value(10);
    writer.end_object();

    writer.end_array();
    writer.end_object();
    return writer.str();
}

std::string make_audio_flow_json(AudioFlowSpec const& spec)
{
    std::string source_id = spec.source_id;
    std::string device_id = spec.device_id;
    if (source_id.empty())
    {
        if (auto derived = derive_uuid(spec.id, "source"))
        {
            source_id = format_uuid(*derived);
        }
    }
    if (device_id.empty())
    {
        if (auto derived = derive_uuid(spec.id, "device"))
        {
            device_id = format_uuid(*derived);
        }
    }

    JsonWriter writer;
    writer.begin_object();
    write_common(writer, spec.id, spec.label, spec.group_hint, "urn:x-nmos:format:audio",
                 "audio/float32");

    writer.key("sample_rate").begin_object();
    writer.key("numerator").value(spec.sample_rate.numerator);
    writer.key("denominator").value(spec.sample_rate.denominator);
    writer.end_object();

    writer.key("channel_count").value(spec.channel_count);
    writer.key("bit_depth").value(32);
    if (!source_id.empty())
    {
        writer.key("source_id").value(source_id);
    }
    if (!device_id.empty())
    {
        writer.key("device_id").value(device_id);
    }

    writer.end_object();
    return writer.str();
}

std::string make_writer_options_json(std::uint32_t max_commit_batch_size_hint)
{
    JsonWriter writer;
    writer.begin_object();
    if (max_commit_batch_size_hint > 0)
    {
        writer.key("maxCommitBatchSizeHint").value(max_commit_batch_size_hint);
    }
    writer.end_object();
    return writer.str();
}
} // namespace obsmxl

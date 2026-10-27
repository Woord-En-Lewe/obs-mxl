# Implementation notes and specification deltas

The research document in [`docs/research/`](research/unified_obs_mxl_plugin_specification.md)
describes the intended product and UX. During implementation a number of
details turned out to be different from the real MXL v1.1 SDK and from the OBS
API. This file records every deliberate deviation so that the specification and
the code do not silently diverge.

## 1. The MXL C API is different from the pseudo-code

The specification sketches functions such as
`mxl_flow_reader_get_grain`, `mxl_reader_get_head_index`,
`mxl_audio_writer_commit_channel`, `mxl_futex_wake` and direct writes into
`shared_header->head_grain_index`. Those do not exist in MXL v1.1.

The plugin uses the actual public API from `libmxl`:

| Purpose | Specification sketch | Actual API used |
| --- | --- | --- |
| Create/open instance | – | `mxlCreateInstance(domain, "")` |
| Create a flow | flow name + UUID | `mxlCreateFlowWriter(instance, flow_def_json, options, …)` |
| Open a reader | flow id | `mxlCreateFlowReader(instance, flow_id, nullptr, …)` |
| Read a video grain | `mxl_flow_reader_get_grain` | `mxlFlowReaderGetGrain(reader, index, timeoutNs, &grain, &payload)` (or `mxlFlowReaderGetGrainSlice` for partial grains) |
| Read audio samples | – | `mxlFlowReaderGetSamples(reader, endIndex, count, timeoutNs, &wrappedSpan)` |
| Write a video grain | memcpy into slots + head update | `mxlFlowWriterOpenGrain` → memcpy → `mxlFlowWriterCommitGrain` |
| Write audio samples | `mxl_audio_writer_commit_channel` | `mxlFlowWriterOpenSamples` → memcpy → `mxlFlowWriterCommitSamples` |
| Reader notification | `mxl_futex_wake` | handled inside the SDK (futex based) |

Consequences:

- The plugin never manipulates MXL shared memory headers or futexes directly.
  The SDK guarantees the single-writer/multiple-reader ring semantics.
- Reading remains zero-copy: the payload pointer returned by
  `mxlFlowReaderGetGrain` is passed straight to OBS.
- Flow lifetime is owned by the SDK: the last writer release deletes the flow
  directory.

## 2. Flows are NMOS resources, not just names and UUIDs

`mxlCreateFlowWriter` requires a complete NMOS IS-04 *flow* JSON document
(`id`, `label`, `format`, `media_type`, `grain_rate`, dimensions/components for
video, `sample_rate`/`channel_count`/`bit_depth` for audio, and a valid
`grouphint` tag). The plugin generates this document from the OBS canvas and
mixer configuration (`src/core/flow-definition.cpp`).

Because MXL has no "flow name" lookup, the **UUID is the identity**. The dialog
therefore exposes a video flow UUID (and an audio flow UUID), not just a flow
name. The flow name becomes the NMOS `label` and part of the `grouphint`
(`<name>:Program` etc.). The default UUIDs are deterministic so that an
untouched configuration is stable across sessions; use the **Generate**
buttons when running more than one OBS instance against the same domain.

### 2.1 One output creates two flows

A single MXL flow can only carry one media type. The specification's "one flow
UUID per output" is therefore implemented as:

- **video flow** – the UUID entered by the user;
- **audio flow** – a second UUID, derived as
  `UUIDv5(video_uuid, "/audio")` by default (editable in the dialog, and
  validated to be disjoint from every other flow in the profile).

If audio is disabled, only the video flow is created.

## 3. Video is V210 (10-bit 4:2:2), not UYVY/RGBA

MXL v1.1 supports `video/v210` (and `video/v210a`) video flows. V210 is
10-bit YCbCr 4:2:2 packed in 16-byte groups of 6 pixels; the line/slice stride
is `((width + 47) / 48) * 128` bytes.

### Ingress

OBS 30 added `VIDEO_FORMAT_V210` async-source support. The source therefore
publishes the MXL payload pointer directly, with `linesize[0]` set to the MXL
slice stride, without conversions or copies. This achieves the zero-copy
ingress goal from the specification.

### Egress

OBS gives raw outputs the canvas format, normally `NV12` (8-bit 4:2:0) or
`P010` (10-bit 4:2:0); the preview renderer produces `BGRA`. The plugin
converts these to V210 on the OBS output thread:

- 8-bit samples are shifted to the 10-bit range (`<< 2`);
- P010 samples are shifted down (`>> 6`);
- 4:2:0 chroma is replicated vertically and horizontally (no interpolation);
- BGRA is converted with the BT.709 limited-range matrix and chroma is averaged
  over each pixel pair;
- the last partial 6-pixel group replicates the edge pixel.

The conversion is scalar and runs at several hundred MB/s; a 1080p60 output
uses roughly 6 MB of conversion buffer per output. GPU-side conversion or
`VIDEO_FORMAT_I210` passthrough is left as a future optimisation.

## 4. "Ring buffer depth" is a domain property

The specification shows a per-output ring-depth spin box. In MXL v1.1 the ring
buffer length is derived from the **domain-wide history duration**:

- video: `grainCount = history_ns * grain_rate / 1e9`
- audio: `bufferLength = history_ns * sample_rate / 0.5e9` (twice the history,
  because only half of the ring is readable at a time)

The duration is read from `<domain>/options.json` under
`urn:x-mxl:option:history_duration/v1.0`. It cannot be set per instance or per
flow.

The dialog therefore has one "domain history" field and a checkbox
**"Manage domain history (writes options.json)"**. The plugin only writes that
file when the user opts in, because in a facility the domain options may be
managed by the deployment tooling. When the checkbox is off, the value is
informational.

## 5. Timing details

- `mxlGrainInfo` has no `sync_timestamp` field. The timestamp of a grain is
  derived from its absolute index with the same rounding the SDK uses
  (`indexToTimestamp`), and the reader maps it to the host clock with a
  calibrated `host - TAI` offset.
- The specification's `time_offset_ns` calibration is implemented as
  `ClockOffset` (`include/obsmxl/time.hpp`) and recalibrated once per second
  (ingress) or five seconds (egress), which bounds drift.
- Egress refuses to write an index older than half the flow history instead of
  rewinding `runtime.headIndex`. This is essential: the MXL writer happily
  commits an old index, but doing so would make readers see stale data as the
  new head.
- Egress clamps timestamps more than one frame in the future to `now + 1 frame`
  to avoid a producer with a bad clock dragging the head forward.
- On macOS there is no `CLOCK_TAI`; the core uses `CLOCK_REALTIME + 37 s`.
  Production use is Linux only.

## 6. Blank frames are black, not transparent

V210 has no alpha channel, so the specification's "blank transparent frame
(`alpha = 0x00`)" cannot be represented. After three consecutive missed grains
the source emits a limited-range black V210 frame (Y=64, Cb=Cr=512) at the
flow geometry. This prevents the compositor from freezing on a stale texture
just as effectively; downstream consumers see black and can apply their own
fallback.

## 7. Audio

- OBS provides the master mix as planar float32, which is exactly MXL's
  `audio/float32` layout. The egress path copies channel planes into the MXL
  ring without conversion.
- The flow sample rate and channel count come from OBS (`audio_output_get_*`),
  not from a hard-coded 48 kHz/8 channels. MXL accepts the rational sample
  rate, so 44.1 kHz projects work.
- Preview audio is optional ("Include master audio"); when enabled it publishes
  the program mix, because OBS has no separate preview audio bus.
- Short gaps are filled with silence; gaps longer than one second cause a
  resynchronisation instead of seconds of silence.
- The source truncates audio flows with more than 8 channels to OBS's maximum
  (`MAX_AV_PLANES`), with a speaker layout inferred from the channel count.

## 8. Preview output uses off-screen rendering

The specification calls `obs_frontend_get_rendering_video()`. That function does
not exist in the OBS frontend API. OBS also does not expose its preview monitor
as a `video_t`.

The preview output is implemented like DistroAV's NDI preview:

- a `gs_texrender` renders `obs_frontend_get_current_preview_scene()` (studio
  mode) or the current program scene;
- a `gs_stagesurf` downloads the BGRA result on the graphics thread;
- a private `video_t` feeds the `mxl_output` instance, which converts BGRA to
  V210.

The cost is one synchronous GPU readback per frame while the preview output is
enabled. The preview scene is updated on the relevant frontend events
(`STUDIO_MODE_*`, `PREVIEW_SCENE_CHANGED`, `SCENE_CHANGED`,
`SCENE_COLLECTION_CLEANUP`).

## 9. Output lifecycle and persistence

- Outputs are **not** represented by a dock. `OutputManager` owns the two
  `obs_output_t` objects and keeps them running when the dialog closes.
- Settings live in the active OBS profile under the `obs-mxl` section
  (`obs_frontend_get_profile_config()`), as specified.
- `OBS_FRONTEND_EVENT_FINISHED_LOADING` auto-starts enabled endpoints;
  `OBS_FRONTEND_EVENT_EXIT` stops them.
- Starting an output creates the flows; stopping it releases the writers, which
  makes MXL delete the flow directories. This is MXL behaviour, not a bug.
- The dialog's "Apply" reconciles only what changed: unchanged endpoints keep
  running, changed endpoints are restarted.

## 10. Flow discovery

Sources list flows by scanning `<domain>/*.mxl-flow/flow_def.json`. The MXL C
API has no "enumerate flows" function. The same parser powers the source
dropdown and the fake backend. UUIDs can also be typed/pasted manually.

## 11. Input validation and failure handling

- Domain path must be non-empty; the MXL domain is created on the egress side
  and must exist for readers.
- Flow UUIDs are validated as RFC 4122 strings and must be disjoint across the
  program and preview endpoints (including audio flows).
- An existing flow with the same UUID but different geometry, sample rate or
  channel count is rejected with a clear error rather than silently reusing it.
- Late frames are dropped; overruns resynchronise; a missing flow retries every
  500 ms; a missing domain retries every second.
- Errors are reported through `obs_output_set_last_error` (so OBS logs and the
  UI can show them) and through the core logger.

## 12. Licensing

The plugin is GPL-2.0-or-later, matching OBS Studio's plugin ecosystem. MXL is
Apache-2.0. The MXL SDK is linked dynamically as `libmxl.so`, so the two
licenses remain compatible for distribution. Flow definitions generated by the
plugin carry `$license: Apache-2.0` because they are derived from MXL's own
examples and consumed by the Apache-2.0 SDK.

## 13. Not implemented (deliberate scope)

- Ancillary data (`video/smpte291`) flows.
- Interlaced flows (`interlace_mode` other than `progressive`).
- 12-bit or 4:4:4 video (no MXL media type in v1.1).
- Automatic audio resampling when the OBS mixer rate differs from a facility
  clock; the flow is created at the OBS rate.
- PTP/`phc` clock selection; the plugin relies on the host `CLOCK_TAI` being
  disciplined (chrony/PTP) as stated in the specification.
- Multiple video flows per source (one flow per MXL Source instance).
- GPU-side NV12/P010 → V210 conversion.

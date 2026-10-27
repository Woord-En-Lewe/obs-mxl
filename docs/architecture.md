# Architecture

`obs-mxl` is split into a portable core, two interchangeable MXL backends and a
thin OBS/Qt adapter. This keeps the interesting logic (timing, conversion,
flow handling, failure recovery) testable on macOS without OBS or an MXL
installation.

```
┌────────────────────────────────────────────────────────────┐
│ OBS adapter (src/obs)                                      │
│  MXL Source ──► IngressPipeline ─┐                         │
│  mxl_output ──► EgressPipeline  ─┤                         │
│  Tools dialog ──► OutputManager  │                         │
└──────────────────────────────────┼─────────────────────────┘
                                   │ core interfaces
┌──────────────────────────────────▼─────────────────────────┐
│ obsmxl_core (src/core, include/obsmxl)                     │
│  TAI clock · UUID · flow descriptors · V210 conversion     │
│  ingress/egress state machines · settings · discovery       │
└──────────────────────────────────┬─────────────────────────┘
                                   │ Backend
                 ┌─────────────────┴──────────────────┐
                 │                                    │
       obsmxl_backend_mxl                    fake backend
       (libmxl 1.1, /dev/shm)                (in-process, tests/macOS)
```

## Portable core

| File | Responsibility |
| --- | --- |
| `time.cpp` | `CLOCK_TAI` (Linux) / `CLOCK_REALTIME+37s` fallback, host↔TAI offset calibration, exact index↔timestamp conversion with the same rounding as MXL |
| `uuid.cpp` | RFC 4122 v4 generation and v5 derivation (SHA-1) for audio flow UUIDs |
| `json.cpp` | dependency-free JSON parser/writer used for NMOS flow definitions |
| `flow-definition.cpp` | `video/v210` and `audio/float32` NMOS flow descriptors, V210 stride rules, validation |
| `v210.cpp` | NV12/P010/I420/YV12/BGRA → V210 conversion, black frames, padding rules |
| `ingress.cpp` | playout-delay reader loop, overrun resync, stall blanking, audio catch-up |
| `egress.cpp` | flow creation, TAI grid quantization, late-frame protection, V210 commit |
| `egress-audio.cpp` | sample ring writes, silence fill, batched commits |
| `backend-fake.cpp` | in-memory MXL domain with the same ring-buffer semantics as libmxl |
| `discovery.cpp` | `<uuid>.mxl-flow/flow_def.json` discovery for the source properties |
| `domain-options.cpp` | opt-in write of the MXL domain history option |
| `settings.cpp` | persistent configuration (domain, endpoints, UUIDs, history) |

Everything in the core is plain C++20 with no OBS, Qt or MXL headers. It is
built and unit-tested on macOS and Linux.

## Backends

`Backend` (include/obsmxl/backend.hpp) abstracts the MXL operations the plugin
needs:

- create/open a V210 flow writer + audio writer,
- open readers for a flow id,
- read grains/samples with timeouts,
- expose flow configuration and head index.

`obsmxl_backend_mxl` maps those calls 1:1 to `mxlCreateFlowWriter`,
`mxlFlowWriterOpenGrain`, `mxlFlowReaderGetGrain`, `mxlFlowWriterOpenSamples`,
`mxlFlowReaderGetSamples` and the corresponding runtime/config accessors. It
owns one `mxlInstance` per OBS output/source and releases MXL handles exactly
once, even if the instance is torn down first.

The fake backend implements the same semantics in memory (absolute indices,
strict write ordering, gap invalidation, blocking reads with timeouts, wrapped
audio buffers). It powers the unit tests and the round-trip tests.

## Ingress

`IngressPipeline::poll()` is called from the source's own reader thread:

1. Calibrate `host_time = TAI + offset` (recalibrated once per second).
2. Compute the target grain index from `TAI_now - playout_delay`.
3. Read the grain (`mxlFlowReaderGetGrain`, 25 ms timeout) and pass its
   `/dev/shm` payload pointer to OBS as a `VIDEO_FORMAT_V210`
   `obs_source_frame2` — **zero-copy**, OBS uploads the pixels directly.
4. Pull audio samples ending at the TAI-derived sample index, copy them into a
   small planar float buffer and call `obs_source_output_audio`.
5. Handle `TOO_LATE` by resynchronising to the current head, `TIMEOUT` by
   incrementing the miss counter, and after 3 missed frames emit a black V210
   frame so stale textures do not freeze on screen.

The thread sleeps only until the next grain boundary (capped at 20 ms), never
spinning.

## Egress

`EgressPipeline` owns the video/audio writers for one output:

- The video flow is created from an NMOS `video/v210` descriptor derived from
  the OBS canvas (width, height, FPS, BT.709).
- The audio flow is `audio/float32` using the OBS mixer's sample rate and
  channel count.
- `push_video()` maps the OBS timestamp to TAI, quantizes to the facility
  frame grid, converts NV12/P010/I420/YV12/BGRA to V210 and commits a full
  grain (`validSlices == totalSlices`).
- Frames older than half the flow history are dropped instead of rewinding the
  ring-buffer head; timestamps too far in the future are clamped to
  `now + 1` frame.
- `push_audio()` trims overlaps, inserts silence for short gaps (up to 1 s),
  resynchronises after long stalls and commits in ≤ 480-sample batches.

Both callbacks are bounded and never wait for readers, so the OBS render and
audio threads are never blocked by a slow MXL consumer.

## Preview output

OBS does not expose the frontend preview view as a `video_t`. The preview
output therefore renders the current studio-mode preview scene (or the program
scene when studio mode is off) off-screen:

- a `gs_texrender` renders the scene at canvas resolution from
  `obs_add_main_render_callback`,
- a `gs_stagesurf` downloads the BGRA result,
- a private `video_t` carries the frames to `mxl_output`, which converts
  BGRA→V210.

This costs one GPU readback per frame and is only active while the preview
output is enabled.

## Threading model

| Thread | Work |
| --- | --- |
| OBS graphics thread | preview scene render + staging surface copy |
| OBS output video thread | `push_video` (V210 conversion + MXL commit) |
| OBS output audio thread | `push_audio` (ring copy + commit) |
| OBS source reader thread | `IngressPipeline::poll` + `obs_source_output_*` |
| UI thread | dialog, output manager, flow discovery |

`EgressPipeline` protects the video and audio writers with separate mutexes.
Flow creation/release is serialized by a lifecycle mutex, so stopping an output
never races a callback. The reader thread is the only user of ingress readers.

## Memory

- Ingress video is zero-copy: the MXL grain payload pointer is handed to OBS,
  which copies it into its frame cache during `obs_source_output_video2`.
- Ingress audio is copied into a small per-chunk buffer (bounded by
  `max_audio_catchup_ms`, default 200 ms).
- Egress video uses a single reusable V210 buffer per output.
- The egress ring buffer length is an MXL **domain** property
  (`options.json`, history duration), not a per-output setting.

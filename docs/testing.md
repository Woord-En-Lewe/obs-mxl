# Testing

## Test pyramid

| Layer | What it covers | Requires |
| --- | --- | --- |
| Core unit tests | clock/index math, UUID, JSON, flow descriptors, V210 conversion, settings, discovery | nothing (macOS or Linux) |
| Pipeline tests | ingress/egress state machines against the in-memory fake MXL domain | nothing |
| Round-trip test | egress → fake ring → ingress, byte-identical V210 | nothing |
| MXL integration | real `libmxl` writers/readers in a temporary domain, including a second `mxlInstance` | libmxl + Linux |
| Manual | OBS source/output, preview renderer, dialog | OBS on Linux |

## Running

```bash
# portable core + pipeline tests
cmake --preset macos-core
cmake --build --preset macos-core
ctest --preset macos-core

# Linux, including the MXL integration tests when libmxl is available
./scripts/build-mxl.sh
cmake --preset linux-dev -DCMAKE_PREFIX_PATH="$PWD/.mxl-sdk/prefix"
cmake --build --preset linux-dev
ctest --test-dir build/linux-dev --output-on-failure

# one test
./build/linux-dev/tests/obsmxl_tests "NV12 converts to V210 with 10 bit expansion"
```

The MXL integration tests create their domain under the system temp directory,
never touch `/dev/shm`, and delete it afterwards.

## What the tests assert

- **Timing** – index conversion matches the MXL rounding (`(ts*num +
  0.5e9*den) / (1e9*den)`), `CLOCK_TAI`↔host offset calibration, monotonic
  behaviour.
- **V210** – exact word layout for 6-pixel groups, 10-bit expansion from 8-bit
  and P010, chroma plane selection for I420/YV12, BGRA BT.709 limited range,
  edge replication for widths that are not a multiple of 6, stride padding is
  untouched, black frames use Y=64/C=512.
- **Fake backend** – a grain can only be written once and in order, skipped
  grains are flagged invalid, readers block with a timeout, audio rings wrap.
- **Ingress** – V210 payloads are passed through without copies, host
  timestamps are `TAI - delay + offset`, overruns resynchronise to the head,
  three misses produce a black frame, audio chunks land on the sample grid.
- **Egress** – grains are committed on the TAI grid with full slices, late
  frames are dropped instead of rewinding the head, future timestamps are
  clamped, planar float audio is written contiguously, short gaps become
  silence, conflicting flow geometry is rejected.
- **Real MXL** – egress creates a flow and another `mxlInstance` reads the same
  V210 grain and audio samples; ingress consumes a flow written by egress.

## Sanitizers

```bash
cmake -S . -B build-asan -G Ninja \
  -DOBS_MXL_ENABLE_PLUGIN=OFF -DOBS_MXL_ENABLE_SANITIZERS=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

## Manual OBS checklist

1. Start OBS with a 1080p60 canvas and the plugin installed.
2. **Tools → MXL Output Settings**, enable the program output, Apply.
3. `mxl-info` (MXL SDK tool) should list the `<uuid>.mxl-flow` directory and
   show a growing head index with 10-bit V210 grains.
4. Add an **MXL Source**, pick the flow from the dropdown, and confirm the
   picture appears with no tearing and no growing latency.
5. Enable the preview output in studio mode; confirm that the preview scene
   (not the program scene) is published.
6. Stop the output; the flow is deleted by MXL. Close OBS; no crash on exit and
   no stale flows.
7. Kill the upstream writer while the source is active; after ~50 ms the source
   should show black rather than freeze on the last frame.

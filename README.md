# obs-mxl

Unified **EBU MXL** (Media eXchange Layer) ingress and egress for OBS Studio.

`obs-mxl` turns OBS into an uncompressed media function in a Dynamic Media
Facility:

- **Ingress** – `MXL Source`, a regular OBS source that reads `video/v210`
  grains (and an optional `audio/float32` flow) directly from
  `/dev/shm/mxl` and feeds them into the compositor.
- **Egress** – `mxl_output`, an unencoded output that publishes the program
  canvas (and optionally the studio-mode preview scene) as MXL flows for
  downstream consumers such as FFmpeg, multiviewers, AI framing or signage.
- **Configuration** – no dock widget; everything lives under
  **Tools → MXL Output Settings** and is persisted in the active OBS profile.

The plugin is a single `obs-mxl.so` module that registers one `obs_source_info`
and one `obs_output_info`.

```
OBS ── MXL Source ──►  /dev/shm/mxl/<camera>.mxl-flow  ──► compositor
OBS ── mxl_output ──►  /dev/shm/mxl/<obs-pgm>.mxl-flow ──► consumers
```

## Status

The portable core, both backends (libmxl and an in-memory fake), the OBS
adapter and the Qt dialog are implemented. See
[docs/implementation-notes.md](docs/implementation-notes.md) for the places
where this implementation deliberately differs from the research specification
(mostly because the real MXL v1.1 C API is different from the pseudo-code in
the document).

## Requirements

| Component | Version |
| --- | --- |
| OBS Studio / libobs | 30.x or newer (V210 async sources are required) |
| MXL SDK (`libmxl`) | 1.1.x |
| Qt (for the dialog) | 6.2+ |
| Compiler | C++20 (GCC 13+, Clang 17+) |
| OS | Linux (production), macOS (development / core) |

`libmxl` is a runtime dependency: the plugin links `libmxl.so` dynamically.
Install it in a standard loader path (for example `/usr/local/lib` plus
`ldconfig`) or set `LD_LIBRARY_PATH` to the MXL prefix when launching OBS. See
[docs/building.md](docs/building.md#runtime-dependencies).

`clock_gettime(CLOCK_TAI)` is used on Linux. macOS has no TAI clock; the core
falls back to `CLOCK_REALTIME + 37 s` and is intended for development only.

## Quick start (Linux)

```bash
# 1. Build and install libmxl (vcpkg based, cached under .mxl-sdk/)
./scripts/build-mxl.sh

# 2. Build the plugin and run the tests
cmake --preset linux-dev \
  -DCMAKE_PREFIX_PATH="$PWD/.mxl-sdk/prefix"
cmake --build --preset linux-dev
ctest --test-dir build/linux-dev --output-on-failure

# 3. Install for the current user
cmake --install build/linux-dev --prefix "$HOME/.local"
```

See [docs/building.md](docs/building.md) for distribution packages, container
builds and the macOS workflow.

## Using the plugin

1. Install `libmxl` in a loader path (see
   [runtime dependencies](docs/building.md#runtime-dependencies)).
2. Start OBS and open **Tools → MXL Output Settings**.
3. Enable the program and/or preview endpoint, optionally generate flow UUIDs,
   and press **Apply**. The outputs keep running when the dialog is closed.
4. Add an **MXL Source** from the Sources dock. The flow dropdown lists the
   `*.mxl-flow` directories found in the domain; select a video flow and,
   optionally, an audio flow. Set the playout delay if the upstream producer
   jitters.
5. Press **Apply**; the source publishes V210 frames and float32 audio on the
   TAI timeline.

Without the Qt dialog (headless/scripted setups), the same configuration is
stored in the active OBS profile under the `obs-mxl` section and the outputs
can be created with `obs_output_create("mxl_output", …)`.

## Repository layout

```
include/obsmxl/   portable core API (no OBS, Qt or MXL headers)
src/core/         timing, UUID/JSON, flow descriptors, V210, pipelines, fake backend
src/mxl/          libmxl backend
src/obs/          OBS source/output adapters, output manager, preview renderer
src/ui/           Qt 6 Tools dialog
tests/            Catch2 unit, pipeline, round-trip and MXL integration tests
ci/               Dagger (Go) CI module and development container
docs/             architecture, building, testing, CI, specification deltas
```

## Quick start (macOS, core only)

The portable core and its test suite build without OBS, Qt or MXL:

```bash
cmake --preset macos-core
cmake --build --preset macos-core
ctest --preset macos-core
```

A full plugin build on macOS requires libobs headers and libmxl; the supported
way to do that is the Linux development container (`./scripts/dev-container.sh`)
or a macOS OBS source tree. See [docs/building.md](docs/building.md).

## CI

CI is a [Dagger](https://dagger.io) module written in Go:

```bash
dagger call ci --source=.
dagger call build --source=. export --path=build/obs-mxl.so
dagger call test --source=.
dagger call package --source=. export --path=build/obs-mxl.tar.gz
```

The Linux pipeline builds libmxl 1.1.0 from source inside Ubuntu 24.04
(cached by Dagger), builds the plugin against OBS 30 headers, runs the unit and
MXL integration tests, checks formatting and produces a relocatable tarball.
A GitHub Actions workflow additionally runs the portable test suite on macOS.

## Documentation

- [Architecture](docs/architecture.md)
- [Building](docs/building.md)
- [Testing](docs/testing.md)
- [CI](docs/ci.md)
- [Implementation notes and specification deltas](docs/implementation-notes.md)
- [Original research specification](docs/research/unified_obs_mxl_plugin_specification.md)

## License

GPL-2.0-or-later. The MXL SDK is Apache-2.0; see `LICENSE` and
[docs/implementation-notes.md](docs/implementation-notes.md#12-licensing) for
details.

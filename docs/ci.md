# CI

CI lives in a [Dagger](https://dagger.io) module written in Go (`dagger.json`
at the repository root, sources under `ci/`). Dagger runs the Linux build in
Ubuntu 24.04 with OBS 30 headers and builds MXL 1.1.0 from source, so no
developer-specific toolchain leaks into the pipeline.

## Requirements

- Docker (or another Dagger-supported container runtime)
- Dagger CLI 0.18.x

```bash
curl -fsSL https://dl.dagger.io/dagger/install.sh | \
  DAGGER_VERSION=0.18.3 sh
```

## Functions

| Function | Description |
| --- | --- |
| `build` | Builds `obs-mxl.so` against OBS 30 + MXL 1.1 and returns the module |
| `test` | Builds and runs the full test suite (unit + MXL integration) |
| `lint` | `clang-format --dry-run --Werror` over the C++ sources |
| `package` | Returns a tarball with the module, locale, README and install notes |
| `ci` | `lint` + `test` + `build` |

Examples:

```bash
# Everything
dagger call ci --source=.

# Just the module
dagger call build --source=. export --path=build/obs-mxl.so

# Only the portable core (no OBS, no MXL) on a host without Docker
cmake --preset macos-core && cmake --build --preset macos-core
ctest --preset macos-core
```

## Caching

The MXL build is the expensive step. The module mounts a Dagger cache volume
for the compiled vcpkg binary packages (`obs-mxl-vcpkg-archives`).

MXL itself is rebuilt (about a minute) because its source is cloned inside the
container. Dagger's exec cache short-circuits the chain when neither the
source tree, the MXL version nor the base image changed.

The **first** run in a fresh environment compiles the vcpkg dependency tree
(including libpcap and ada-url) and takes 20–30 minutes. Subsequent runs reuse
the Dagger cache and the `obs-mxl-vcpkg-archives` volume.

## GitHub Actions

`.github/workflows/ci.yml` has two jobs:

- **linux** – installs Dagger and runs `dagger call ci --source=.`
- **macos-core** – builds the portable core and test suite with the
  `macos-core` preset (no OBS, Qt or MXL dependencies).

The release workflow can publish the `package` artifact; the tarball layout is:

```
obs-mxl.so
locale/en-US.ini
README.md
LICENSE
```

The Dagger Go bindings (`ci/dagger.gen.go`, `ci/internal/`) are generated and
intentionally not committed. `dagger call` regenerates them automatically; run
`dagger develop` if you want to `go build` inside `ci/` directly.

## Local container without Dagger

`ci/Dockerfile` mirrors the Dagger environment and is used by
`scripts/dev-container.sh` for interactive development:

```bash
./scripts/dev-container.sh
```

It caches the vcpkg/MXL build in the image, so subsequent starts are fast.

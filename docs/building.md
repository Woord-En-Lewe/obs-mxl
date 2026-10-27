# Building

## Linux (production target)

### Distribution packages

Ubuntu 24.04 / Debian trixie and newer provide everything except the MXL SDK:

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  bison flex libobs-dev qt6-base-dev qt6-base-dev-tools \
  curl zip unzip tar
```

`libobs-dev` 30.0.2 (Ubuntu 24.04) is the oldest tested version. Newer OBS
releases work as well; the plugin only needs the V210 async-source support that
was added in OBS 30.

### libmxl 1.1

The MXL SDK is not packaged by distributions yet. The helper script builds it
from source with CMake + vcpkg into a local prefix:

```bash
./scripts/build-mxl.sh
```

It performs the following (all cached under `.mxl-sdk/`):

```bash
git clone --branch v1.1.0 https://github.com/dmf-mxl/mxl
git clone https://github.com/microsoft/vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake -S mxl -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake \
  -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
  -DCMAKE_INSTALL_PREFIX=.mxl-sdk/prefix
cmake --build build
cmake --install build
```

The first run downloads and compiles the vcpkg dependencies (fmt, spdlog,
stduuid, picojson, ada-url); it can take 15–30 minutes. Later runs only rebuild
MXL itself.

If your facility already ships `libmxl`, point CMake at it instead:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/opt/mxl \
  -DOBS_MXL_ENABLE_PLUGIN=ON
```

### Plugin

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$PWD/.mxl-sdk/prefix"

cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure```

The module is `build/obs-mxl.so`.

Useful options:

| Option | Default | Meaning |
| --- | --- | --- |
| `OBS_MXL_ENABLE_PLUGIN` | `ON` | Build the OBS module (requires libobs + libmxl) |
| `OBS_MXL_ENABLE_UI` | `ON` | Build the Qt dialog (requires Qt 6 Widgets) |
| `OBS_MXL_BUILD_TESTS` | `ON` | Build the Catch2 test suite |
| `OBS_MXL_WARNINGS_AS_ERRORS` | `OFF` | Turn warnings into errors (CI) |
| `OBS_MXL_ENABLE_SANITIZERS` | `OFF` | ASan + UBSan |

### Installing

```bash
cmake --install build --prefix "$HOME/.local"
```

Installs:

```
$prefix/lib/obs-plugins/obs-mxl.so
$prefix/share/obs/obs-plugins/obs-mxl/locale/en-US.ini
```

On Debian/Ubuntu OBS looks in `/usr/lib/x86_64-linux-gnu/obs-plugins` and
`/usr/share/obs/obs-plugins`. Override the destinations if needed:

```bash
cmake --install build --prefix /usr \
  -DOBS_PLUGIN_DESTINATION=lib/x86_64-linux-gnu/obs-plugins
```

For a per-user install, copy `obs-mxl.so` to
`~/.config/obs-studio/plugins/obs-mxl/bin/64bit/` and the locale directory to
`~/.config/obs-studio/plugins/obs-mxl/data/`.

### Runtime dependencies

The module is linked against `libmxl.so`. When MXL is installed outside the
default loader path, make it discoverable before starting OBS:

```bash
# option 1: system install
sudo cmake --install .mxl-sdk/build --prefix /usr/local
sudo ldconfig

# option 2: per-session
export LD_LIBRARY_PATH="$PWD/.mxl-sdk/prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
obs
```

Check with `ldd /usr/lib/obs-plugins/obs-mxl.so | grep mxl`.

## macOS (development target)

The portable core, the fake backend and the whole unit-test suite build with
the Xcode command line tools only:

```bash
cmake --preset macos-core
cmake --build --preset macos-core
ctest --preset macos-core
```

If `ctest` is not on your `PATH` (for example when using the CMake.app
bundle), use `"$(dirname "$(command -v cmake)")/ctest"` or the
`scripts/test-core.sh` helper, which locates it automatically.

This is the recommended inner loop: it exercises timing conversion, V210
conversion, flow descriptors, ingress/egress state machines and the fake MXL
ring buffers.

A **full plugin** build on macOS needs `libobs` headers and `libmxl`. Neither is
available from Homebrew in a form that matches a stock OBS.app. Two options:

1. **Linux dev container** (recommended):

   ```bash
   ./scripts/dev-container.sh
   # inside the container
   cmake --preset linux-dev
   cmake --build --preset linux-dev
   ctest --preset linux-ci
   ```

2. Build OBS from source and pass its build tree:

   ```bash
   cmake -S . -B build-macos -G Ninja \
     -Dlibobs_DIR="$HOME/obs-studio/build/libobs" \
     -Dobs-frontend-api_DIR="$HOME/obs-studio/build/UI/obs-frontend-api" \
     -DCMAKE_PREFIX_PATH="$HOME/mxl-install" \
     -DOBS_MXL_ENABLE_PLUGIN=ON
   ```

MXL itself builds on macOS (see the MXL `docs/Building.md`), but the plugin's
production target is Linux. The OBS adapter uses macOS-compatible APIs; the
main functional difference is that `CLOCK_TAI` is unavailable, so the core uses
`CLOCK_REALTIME + 37 s`.

## Troubleshooting

- **`Could NOT find mxl`** – set `CMAKE_PREFIX_PATH` to the MXL install prefix,
  or run `./scripts/build-mxl.sh`.
- **`libobs was not found`** – install `libobs-dev`, or configure with
  `-DOBS_MXL_ENABLE_PLUGIN=OFF` to build only the core and tests.
- **Qt dialog missing** – `OBS_MXL_ENABLE_UI=ON` requires `qt6-base-dev`.
- **`mxlCreateFlowWriter failed` at runtime** – another process still owns the
  flow UUID, or the flow exists with a different geometry. Use the dialog to
  generate a fresh UUID.
- **Flows disappear when OBS exits** – expected: MXL deletes a flow when its
  last writer is released.

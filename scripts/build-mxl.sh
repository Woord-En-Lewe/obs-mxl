#!/usr/bin/env bash
# Builds libmxl (v1.1.0 by default) into .mxl-sdk/prefix using vcpkg.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${root}/.mxl-sdk"
version="${MXL_VERSION:-v1.1.0}"
prefix="${sdk}/prefix"

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake is required" >&2
  exit 1
fi
if ! command -v git >/dev/null 2>&1; then
  echo "git is required" >&2
  exit 1
fi
if ! command -v bison >/dev/null 2>&1 || ! command -v flex >/dev/null 2>&1; then
  echo "bison and flex are required by MXL's vcpkg dependencies (libpcap)" >&2
  echo "  sudo apt install bison flex m4" >&2
  exit 1
fi
for tool in curl zip unzip tar; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "${tool} is required to bootstrap vcpkg (sudo apt install ${tool})" >&2
    exit 1
  fi
done

generator="Ninja"
if ! command -v ninja >/dev/null 2>&1; then
  generator="Unix Makefiles"
fi

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

mkdir -p "${sdk}"

if [ ! -d "${sdk}/src/.git" ]; then
  echo "==> cloning MXL ${version}"
  git clone --depth 1 --branch "${version}" https://github.com/dmf-mxl/mxl "${sdk}/src"
else
  echo "==> reusing ${sdk}/src"
fi

if [ ! -x "${sdk}/vcpkg/vcpkg" ]; then
  echo "==> bootstrapping vcpkg"
  if [ ! -d "${sdk}/vcpkg/.git" ]; then
    git clone https://github.com/microsoft/vcpkg "${sdk}/vcpkg"
  fi
  "${sdk}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

export VCPKG_ROOT="${sdk}/vcpkg"

echo "==> configuring MXL"
cmake -S "${sdk}/src" -B "${sdk}/build" -G "${generator}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DBUILD_TESTS=OFF \
  -DBUILD_TOOLS=OFF \
  -DBUILD_UTILS=OFF \
  -DBUILD_DOCS=OFF \
  -DMXL_ENABLE_FABRICS_OFI=OFF \
  -DCMAKE_INSTALL_PREFIX="${prefix}"

echo "==> building MXL"
cmake --build "${sdk}/build" -j"${jobs}"

echo "==> installing MXL to ${prefix}"
cmake --install "${sdk}/build"

cat <<EOF

Done.

Configure the plugin with:
  cmake --preset linux-dev -DCMAKE_PREFIX_PATH="${prefix}"
EOF

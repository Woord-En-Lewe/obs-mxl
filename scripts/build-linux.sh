#!/usr/bin/env bash
# Configure, build and test the Linux plugin. Assumes libobs and Qt are
# installed system-wide and libmxl lives in .mxl-sdk/prefix (scripts/build-mxl.sh).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${MXL_PREFIX:-${root}/.mxl-sdk/prefix}"

if [ ! -d "${prefix}" ]; then
  echo "MXL SDK not found at ${prefix}; run scripts/build-mxl.sh first" >&2
  exit 1
fi

cmake --preset linux-dev -DCMAKE_PREFIX_PATH="${prefix}"
cmake --build --preset linux-dev

ctest_bin="${CTEST:-$(command -v ctest || true)}"
if [ -z "${ctest_bin}" ] || [ ! -x "${ctest_bin}" ]; then
  for candidate in \
    "$(dirname "$(command -v cmake)")/ctest" \
    /Applications/CMake.app/Contents/bin/ctest \
    /usr/local/bin/ctest \
    /usr/bin/ctest; do
    if [ -x "${candidate}" ]; then
      ctest_bin="${candidate}"
      break
    fi
  done
fi
"${ctest_bin}" --test-dir build/linux-dev --output-on-failure

echo
echo "Plugin: ${root}/build/linux-dev/obs-mxl.so"

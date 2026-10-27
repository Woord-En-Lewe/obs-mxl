#!/usr/bin/env bash
# Runs the portable core test suite (works on macOS and Linux).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

preset="macos-core"
if [ "$(uname -s)" = "Linux" ]; then
  preset="linux-core"
fi

cmake --preset "${preset}"
cmake --build --preset "${preset}"

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
"${ctest_bin}" --preset "${preset}" --output-on-failure

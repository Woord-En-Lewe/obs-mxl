#!/usr/bin/env bash
# Formats all C++ sources with clang-format.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
clang_format="${CLANG_FORMAT:-clang-format}"

if ! command -v "${clang_format}" >/dev/null 2>&1; then
  echo "clang-format not found (set CLANG_FORMAT)" >&2
  exit 1
fi

cd "${root}"
find include src tests -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 \
  | xargs -0 "${clang_format}" -i

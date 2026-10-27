#!/usr/bin/env bash
# Builds the reusable Linux development image and opens a shell in it with the
# repository bind-mounted. Everything is cached in the Docker image, so the
# vcpkg/MXL build only happens when ci/Dockerfile changes.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="${OBS_MXL_DEV_IMAGE:-obs-mxl-dev:latest}"

docker build -t "${image}" -f "${root}/ci/Dockerfile" "${root}/ci"

exec docker run --rm -it \
  -v "${root}:/src/obs-mxl" \
  -w /src/obs-mxl \
  "${image}" "$@"

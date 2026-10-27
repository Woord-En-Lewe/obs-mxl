FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      bison \
      build-essential \
      ca-certificates \
      cmake \
      curl \
      flex \
      git \
      libgl1-mesa-dev \
      libobs-dev \
      m4 \
      ninja-build \
      obs-studio \
      pkg-config \
      qt6-base-dev \
      qt6-base-dev-tools \
      libqt6svg6-dev \
      tar \
      unzip \
      zip \
    && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

RUN git clone --depth 1 --branch v1.1.0 https://github.com/dmf-mxl/mxl /src/mxl

RUN cmake -S /src/mxl -B /src/mxl/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DBUILD_TESTS=OFF \
      -DBUILD_TOOLS=OFF \
      -DBUILD_UTILS=OFF \
      -DBUILD_DOCS=OFF \
      -DMXL_ENABLE_FABRICS_OFI=OFF \
      -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /src/mxl/build -j"$(nproc)" \
    && cmake --install /src/mxl/build

ENV CMAKE_PREFIX_PATH=/opt/mxl

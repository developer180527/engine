# Linux build environment for the engine.
#
# The engine has never been compiled on Linux. This image exists so that can
# be checked in seconds from a Mac, without waiting on CI or owning a Linux
# box. The package list is kept in sync with .github/workflows/ci.yml — if a
# dependency is added there, add it here too.
#
# Usage (from the repo root):
#   docker build -f docker/linux-build.Dockerfile -t engine-linux .
#   docker run --rm -it -v "$PWD":/src -w /src engine-linux
#
# The source is bind-mounted rather than COPYed: the tree is tens of GB with
# build artifacts, and mounting keeps builds incremental across runs. Build
# into build-linux/ so the host's macOS build/ tree is never touched.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        cmake \
        ninja-build \
        pkg-config \
        git \
        ccache \
        # GLFW: X11 + Wayland + xkbcommon. xorg-dev is the umbrella that
        # pulls the Xrandr/Xinerama/Xcursor/Xi headers GLFW probes for.
        xorg-dev \
        libx11-dev \
        libgl1-mesa-dev \
        libwayland-dev \
        libxkbcommon-dev \
        wayland-protocols \
        # miniaudio
        libasound2-dev \
        libpulse-dev \
        # asset registry (macOS ships this in the SDK; Linux does not)
        libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

# git refuses to operate on a tree owned by another uid, which is exactly what
# a bind-mounted host checkout looks like from inside the container.
RUN git config --global --add safe.directory '*'

ENV CCACHE_DIR=/src/.ccache-linux

WORKDIR /src
CMD ["/bin/bash"]

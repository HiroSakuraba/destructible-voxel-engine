# Linux CI build image: the GHCR trixie mirror plus everything ci.yml's DVE_APT_PACKAGES lists.
# Keep the package list in sync with DVE_APT_PACKAGES in .github/workflows/ci.yml (ci.yml falls back
# to apt-get when a package is missing, so drift only costs time). Built by ci-image-mirror.yml.
ARG BASE=ghcr.io/hirosakuraba/dve-ci-debian:trixie
FROM ${BASE}
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    ccache \
    git \
    python3 \
    pkg-config \
    file \
    xz-utils \
    zstd \
    ca-certificates \
    dpkg-dev \
    libpng-dev \
    libjpeg-dev \
    liblua5.4-dev \
    libsdl3-dev \
    libsdl3-ttf-dev \
    libx11-dev \
    libxext-dev \
    libxft-dev \
    libfreetype-dev \
    libfontconfig-dev \
    libwayland-dev \
    libxkbcommon-dev \
    libasound2-dev \
    libpulse-dev \
    libjack-dev \
    libflac-dev \
    libogg-dev \
    libvorbis-dev \
    libopus-dev \
    ffmpeg \
    glslang-tools \
    xvfb \
    xauth \
    mold \
 && rm -rf /var/lib/apt/lists/*

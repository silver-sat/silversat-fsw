# Slim Zephyr development image for SilverSat.
#
# The upstream ghcr.io/zephyrproject-rtos/ci image carries toolchains for
# every architecture Zephyr supports and unpacks to well over 20 GB, which
# does not fit a default Codespace's 32 GB disk. This image installs only:
#
#   * host gcc + 32-bit multilib   -> native_sim (both variants)
#   * arm-zephyr-eabi              -> nucleo_f446re
#
# Result is a few GB rather than a few dozen.

FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Must match the SDK the CI workflow uses, or a build here can succeed and
# the same build in CI can fail. Check the "SDK_VERSION" line in an Actions
# log when bumping this.
ARG ZSDK_VERSION=1.0.1

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ccache \
        cmake \
        device-tree-compiler \
        dfu-util \
        file \
        g++-multilib \
        gcc \
        gcc-multilib \
        gperf \
        libmagic1 \
        libsdl2-dev \
        make \
        ninja-build \
        python3-dev \
        python3-pip \
        python3-tk \
        python3-venv \
        wget \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Zephyr SDK: the "minimal" bundle plus one toolchain, rather than the full
# bundle which pulls all architectures.
RUN mkdir -p /opt/toolchains \
 && cd /opt/toolchains \
 && wget "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZSDK_VERSION}/zephyr-sdk-${ZSDK_VERSION}_linux-x86_64_minimal.tar.xz" \
 && tar xf "zephyr-sdk-${ZSDK_VERSION}_linux-x86_64_minimal.tar.xz" \
 && rm "zephyr-sdk-${ZSDK_VERSION}_linux-x86_64_minimal.tar.xz" \
 && wget "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZSDK_VERSION}/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz" \
 && tar xf "toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz" -C "zephyr-sdk-${ZSDK_VERSION}/" \
 && rm "toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz" \
 && "zephyr-sdk-${ZSDK_VERSION}/setup.sh" -h -c

ENV ZEPHYR_SDK_INSTALL_DIR=/opt/toolchains/zephyr-sdk-${ZSDK_VERSION}

# Ubuntu 24.04 marks its Python as externally managed; this container is
# single-purpose, so installing into the system interpreter is fine.
RUN pip3 install --break-system-packages --no-cache-dir west

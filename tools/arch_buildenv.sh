#!/bin/bash
# This script works with Arch Linux.
# shellcheck disable=SC1091

# Fail if not executed with bash
if [ -z "$BASH_VERSION" ]; then
    echo "Error: This script must be called as executable: ./arch_buildenv.sh setup" >&2
    exit 1
fi

set -o pipefail

case "$1" in
    name)
        echo "No build environment name required for Arch Linux." >&2
        echo "This script installs the build dependencies via pacman using the \"setup\" option." >&2
        ;;

    setup)
        yay -Syu --needed \
            benchmark \
            ccache \
            chromaprint \
            clang \
            clazy \
            cmake \
            faad2 \
            ffmpeg \
            fftw \
            flac \
            gcc \
            gtest \
            hidapi \
            lame \
            lcov \
            libebur128 \
            libid3tag \
            libmad \
            libmodplug \
            libshout \
            libsndfile \
            libusb \
            lilv \
            lv2 \
            mesa \
            mold \
            microsoft-gsl \
            ninja \
            openssl \
            opus \
            opusfile \
            portaudio \
            portmidi \
            protobuf \
            qt6-5compat \
            qt6-base \
            qt6-declarative \
            qt6-shadertools \
            qt6-svg \
            qtkeychain-qt6 \
            rubberband \
            soundtouch \
            sqlite \
            systemd \
            taglib \
            ttf-opensans \
            ttf-ubuntu-font-family \
            upower \
            wavpack \
            microsoft-gsl
        ;;
    *)
        echo "Usage: $0 [options]"
        echo ""
        echo "options:"
        echo "   help       Displays this help."
        echo "   name       Displays the name of the required build environment."
        echo "   setup      Installs the build environment."
        ;;
esac

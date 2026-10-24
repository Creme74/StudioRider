# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioWire authors
#
# Shared by build.sh and package.sh: tool checks and third-party downloads.
# Meant to be sourced from the project's root folder.

JUCE_TAG="9.0.4"
CIVETWEB_TAG="v1.16"
FLAC_TAG="1.5.0"
OPUS_TAG="v1.5.2"
SPEEXDSP_TAG="SpeexDSP-1.2.1"

# Oldest macOS supported: cloudflared (the public link) needs 13 (see CMakeLists.txt).
MIN_MACOS="13.0"

say() { printf "\n\033[1;38;5;208m==>\033[0m \033[1m%s\033[0m\n" "$*"; }
die() { printf "\n\033[1;31mError:\033[0m %s\n" "$*" >&2; exit 1; }

studiowire_version() {
    sed -n 's/^project(StudioWire VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt
}

check_tools() {
    [[ "$(uname)" == "Darwin" ]] || die "This script is meant for macOS."

    say "Checking tools"
    if ! xcode-select -p >/dev/null 2>&1; then
        echo "Apple's developer tools (Command Line Tools) are required."
        xcode-select --install || true
        die "Install them in the window that just opened, then run this script again."
    fi
    echo "Apple tools: $(xcode-select -p)"

    # Homebrew may be installed in a personal folder.
    for p in /opt/homebrew/bin /usr/local/bin "$HOME/.homebrew/bin" "$HOME/homebrew/bin"; do
        [[ -d "$p" ]] && PATH="$PATH:$p"
    done
    export PATH

    if ! command -v cmake >/dev/null 2>&1; then
        if command -v brew >/dev/null 2>&1; then
            say "Installing CMake (Homebrew)"
            brew install cmake
        else
            say "Installing CMake (Python, into .tools/)"
            python3 -m venv .tools/venv
            .tools/venv/bin/pip install -q cmake
            PATH="$PWD/.tools/venv/bin:$PATH"
        fi
    fi
    echo "CMake: $(cmake --version | head -1)"
}

fetch() { # fetch <GitHub repo> <tag> <folder>
    say "Downloading $1 $2 (from GitHub)"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$2" "https://github.com/$1.git" "$3" 2>&1 \
        | grep -v "is not a commit" || true
    [[ -d "$3" ]] || die "Could not download $1."
    rm -rf "$3/.git"
}

fetch_dependencies() {
    if [[ ! -f third_party/civetweb/src/civetweb.c ]]; then
        local tmp; tmp="$(mktemp -d)"
        fetch civetweb/civetweb "$CIVETWEB_TAG" "$tmp/civetweb"
        mkdir -p third_party/civetweb/src third_party/civetweb/include
        for f in civetweb.c handle_form.inl http2.inl match.inl md5.inl response.inl sha1.inl sort.inl timer.inl; do
            cp "$tmp/civetweb/src/$f" third_party/civetweb/src/
        done
        cp "$tmp/civetweb/include/civetweb.h" third_party/civetweb/include/
        cp "$tmp/civetweb/LICENSE.md" third_party/civetweb/
        rm -rf "$tmp"
    fi
    [[ -f third_party/flac/CMakeLists.txt ]] || fetch xiph/flac "$FLAC_TAG" third_party/flac
    [[ -f third_party/opus/CMakeLists.txt ]] || fetch xiph/opus "$OPUS_TAG" third_party/opus
    [[ -f third_party/opus/package_version ]] || echo "PACKAGE_VERSION=\"${OPUS_TAG#v}\"" > third_party/opus/package_version
    if [[ ! -f third_party/speexdsp/resample.c ]]; then
        local tmp; tmp="$(mktemp -d)"
        fetch xiph/speexdsp "$SPEEXDSP_TAG" "$tmp/speexdsp"
        mkdir -p third_party/speexdsp
        cp "$tmp/speexdsp/libspeexdsp/resample.c" "$tmp/speexdsp/libspeexdsp/arch.h" \
           "$tmp/speexdsp/include/speex/speex_resampler.h" "$tmp/speexdsp/COPYING" third_party/speexdsp/
        rm -rf "$tmp"
    fi
    [[ -f JUCE/CMakeLists.txt ]] || fetch juce-framework/JUCE "$JUCE_TAG" JUCE
}

# cmake_build <build folder> <architectures> [extra cmake arguments…]
cmake_build() {
    local dir="$1" archs="$2"; shift 2
    if [[ -f "$dir/CMakeCache.txt" ]] && ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$PWD" "$dir/CMakeCache.txt"; then
        say "Clearing a build folder made elsewhere"   # e.g. after the folder was renamed
        rm -rf "$dir"
    fi

    say "Configuring (macOS $(sw_vers -productVersion), $archs)"
    cmake -S . -B "$dir" -G "Unix Makefiles" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_OSX_ARCHITECTURES="$archs" \
          -DJUCE_DIR="$PWD/JUCE" "$@" >/dev/null

    say "Building (a few minutes the first time)"
    set +e
    cmake --build "$dir" --config Release -j"$(sysctl -n hw.ncpu)" 2>&1 | tee "$dir.log" | grep --line-buffered -E "Built target|error:"
    local status=${PIPESTATUS[0]}
    set -e
    [[ $status -eq 0 ]] || die "The build failed (full log: $dir.log)."
}

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v pkg-config >/dev/null 2>&1 || \
   ! pkg-config --exists x11 xft fontconfig; then
    echo "X11/Xft/fontconfig development packages are required." >&2
    echo "Ubuntu/Debian: sudo apt install build-essential libx11-dev libxft-dev libfontconfig1-dev" >&2
    echo "Fedora:        sudo dnf install gcc-c++ libX11-devel libXft-devel fontconfig-devel" >&2
    exit 1
fi

compiler_major() {
    "$1" -dumpversion 2>/dev/null | cut -d. -f1
}

if [[ -z "${CXX:-}" ]]; then
    if command -v g++ >/dev/null 2>&1 && \
       [[ "$(compiler_major g++)" -ge 8 ]]; then
        CXX=g++
    else
        for candidate in g++-12 g++-11 g++-10 g++-9 g++-8; do
            if command -v "$candidate" >/dev/null 2>&1; then
                CXX="$candidate"
                break
            fi
        done
    fi
fi

if [[ -z "${CXX:-}" ]]; then
    echo "A C++17 compiler is required (GCC 8+ or Clang 7+)." >&2
    exit 1
fi

export CXX
echo "Using compiler: $CXX"
exec make "$@"

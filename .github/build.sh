#!/usr/bin/env bash
# Builds a portable Linux release tarball. Run it as root in a fresh ubuntu:22.04 container, from the repo root.
# Usage: .github/build.sh [OUTPUT_DIR]
set -euo pipefail

GLFW_VERSION=3.4
CJSON_VERSION=1.7.19
deps=/opt/deps
out=${1:-dist}
mkdir -p "$out"
out=$(cd "$out" && pwd)
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates curl make gcc libc6-dev binutils cmake pkg-config python3 \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev \
    libwayland-dev wayland-protocols libwayland-bin libgl-dev libegl-dev libegl-mesa0 libgl1-mesa-dri

# Paintboard needs C23 #embed. Jammy's own compilers are too old.
. /etc/os-release
install -d /etc/apt/keyrings
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/keyrings/llvm.asc
echo "deb [signed-by=/etc/apt/keyrings/llvm.asc] https://apt.llvm.org/$VERSION_CODENAME/ llvm-toolchain-$VERSION_CODENAME-19 main" \
    > /etc/apt/sources.list.d/llvm.list
apt-get update
apt-get install -y --no-install-recommends clang-19
export CC=clang-19

# Static GLFW and cJSON keep libglfw and libcjson out of the runtime dependencies.
# GLFW loads the X11 and Wayland libraries with dlopen at run time.
src=$(mktemp -d)
curl -fsSL "https://github.com/glfw/glfw/archive/refs/tags/$GLFW_VERSION.tar.gz" | tar -xz -C "$src"
curl -fsSL "https://github.com/DaveGamble/cJSON/archive/refs/tags/v$CJSON_VERSION.tar.gz" | tar -xz -C "$src"
cmake -S "$src/glfw-$GLFW_VERSION" -B "$src/glfw-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DGLFW_BUILD_X11=ON -DGLFW_BUILD_WAYLAND=ON \
    -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF
cmake --build "$src/glfw-build" -j"$(nproc)"
cmake --install "$src/glfw-build"
cmake -S "$src/cJSON-$CJSON_VERSION" -B "$src/cjson-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF -DENABLE_CJSON_UTILS=OFF
cmake --build "$src/cjson-build" -j"$(nproc)"
cmake --install "$src/cjson-build"
export PKG_CONFIG_PATH="$deps/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

make CC=clang-19 PYTHON=python3
make test PYTHON=python3
strip paintboard

needed=$(readelf -d paintboard | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
echo "NEEDED: ${needed//$'\n'/ }"
if unexpected=$(grep -Ev '^(libGL\.so\.1|libEGL\.so\.1|libm\.so\.6|libc\.so\.6|ld-linux.*)$' <<<"$needed"); then
    echo "paintboard links unexpected shared libraries: ${unexpected//$'\n'/ }" >&2
    exit 1
fi
echo "Highest glibc symbol version: $(objdump -T paintboard | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -n1)"

version=$(./paintboard --version | awk '{print $2}')
name=paintboard-$version-linux-$(uname -m)
stage=$(mktemp -d)
make install CC=clang-19 PYTHON=python3 DESTDIR="$stage/$name" PREFIX=
install -Dm644 -t "$stage/$name/share/doc/paintboard" LICENSE vendor/NOTICE vendor/OFL.txt
tar -C "$stage" --owner=0 --group=0 --numeric-owner -czf "$out/$name.tar.gz" "$name"
(cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
tar -tzvf "$out/$name.tar.gz"
cat "$out/$name.tar.gz.sha256"

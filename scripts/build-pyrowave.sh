#!/bin/bash
# Builds the PyroWave runtime (libpyrowave-shared) for Linux or macOS and
# copies it to the given directory.
#
# Usage: scripts/build-pyrowave.sh <output-dir> [macOS architecture]
#
# The macOS architecture is arm64, x86_64 or universal (setup-deps.py builds
# a universal library into libs/mac/lib, which app.pro bundles).
#
# PYROWAVE_BUILD controls failures:
#   auto    (default) warn and continue without the runtime; PyroWave is hidden
#   require fail, for CI and release builds
#   skip    do nothing
#
# third-party/pyrowave is marked "update = none" in .gitmodules so recursive
# checkouts skip its large dependency tree. This script fetches only what the
# runtime build needs: pyrowave, Granite, volk and the Vulkan headers.

set -u

SOURCE_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT_DIR=${1:-}
TARGET_ARCH=${2:-}
MODE=${PYROWAVE_BUILD:-auto}

PYROWAVE_DIR=$SOURCE_ROOT/third-party/pyrowave
GRANITE_DIR=$PYROWAVE_DIR/third_party/Granite

give_up()
{
	if [ "$MODE" = "require" ]; then
		echo "PyroWave runtime: $1" 1>&2
		exit 1
	fi
	echo "PyroWave runtime: $1. Continuing without it; PyroWave will be hidden." 1>&2
	exit 0
}

case "$MODE" in
	skip) echo "PyroWave runtime build skipped (PYROWAVE_BUILD=skip)"; exit 0 ;;
	auto|require) ;;
	*) echo "Invalid PYROWAVE_BUILD: $MODE (expected auto, require or skip)" 1>&2; exit 1 ;;
esac

[ -n "$OUT_DIR" ] || { echo "Usage: $0 <output-dir> [macOS architecture]" 1>&2; exit 1; }
command -v git >/dev/null 2>&1 || give_up "git was not found"
command -v cmake >/dev/null 2>&1 || give_up "cmake was not found"

# Shallow first; fall back to a full fetch for servers that refuse a
# shallow fetch of an unadvertised commit.
update_submodule()
{
	local repo=$1
	shift
	git -C "$repo" submodule update --init --checkout --depth 1 "$@" ||
		git -C "$repo" submodule update --init --checkout "$@"
}

echo "Fetching PyroWave sources"
update_submodule "$SOURCE_ROOT" third-party/pyrowave || give_up "unable to fetch third-party/pyrowave"
update_submodule "$PYROWAVE_DIR" third_party/Granite || give_up "unable to fetch Granite"
update_submodule "$GRANITE_DIR" third_party/volk third_party/khronos/vulkan-headers ||
	give_up "unable to fetch volk and the Vulkan headers"

BUILD_DIR=$SOURCE_ROOT/build/pyrowave-runtime${TARGET_ARCH:+-$TARGET_ARCH}
rm -rf "$BUILD_DIR"

CMAKE_ARGS=(-S "$SOURCE_ROOT/third-party/pyrowave-runtime" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release)
if [ "$(uname -s)" = "Darwin" ]; then
	JOBS=$(sysctl -n hw.logicalcpu)
	LIBRARY_NAME=libpyrowave-shared.101.dylib
	CMAKE_ARGS+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}")
	if [ "$TARGET_ARCH" = "universal" ]; then
		CMAKE_ARGS+=("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64")
	elif [ -n "$TARGET_ARCH" ]; then
		CMAKE_ARGS+=("-DCMAKE_OSX_ARCHITECTURES=$TARGET_ARCH")
	fi
else
	JOBS=$(nproc)
	LIBRARY_NAME=libpyrowave-shared.so.101
fi

echo "Building PyroWave runtime in $BUILD_DIR"
(
	# The app build exports LTO flags. Keep them away from this library.
	unset CFLAGS CXXFLAGS LDFLAGS
	cmake "${CMAKE_ARGS[@]}" || exit 1
	cmake --build "$BUILD_DIR" --target pyrowave-shared -j"$JOBS" || exit 1
) || give_up "the CMake build failed"

BUILT_LIBRARY=$BUILD_DIR/out/$LIBRARY_NAME
[ -s "$BUILT_LIBRARY" ] || give_up "the build did not produce $LIBRARY_NAME"

mkdir -p "$OUT_DIR" || give_up "unable to create $OUT_DIR"
# Copy the real file under its SONAME / install name, not the symlink chain.
cp -L "$BUILT_LIBRARY" "$OUT_DIR/$LIBRARY_NAME" || give_up "unable to copy $LIBRARY_NAME"
echo "PyroWave runtime: $OUT_DIR/$LIBRARY_NAME"

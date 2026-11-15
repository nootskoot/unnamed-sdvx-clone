#!/bin/sh
#
# Build and run the offline Tests.Audio harness with zig c++.
#
# Why this script exists instead of `cmake -B build`
# --------------------------------------------------
# The CMake target links Shared, Audio, Beatmap and Tests, and each of those
# pulls development packages this machine does not have (ogg, vorbis, libarchive,
# zlib, libpng, libjpeg, freetype, SDL2). The harness itself needs none of them:
# it renders DSP blocks in memory and never opens a device or a window. So this
# script compiles exactly the translation units the DSP chain needs, against a
# stub SDL2 header, and links them into one binary.
#
# What it builds
#   Tests.Audio/src/*.cpp          the harness entry point and smoke table
#   Audio/src/DSP.cpp              every DSP under test
#   Audio/src/AudioBase.cpp        the block clock and the effect window
#   Shared/src + Unix + Linux      String, Log, Vector, Math, Path and friends
#   Tests/src                      TestManager
#   third_party/soundtouch/src     real pitch shifting, used by PitchShiftDSP
#
# Deliberately NOT built
#   Shared/src/StringEncodingConverter.cpp    needs <archive.h> (libarchive)
#   Shared/src/StringEncodingDetector.cpp     needs <archive_entry.h>
#   Neither is referenced by anything the harness links. If that changes, the
#   link step fails by name rather than being papered over.
#
# Usage
#   Tests.Audio/build_audit.sh              build, then run the smoke table
#   Tests.Audio/build_audit.sh --build-only build only
#   Tests.Audio/build_audit.sh --clean      remove the build tree first
#   ZIG=/path/to/zig Tests.Audio/build_audit.sh

set -eu

# ---- locate zig ----
if [ -n "${ZIG:-}" ] && [ -x "${ZIG}" ]; then
	:
elif command -v zig >/dev/null 2>&1; then
	ZIG=$(command -v zig)
elif [ -x "${HOME}/.local/lib/python3.11/site-packages/ziglang/zig" ]; then
	ZIG="${HOME}/.local/lib/python3.11/site-packages/ziglang/zig"
elif [ -x "${HOME}/.local/bin/zig" ]; then
	ZIG="${HOME}/.local/bin/zig"
else
	echo "build_audit.sh: no zig found. Set ZIG=/path/to/zig." >&2
	exit 127
fi

# ---- paths ----
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${REPO_ROOT}/Tests.Audio/build"
OBJ_DIR="${BUILD_DIR}/obj"
BIN="${BUILD_DIR}/Tests.Audio"

BUILD_ONLY=0
for arg in "$@"; do
	case "${arg}" in
	--clean) rm -rf "${BUILD_DIR}" ;;
	--build-only) BUILD_ONLY=1 ;;
	*) echo "build_audit.sh: unknown argument ${arg}" >&2; exit 2 ;;
	esac
done

mkdir -p "${OBJ_DIR}"

# ---- flags ----
# -ffunction-sections/-fdata-sections plus --gc-sections drops the parts of
# AudioBase.cpp that call into Audio_Impl's stream stack, so a device-free
# binary links without libarchive, ogg or SDL.
CXXFLAGS="-std=c++17 -O2 -g -ffunction-sections -fdata-sections -Wno-unused-command-line-argument"
LDFLAGS="-Wl,--gc-sections -pthread"

INCLUDES="\
 -I${SCRIPT_DIR}/shim\
 -I${REPO_ROOT}/Audio/include\
 -I${REPO_ROOT}/Audio/include/Audio\
 -I${REPO_ROOT}/Shared/include\
 -I${REPO_ROOT}/Shared/include/Shared\
 -I${REPO_ROOT}/Tests/include\
 -I${REPO_ROOT}/Tests/include/Tests\
 -I${SCRIPT_DIR}\
 -I${SCRIPT_DIR}/include\
 -I${SCRIPT_DIR}/src\
 -I${REPO_ROOT}/third_party/soundtouch/include"

# Each library has its own stdafx.h, and every source starts with #include
# "stdafx.h". The precompiled-header directory of the unit's own library must
# come first, or Shared/src/Path.cpp silently picks up Audio/stdafx.h (and with
# it SDL) instead of Shared/stdafx.h. This mirrors what CMake does per target.
pch_dir_for() {
	case "$1" in
	"${REPO_ROOT}"/Shared/*) echo "${REPO_ROOT}/Shared" ;;
	"${REPO_ROOT}"/Tests/*) echo "${REPO_ROOT}/Tests" ;;
	"${REPO_ROOT}"/Audio/*) echo "${REPO_ROOT}/Audio" ;;
	"${SCRIPT_DIR}"/*) echo "${SCRIPT_DIR}" ;;
	*) echo "" ;;
	esac
}

# The harness has no device, so Audio_Impl's one needed member comes from
# shim/Audio_ImplStandalone.cpp instead of Audio/src/Audio.cpp.
DEFINES="-DUSC_AUDIO_AUDIT_STANDALONE=1"

# ---- source list ----
SOURCES=""
add_sources() {
	for f in "$@"; do
		[ -f "$f" ] || continue
		SOURCES="${SOURCES} $f"
	done
}

# Harness
for f in "${SCRIPT_DIR}"/src/*.cpp "${SCRIPT_DIR}"/src/reference/*.cpp; do add_sources "$f"; done
add_sources "${SCRIPT_DIR}/stdafx.cpp"
# Shims
add_sources "${SCRIPT_DIR}/shim/Audio_ImplStandalone.cpp"
# Audio units under test
add_sources "${REPO_ROOT}/Audio/src/DSP.cpp" "${REPO_ROOT}/Audio/src/AudioBase.cpp"
# Shared library, minus the two libarchive units named above
for f in "${REPO_ROOT}"/Shared/src/*.cpp "${REPO_ROOT}"/Shared/src/Unix/*.cpp "${REPO_ROOT}"/Shared/src/Linux/*.cpp; do
	case "$f" in
	*StringEncodingConverter.cpp | *StringEncodingDetector.cpp) continue ;;
	esac
	add_sources "$f"
done
# Tests library
for f in "${REPO_ROOT}"/Tests/src/*.cpp; do add_sources "$f"; done
# soundtouch: real library, PitchShiftDSP's dependency
for f in "${REPO_ROOT}"/third_party/soundtouch/src/*.cpp; do add_sources "$f"; done

# ---- compile ----
echo "== zig: ${ZIG}"
"${ZIG}" version | sed 's/^/   version: /'
echo "== building Tests.Audio into ${BUILD_DIR}"

OBJECTS=""
COUNT=0
for src in ${SOURCES}; do
	# Object name keeps the relative path so two same-named files cannot collide.
	rel=${src#"${REPO_ROOT}"/}
	obj="${OBJ_DIR}/$(echo "${rel}" | tr '/' '_').o"
	OBJECTS="${OBJECTS} ${obj}"
	COUNT=$((COUNT + 1))
	PCH_DIR=$(pch_dir_for "${src}")
	PCH_FLAG=""
	[ -n "${PCH_DIR}" ] && PCH_FLAG="-I${PCH_DIR}"
	# shellcheck disable=SC2086
	if ! "${ZIG}" c++ ${CXXFLAGS} ${DEFINES} ${PCH_FLAG} ${INCLUDES} -c "${src}" -o "${obj}"; then
		echo "build_audit.sh: compile failed: ${rel}" >&2
		exit 1
	fi
done
echo "   compiled ${COUNT} translation units"

# ---- link ----
# shellcheck disable=SC2086
if ! "${ZIG}" c++ ${LDFLAGS} -o "${BIN}" ${OBJECTS}; then
	echo "build_audit.sh: link failed. An undefined symbol above names the missing" >&2
	echo "unit; add it to the source list or drop the code that needs it." >&2
	exit 1
fi
echo "== linked ${BIN}"

if [ "${BUILD_ONLY}" -eq 1 ]; then
	echo "== build only, not running"
	exit 0
fi

# ---- run ----
echo "== running the smoke table"
echo
"${BIN}"

#!/bin/sh
#
# Build the baseline renderer twice and run both.
#
#   Tests.Audio/baseline/build_baseline.sh
#
# Produces
#   Tests.Audio/baseline/build/before.csv   git HEAD's Audio units (pre-fix)
#   Tests.Audio/baseline/build/after.csv    the working tree
#
# Why two binaries instead of one
# -------------------------------
# The other workers are editing Audio/src/DSP.cpp and Audio/src/AudioBase.cpp
# while this runs, so "before" cannot be the working tree at some earlier
# moment. git HEAD is immutable and is the pre-fix state: every timing and mix
# fix in this audit is an uncommitted change. Building HEAD's two Audio units
# plus HEAD's Audio headers, and nothing else, gives a stable reference side.
#
# The shared libraries (Shared, Tests, soundtouch) do not include Audio headers,
# so they are compiled once and linked into both binaries.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
BUILD="${SCRIPT_DIR}/build"
OBJ_COMMON="${BUILD}/obj_common"
SNAP="${BUILD}/snapshot"

if [ -n "${ZIG:-}" ] && [ -x "${ZIG}" ]; then
	:
elif command -v zig >/dev/null 2>&1; then
	ZIG=$(command -v zig)
elif [ -x "${HOME}/.local/lib/python3.11/site-packages/ziglang/zig" ]; then
	ZIG="${HOME}/.local/lib/python3.11/site-packages/ziglang/zig"
else
	echo "build_baseline.sh: no zig found. Set ZIG=/path/to/zig." >&2
	exit 127
fi

mkdir -p "${BUILD}" "${OBJ_COMMON}"

CXXFLAGS="-std=c++17 -O2 -g -ffunction-sections -fdata-sections -Wno-unused-command-line-argument"
LDFLAGS="-Wl,--gc-sections -pthread"
DEFINES="-DUSC_AUDIO_AUDIT_STANDALONE=1"

COMMON_INCLUDES="\
 -I${SCRIPT_DIR}/shim\
 -I${REPO_ROOT}/Tests.Audio/shim\
 -I${REPO_ROOT}/Shared\
 -I${REPO_ROOT}/Shared/include\
 -I${REPO_ROOT}/Shared/include/Shared\
 -I${REPO_ROOT}/Tests\
 -I${REPO_ROOT}/Tests/include\
 -I${REPO_ROOT}/Tests/include/Tests\
 -I${REPO_ROOT}/Tests.Audio\
 -I${REPO_ROOT}/Tests.Audio/src\
 -I${REPO_ROOT}/Tests.Audio/include\
 -I${REPO_ROOT}/third_party/soundtouch/include"

compile_common()
{
	for src in "$@"; do
		[ -f "$src" ] || continue
		rel=${src#"${REPO_ROOT}"/}
		obj="${OBJ_COMMON}/$(echo "${rel}" | tr '/' '_').o"
		if [ -f "${obj}" ] && [ "${obj}" -nt "${src}" ]; then
			continue
		fi
		"${ZIG}" c++ ${CXXFLAGS} ${DEFINES} ${COMMON_INCLUDES} -c "${src}" -o "${obj}"
	done
}

echo "== zig: ${ZIG} ($("${ZIG}" version))"
echo "== compiling shared units once"

for f in "${REPO_ROOT}"/Shared/src/*.cpp "${REPO_ROOT}"/Shared/src/Unix/*.cpp "${REPO_ROOT}"/Shared/src/Linux/*.cpp; do
	case "$f" in
	*StringEncodingConverter.cpp | *StringEncodingDetector.cpp) continue ;;
	esac
	compile_common "$f"
done
# Tests/src is deliberately absent: TestManager needs the full Shared/stdafx
# include set and prints through Logger, and this renderer has its own entry
# point and writes a CSV. Nothing here calls into it.
for f in "${REPO_ROOT}"/third_party/soundtouch/src/*.cpp; do
	compile_common "$f"
done

build_variant()
{
	variant=$1
	audio_include=$2
	audio_src=$3
	extra_define=$4

	objdir="${BUILD}/obj_${variant}"
	mkdir -p "${objdir}"
	bin="${BUILD}/Tests.Audio.baseline.${variant}"

	includes="-I${audio_include}/Audio -I${audio_include} -I${REPO_ROOT}/Audio ${COMMON_INCLUDES}"

	objects=""
	for src in \
		"${SCRIPT_DIR}/main.cpp" \
		"${audio_src}/DSP.cpp" \
		"${audio_src}/AudioBase.cpp" \
		"${REPO_ROOT}/Tests.Audio/shim/Audio_ImplStandalone.cpp"; do
		[ -f "${src}" ] || { echo "missing source ${src}" >&2; exit 1; }
		obj="${objdir}/$(basename "${src}" .cpp).o"
		objects="${objects} ${obj}"
		# shellcheck disable=SC2086
		"${ZIG}" c++ ${CXXFLAGS} ${DEFINES} ${extra_define} ${includes} -c "${src}" -o "${obj}"
	done

	# shellcheck disable=SC2086
	"${ZIG}" c++ ${LDFLAGS} -o "${bin}" ${objects} "${OBJ_COMMON}"/*.o
	echo "== built ${bin}"
}

# HEAD's Audio layer, for the "before" side.
mkdir -p "${SNAP}"
if [ ! -f "${SNAP}/.done" ]; then
	git -C "${REPO_ROOT}" archive HEAD Audio/include | tar -x -C "${SNAP}"
	mkdir -p "${SNAP}/Audio/src"
	git -C "${REPO_ROOT}" show HEAD:Audio/src/DSP.cpp > "${SNAP}/Audio/src/DSP.cpp"
	git -C "${REPO_ROOT}" show HEAD:Audio/src/AudioBase.cpp > "${SNAP}/Audio/src/AudioBase.cpp"
	: > "${SNAP}/.done"
	echo "== snapshotted HEAD Audio layer into ${SNAP}"
fi

echo "== building before (git HEAD)"
build_variant before "${SNAP}/Audio/include" "${SNAP}/Audio/src" ""

echo "== building after (working tree)"
build_variant after "${REPO_ROOT}/Audio/include" "${REPO_ROOT}/Audio/src" "-DUSC_BASELINE_HAS_BLOCK_START=1"

echo "== running"
"${BUILD}/Tests.Audio.baseline.before" before "${BUILD}/before.csv"
"${BUILD}/Tests.Audio.baseline.after" after "${BUILD}/after.csv"
echo "== wrote ${BUILD}/before.csv and ${BUILD}/after.csv"

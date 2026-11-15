#!/bin/sh
#
# Build and run the chart-driven effect render.
#
# Two binaries, one source:
#   render_effects_before  compiled against the pre-fix tree (a git worktree at
#                          HEAD) with AUDIT_BEFORE=1
#   render_effects_after   compiled against this working tree with AUDIT_BEFORE=0
#
# The source is identical, so the pair differs only by the code under test and by
# the two behaviours that live in AudioPlayback rather than in the DSP classes:
#   * AUDIT_BEFORE=1 does not call GateDSP::SetMeasureTimes for a KSH chart, which
#     is what the old m_ConfigureGateClock did (KSON only)
#   * AUDIT_BEFORE=1 does not stamp dsp->endTime, so an effect runs until the game
#     thread removes it
# Everything else (block size, block clock, DSP classes) comes from the tree.
#
# Usage
#   Tests.Audio/checks/run_render.sh            build both, render every case
#   Tests.Audio/checks/run_render.sh --build    build only
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BEFORE_TREE=${BEFORE_TREE:-/home/superbot/sdvx/before}
OUT="${REPO}/Tests.Audio/audit_out"
PCM_DIR=${PCM_DIR:-/home/superbot/sdvx/pcm}
ZIG=${ZIG:-/home/superbot/.local/lib/python3.11/site-packages/ziglang/zig}

# Shared units the DSP chain needs (String, Log, Math, Buffer, Interpolation, Path...).
# The two encoding units are left out: they need libarchive, and nothing here calls them.
shared_sources() {
	tree=$1
	for f in "${tree}"/Shared/src/*.cpp "${tree}"/Shared/src/Unix/*.cpp "${tree}"/Shared/src/Linux/*.cpp; do
		[ -f "$f" ] || continue
		case "$f" in
		*StringEncodingConverter.cpp | *StringEncodingDetector.cpp) continue ;;
		esac
		echo "$f"
	done
}

build() {
	flavor=$1
	tree=$2
	before=$3
	echo "== building render_effects_${flavor} from ${tree} (AUDIT_BEFORE=${before})"
	ZIG_GLOBAL_CACHE_DIR=/tmp/zig-render-global ZIG_LOCAL_CACHE_DIR=/tmp/zig-render-local \
	"$ZIG" c++ -std=c++20 -O2 -g -ffunction-sections -fdata-sections \
		-DAUDIT_BEFORE="${before}" \
		-DUSC_AUDIO_AUDIT_STANDALONE=1 \
		-I "$HERE/shim" \
		-I "$REPO/Tests.Audio" \
		-I "$REPO/Tests.Audio/include" \
		-I "${tree}/Audio" \
		-I "${tree}/Audio/include" \
		-I "${tree}/Audio/include/Audio" \
		-I "$REPO/Shared/include" \
		-I "$REPO/Shared/include/Shared" \
		-I "$REPO/Tests/include" \
		-I "$REPO/third_party/soundtouch/include" \
		"$HERE/render_effects.cpp" \
		"${tree}/Audio/src/DSP.cpp" \
		"${tree}/Audio/src/AudioBase.cpp" \
		"$REPO/Tests.Audio/shim/Audio_ImplStandalone.cpp" \
		$(shared_sources "${tree}") \
		-Wl,--gc-sections -pthread \
		-o "$HERE/render_effects_${flavor}"
}

mkdir -p "$OUT"
if [ "${SKIP_BUILD:-0}" = "1" ]; then
	echo "== SKIP_BUILD=1, using the binaries already in $HERE"
else
	build before "$BEFORE_TREE" 1
	build after "$REPO" 0
fi

if [ "${1:-}" = "--build" ]; then
	echo "== build only"
	exit 0
fi

render() {
	case_name=$1
	pcm=$2
	start=$3
	end=$4
	shift 4
	flavor=$1
	export AUDIT_MEASURE_TIMES="${OUT}/${case_name}.measures"
	export AUDIT_CLOCK_OFFSET="${5:-0}"
	"$HERE/render_effects_${flavor}" \
		--events "${OUT}/${case_name}.csv" \
		--pcm "${pcm}" \
		--start "$start" --end "$end" \
		--out "${OUT}/${case_name}_${flavor}.wav" \
		--dry "${OUT}/${case_name}_dry.wav" \
		--log "${OUT}/${case_name}_${flavor}.log"
}

for case_name in cherish_ex:68000:76000 cherish_in:70000:74000 hyper:18800:25500; do
	name=${case_name%%:*}
	rest=${case_name#*:}
	start=${rest%%:*}
	end=${rest##*:}
	case $name in
	hyper) pcm="${PCM_DIR}/hyper.f32"; offset=0 ;;
	*) pcm="${PCM_DIR}/cherish.f32"; offset=69 ;;
	esac
	for flavor in before after; do
		render "$name" "$pcm" "$start" "$end" "$flavor" "$offset"
	done
done

echo "== rendered into ${OUT}"

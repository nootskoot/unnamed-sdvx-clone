#!/bin/sh
# Builds and runs the w8 gate-clock check: does a KSH v1 gate follow the chart's
# measure grid instead of free-running from the frame that built its DSP?
#
# Needs no system compiler. zig is used as the C++ driver because this machine has
# no libstdc++-backed g++/clang; the DSP translation units need nothing from SDL
# beyond the type declarations in ./shim, and SoundTouch is only referenced by
# PitchShiftDSP, which --gc-sections drops.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
ZIG=${ZIG:-/home/superbot/.local/lib/python3.11/site-packages/ziglang/zig}

ZIG_GLOBAL_CACHE_DIR=${ZIG_GLOBAL_CACHE_DIR:-$HERE/.zig-cache-global} \
ZIG_LOCAL_CACHE_DIR=${ZIG_LOCAL_CACHE_DIR:-$HERE/.zig-cache} \
"$ZIG" c++ -std=c++20 -O1 -ffunction-sections -fdata-sections \
	-I "$HERE/shim" \
	-I "$ROOT/Audio" \
	-I "$ROOT/Audio/include" \
	-I "$ROOT/Audio/include/Audio" \
	-I "$ROOT/Shared/include" \
	-I "$ROOT/Beatmap/include" \
	-I "$ROOT/Main/include" \
	-I "$ROOT/third_party/soundtouch/include" \
	"$HERE/w8_gate_clock.cpp" \
	"$ROOT/Audio/src/DSP.cpp" \
	"$ROOT/Audio/src/AudioBase.cpp" \
	-Wl,--gc-sections \
	-o "$HERE/w8_gate_clock"

"$HERE/w8_gate_clock"

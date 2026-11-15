#!/bin/sh
# Builds and runs the w13 check: what the clone's peaking filter does at the end
# of a hold, against ksmaudio's 50 ms release and 100-frame v easing.
# Measurement only; no production file is involved.
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
	"$HERE/w13_peaking_release.cpp" \
	"$ROOT/Audio/src/DSP.cpp" \
	"$ROOT/Audio/src/AudioBase.cpp" \
	-Wl,--gc-sections \
	-o "$HERE/w13_peaking_release"

"$HERE/w13_peaking_release"

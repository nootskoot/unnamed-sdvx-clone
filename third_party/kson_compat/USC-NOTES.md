Vendored from https://github.com/kshootmania/libkson
Commit: ed22a3bf2a049de6f9084635deb17d2c319b3109
License: MIT (see LICENSE).

Local change: KshIOIn.cpp center_split uses ParseNumeric<double> instead of
ParseNumeric<int32_t>, matching the official KSH format float specification.
Other upstream files are unchanged. Built as the isolated USCKSH2 C++20 target.

USCKSH2 is compiled with a private nlohmann namespace via a target-local
preprocessor definition in Beatmap/CMakeLists.txt. The bridge exposes only
std::string, so no JSON objects cross this boundary. Standalone builds must
use the same definition for all vendored translation units and the bridge.

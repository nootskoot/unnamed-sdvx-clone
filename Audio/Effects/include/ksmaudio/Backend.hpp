#pragma once
// Shim: replaces ksmaudio's Backend.hpp so the vendored effects build without BASS.
#include <cstdint>
typedef std::uint32_t DWORD;
typedef std::uint64_t QWORD;
typedef DWORD HDSP;

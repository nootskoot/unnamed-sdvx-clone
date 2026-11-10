#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace KSHCompatibility {
// Values use the chart's raw units (not USC's /100 camera units).
inline bool ZoomValue(const std::string& text, int version, bool sameTimePoint, double& value) {
    const auto input = version < 167 ? text.substr(0, 4) : text;
    char* end = nullptr;
    value = std::strtod(input.c_str(), &end);
    if (end == input.c_str() || !std::isfinite(value)) return false;
    value = std::trunc(value);
    return std::abs(value) <= (version < 167 ? 300.0 : 65535.0) ||
        (version < 167 && sameTimePoint);
}
inline float SlamVolumeScale(float width, bool automatic) {
    return automatic ? std::clamp(std::abs(width) * 2.0f, 0.0f, 1.0f) : 1.0f;
}
struct Crop { float first, last; bool Visible() const { return last > first; } };
// Crop the source half against KSM's 512px target before perspective.
// split is USC's center_split / 100. Side 0 is right, side 1 is left.
inline Crop SplitCrop(float split, int side, float contentWidth = 256.0f) {
    if (!std::isfinite(split)) return {0,0};
    const float half = contentWidth * 0.5f;
    const float start = side == 0 ? 0.0f : -half;
    const float shift = (side == 0 ? 1.0f : -1.0f) * 21.0f * split;
    return {std::clamp((-256.0f - shift - start) / half, 0.0f, 1.0f),
            std::clamp((256.0f - shift - start) / half, 0.0f, 1.0f)};
}
}

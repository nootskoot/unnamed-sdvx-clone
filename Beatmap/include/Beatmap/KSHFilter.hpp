#pragma once
#include <algorithm>
#include <cmath>
namespace KSHFilter {
// Built-in KSH laser curves from KSM v1 play_audio_effects.hsp.
// pfiltergain is a gain/resonance control, not the wet/dry mix.
struct Parameters { float frequency, q, gain; };
inline Parameters Peak(float v, float gain, float sampleRate) {
    v=std::clamp(v,0.f,1.f);
    const float c=std::cos(v);
    const float frequency=std::min(10000.f-9950.f*(4*c+5*c*c)/9.f+15000.f*std::max(v-.8f,0.f),sampleRate*.49f);
    const float envelope=std::clamp(14.f*std::min(v/.15f,1.f)+3.f*(1-std::abs(v-.35f)/.35f)-17.5f*std::max(v-.8f,0.f),0.f,18.f);
    const double omega=6.283185307179586*frequency/sampleRate;
    const float q=float(1.0/(2*std::sinh(std::log(2.0)*.6*omega/std::sin(omega))));
    return {frequency,q,frequency<100 ? 0.f : envelope*gain*2};
}
inline float HighFrequency(float v) { return 4600.f-4500.f*std::cos(std::clamp(v,0.f,1.f)); }
inline float LowFrequency(float v) { return 15000.f-14200.f*std::sin(std::sin(std::clamp(v,0.f,1.f)*1.25f)/std::sin(1.25f)*1.5f)/std::sin(1.5f); }
}

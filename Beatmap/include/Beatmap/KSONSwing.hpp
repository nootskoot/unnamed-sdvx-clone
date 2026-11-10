#pragma once
#include <cmath>
#include <cstdint>

struct KSONSwing
{
    uint32_t length = 0; // pulses, 240 per quarter note
    double scale = 250.0;
    uint32_t repeat = 3;
    uint32_t decay = 2;
    int direction = 1;

    // Return normalized zoom_side units, so USC's existing camera transform
    // handles aspect ratio and track scale. KSM shiftX=0.1*scale*wave;
    // zoom_side=6*shiftX and USC stores zoom_side/100.
    double Offset(double elapsedPulse) const
    {
        if (!length || elapsedPulse < 0 || elapsedPulse >= length) return 0;
        const double t = elapsedPulse / length;
        const double envelope = decay==0 ? 1.0 : decay==1 ? 1.0-t : (1.0-t)*(1.0-t);
        return .006 * scale * std::sin(3.14159265358979323846 * repeat * t) * envelope * direction;
    }
};

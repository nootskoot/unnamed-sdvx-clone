
// w13: what the peaking filter does today when its hold ends, against what
// ksmaudio's PeakingFilterDSP does over its 50 ms release. Measurement only:
// nothing here is wired into the game, and no production file is changed.
//
// Builds the clone's real BQFDSP from Audio/src/DSP.cpp. The reference side is
// the release and easing arithmetic transcribed from
// ksmaudio/src/AudioEffect/DSP/PeakingFilterDSP.cpp at 5c2466d.

#include "stdafx.h"
#include "AudioBase.hpp"
#include "DSP.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
	constexpr uint32 kSampleRate = 44100;
	constexpr uint32 kBlock = 384;          // Audio_Impl::m_sampleBufferLength
	constexpr size_t kFrames = 20000;

	// ksmaudio constants, PeakingFilterDSP.cpp anonymous namespace.
	constexpr float kFilterReleaseSec = 0.05f;
	constexpr float kVEasingSpeed = 0.01f;

	const size_t kReleaseFrames = static_cast<size_t>(kSampleRate * kFilterReleaseSec); // 2205
	const size_t kEasingFrames = static_cast<size_t>(1.0f / kVEasingSpeed);             // 100

	struct Sine
	{
		double phase = 0.0;
		double inc;
		explicit Sine(double hz) : inc(2.0 * 3.14159265358979323846 * hz / kSampleRate) {}
		float Next()
		{
			const float v = static_cast<float>(0.25 * std::sin(phase));
			phase += inc;
			return v;
		}
	};

	std::vector<float> MakeInput(double hz, size_t frames)
	{
		Sine s(hz);
		std::vector<float> in(frames * 2);
		for (size_t i = 0; i < frames; ++i)
		{
			const float v = s.Next();
			in[i * 2 + 0] = v;
			in[i * 2 + 1] = v;
		}
		return in;
	}

	// RMS in dB over [from, from+len) of one channel, relative to the input RMS.
	double WindowDb(const std::vector<float>& out, const std::vector<float>& in, size_t from, size_t len)
	{
		double so = 0.0, si = 0.0;
		for (size_t i = from; i < from + len && i * 2 + 1 < out.size(); ++i)
		{
			so += double(out[i * 2]) * out[i * 2];
			si += double(in[i * 2]) * in[i * 2];
		}
		if (si <= 0.0 || so <= 0.0) return -200.0;
		return 10.0 * std::log10(so / si);
	}
}

int main()
{
	const std::vector<float> in = MakeInput(1000.0, kFrames);
	const uint32 qTimes100 = 400;   // BQFDSP::SetPeaking takes q as a float; Q4 matches a 1.2 octave peaking band
	const float q = 4.0f;
	const float freq = 1000.0f;
	const float gainDb = 6.0f;
	const size_t switchAt = 4096;   // where the hold ends

	std::printf("peaking: Q %.1f, %.0f Hz, %+.1f dB, %u Hz\n", q, freq, gainDb, kSampleRate);
	std::printf("ksmaudio release = %zu frames (%.0f ms), v easing = %zu frames (%.3f ms)\n\n",
		kReleaseFrames, kFilterReleaseSec * 1000.0f, kEasingFrames, 1000.0 * double(kEasingFrames) / kSampleRate);

	// ---- A: the clone today. Coefficients are set from the game thread, so the
	// hold's end arrives as one more SetPeaking call with gain 0.
	std::vector<float> a = in;
	{
		BQFDSP f(kSampleRate);
		f.mix = 1.0f;
		f.SetPeaking(q, freq, gainDb);
		size_t i = 0;
		while (i < switchAt)
		{
			const uint32 n = static_cast<uint32>(std::min<size_t>(kBlock, switchAt - i));
			f.Process(&a[i * 2], n);
			i += n;
		}
		f.SetPeaking(q, freq, 0.0f);   // hold ended
		while (i < kFrames)
		{
			const uint32 n = static_cast<uint32>(std::min<size_t>(kBlock, kFrames - i));
			f.Process(&a[i * 2], n);
			i += n;
		}
	}

	// ---- B: ksmaudio's release. Gain falls linearly to zero over 2205 frames and
	// the coefficients are recomputed every frame, exactly as the reference does.
	std::vector<float> b = in;
	{
		BQFDSP f(kSampleRate);
		f.mix = 1.0f;
		f.SetPeaking(q, freq, gainDb);
		size_t i = 0;
		while (i < switchAt)
		{
			const uint32 n = static_cast<uint32>(std::min<size_t>(kBlock, switchAt - i));
			f.Process(&b[i * 2], n);
			i += n;
		}
		for (size_t k = 0; k < kReleaseFrames; ++k)
		{
			const float scale = 1.0f - static_cast<float>(k) / static_cast<float>(kReleaseFrames);
			f.SetPeaking(q, freq, gainDb * scale);
			f.Process(&b[(switchAt + k) * 2], 1);
		}
		f.SetPeaking(q, freq, 0.0f);
		for (size_t k = switchAt + kReleaseFrames; k < kFrames; ++k)
			f.Process(&b[k * 2], 1);
	}

	// ---- gain trajectory either side of the switch
	std::printf("%-9s %-8s %-10s %-10s\n", "t_rel_ms", "frames", "clone_dB", "release_dB");
	const size_t win = 64;
	for (int off = -1; off <= 40; ++off)
	{
		const long long from = static_cast<long long>(switchAt) + static_cast<long long>(off) * static_cast<long long>(win);
		if (from < 0 || size_t(from) + win >= kFrames) continue;
		const double tms = 1000.0 * double(from - static_cast<long long>(switchAt)) / kSampleRate;
		std::printf("%-9.2f %-8lld %-10.2f %-10.2f\n", tms, from - static_cast<long long>(switchAt),
			WindowDb(a, in, size_t(from), win), WindowDb(b, in, size_t(from), win));
	}

	// How much the switch exceeds the signal's own motion, and how far the filter
	// rings afterwards. A clean handover gives a ratio near 1 and no overshoot.
	auto stepAt = [](const std::vector<float>& out, size_t i)
	{
		return std::fabs(double(out[i * 2]) - out[i * 2 - 2]);
	};
	auto maxStepIn = [&](const std::vector<float>& out, size_t from, size_t to)
	{
		double worst = 0.0;
		for (size_t i = from + 2; i < to; ++i) worst = std::max(worst, stepAt(out, i));
		return worst;
	};
	auto peakIn = [](const std::vector<float>& out, size_t from, size_t to)
	{
		double p = 0.0;
		for (size_t i = from; i < to && i * 2 + 1 < out.size(); ++i) p = std::max(p, std::fabs(double(out[i * 2])));
		return p;
	};
	auto report = [&](const char* label, const std::vector<float>& out)
	{
		const double natural = maxStepIn(out, switchAt - 512, switchAt - 2);
		const double atSwitch = stepAt(out, switchAt);
		const double before = peakIn(out, switchAt - 1024, switchAt);
		const double after = peakIn(out, switchAt, switchAt + 882);   // 20 ms
		std::printf("  %-8s step at switch %.5f, largest natural step %.5f, ratio %.2fx, peak ring %+.2f dB\n",
			label, atSwitch, natural, natural > 0.0 ? atSwitch / natural : 0.0,
			before > 0.0 ? 20.0 * std::log10(after / before) : 0.0);
	};
	std::printf("\nhandover at the switch:\n");
	report("clone", a);
	report("release", b);

	// ---- the v easing: a frequency jump with and without the 100-frame glide
	const float freqFrom = 300.0f, freqTo = 3000.0f;
	const size_t jumpAt = 8192;
	std::vector<float> snap = in, eased = in;
	{
		BQFDSP f(kSampleRate);
		f.mix = 1.0f;
		f.SetPeaking(q, freqFrom, gainDb);
		size_t i = 0;
		while (i < jumpAt) { const uint32 n = static_cast<uint32>(std::min<size_t>(kBlock, jumpAt - i)); f.Process(&snap[i * 2], n); i += n; }
		f.SetPeaking(q, freqTo, gainDb);          // one frame, no glide
		while (i < kFrames) { const uint32 n = static_cast<uint32>(std::min<size_t>(kBlock, kFrames - i)); f.Process(&snap[i * 2], n); i += n; }
	}
	{
		BQFDSP f(kSampleRate);
		f.mix = 1.0f;
		f.SetPeaking(q, freqFrom, gainDb);
		size_t i = 0;
		while (i < jumpAt) { const uint32 n = static_cast<uint32>(std::min<size_t>(kBlock, jumpAt - i)); f.Process(&eased[i * 2], n); i += n; }
		for (size_t k = 0; k < kEasingFrames; ++k)
		{
			const float t = static_cast<float>(k + 1) / static_cast<float>(kEasingFrames);
			f.SetPeaking(q, freqFrom + (freqTo - freqFrom) * t, gainDb);
			f.Process(&eased[(jumpAt + k) * 2], 1);
		}
		for (size_t k = jumpAt + kEasingFrames; k < kFrames; ++k) f.Process(&eased[k * 2], 1);
	}
	std::printf("\nfrequency jump %.0f -> %.0f Hz at %+.1f dB:\n", freqFrom, freqTo, gainDb);
	{
		auto peakIn2 = [](const std::vector<float>& out, size_t from, size_t to)
		{
			double p = 0.0;
			for (size_t i = from; i < to && i * 2 + 1 < out.size(); ++i) p = std::max(p, std::fabs(double(out[i * 2])));
			return p;
		};
		auto maxStep2 = [](const std::vector<float>& out, size_t from, size_t to)
		{
			double worst = 0.0;
			for (size_t i = from + 2; i < to; ++i) worst = std::max(worst, std::fabs(double(out[i * 2]) - out[i * 2 - 2]));
			return worst;
		};
		for (const auto& pair : {std::pair<const char*, const std::vector<float>*>{"snap", &snap}, {"eased", &eased}})
		{
			const auto& o = *pair.second;
			const double natural = maxStep2(o, jumpAt - 512, jumpAt - 2);
			const double at = maxStep2(o, jumpAt, jumpAt + 64);
			const double before = peakIn2(o, jumpAt - 1024, jumpAt);
			const double after = peakIn2(o, jumpAt, jumpAt + 882);
			std::printf("  %-5s step ratio %.2fx, peak ring %+.2f dB over 20 ms\n",
				pair.first, natural > 0.0 ? at / natural : 0.0, 20.0 * std::log10(after / before));
		}
	}

	// ---- gain a stationary filter is holding, for scale
	std::printf("\nsteady state boost before the switch, clone: %.2f dB\n",
		WindowDb(a, in, switchAt - win * 4, win));
	return 0;
}

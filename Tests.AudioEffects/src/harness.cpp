// Offline harness for the vendored ksmaudio effects (migration step 1 gate + section-7 edge test).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <string>
#include "ksmaudio/Stream.hpp"
#include "ksmaudio/AudioEffect/AudioEffectBus.hpp"
#include "ksmaudio/AudioEffect/All.hpp"

using namespace ksmaudio;
using AudioEffect::Status;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ---------- A. smoke: every effect, FX and laser bus, noise + clicks, varied block sizes ----------
template <typename T>
static void smoke(const char* name, bool isLaser)
{
	constexpr std::size_t sr = 44100, ch = 2;
	Stream stream(sr, ch);
	{
		AudioEffect::AudioEffectBus bus(isLaser, &stream);
		bus.emplaceAudioEffect<T>("fx");
		std::mt19937 rng(1234);
		std::uniform_real_distribution<float> d(-0.5f, 0.5f);
		const std::size_t blocks[] = { 64, 256, 512, 1000, 4096 };
		double sec = 0.0; bool bad = false; float peak = 0;
		for (int iter = 0; iter < 400 && !bad; ++iter)
		{
			const std::size_t frames = blocks[iter % 5];
			std::vector<float> buf(frames * ch);
			for (auto& s : buf) s = d(rng);
			if (iter % 7 == 0) buf[0] = buf[1] = 1.0f; // click
			Status st{ .v = 0.5f + 0.5f * std::sin(iter * 0.37f), .bpm = 150.0f, .sec = static_cast<float>(sec), .playbackSpeed = 1.0f };
			const bool on = (iter / 20) % 2 == 0;
			if (isLaser) bus.updateByLaser(st, on ? std::optional<std::size_t>(0) : std::nullopt);
			else { AudioEffect::ParamValueSetDict none; AudioEffect::ActiveAudioEffectDict act; if (on) act.emplace(0, AudioEffect::ActiveAudioEffectInvocation{ &none, 0 }); bus.updateByFX(st, act); }
			stream.processOffline(buf.data(), buf.size());
			for (float s : buf) { if (!std::isfinite(s)) bad = true; peak = std::fmax(peak, std::fabs(s)); }
			sec += static_cast<double>(frames) / sr;
		}
		CHECK(!bad, "%s (%s) produced non-finite samples", name, isLaser ? "laser" : "fx");
		CHECK(peak < 8.0f, "%s (%s) peak %.2f unreasonably large", name, isLaser ? "laser" : "fx", peak);
		std::printf("  %-14s %-5s peak=%.3f %s\n", name, isLaser ? "laser" : "fx", peak, bad ? "NaN/Inf" : "ok");
	}
	CHECK(stream.chainSize() == 0, "%s: bus destructor left effects in chain", name);
}

// ---------- B. gate edge test ----------
// Chart: bpm 120, gate wave_length 1/8 note-of-whole (=0.25 s at 120 bpm), rate 50%, update triggers at map-time T1,T2.
// Free-running phase is closed in [0.125,0.25) so T1=0.2 and T2=0.6 land in closed parts -> each trigger gives a rising edge.
static void gateEdges(double audioOffset, double speed)
{
	constexpr std::size_t sr = 44100, ch = 2;
	const float T1 = 0.2f, T2 = 0.6f;
	Stream stream(sr, ch);
	AudioEffect::AudioEffectBus bus(false, &stream);
	bus.emplaceAudioEffect<Gate>("gate", std::map<std::string, std::string>{ {"wave_length", "1/8"}, {"rate", "50%"}, {"mix", "100%"} },
		std::unordered_map<std::string, std::map<float, std::string>>{}, std::set<float>{ T1, T2 });
	AudioEffect::ParamValueSetDict none; AudioEffect::ActiveAudioEffectDict act; act.emplace(0, AudioEffect::ActiveAudioEffectInvocation{ &none, 0 });

	const std::size_t total = sr * 2;           // 2 s of output
	std::vector<float> out; out.reserve(total * ch);
	const std::size_t block = 512;
	for (std::size_t f = 0; f < total; f += block)
	{
		// Host: file time advances by `speed` per output second; map time = file time - audioOffset.
		const double fileSec = static_cast<double>(f) / sr * speed;
		const double mapSec = fileSec - audioOffset;
		// Convention under test: host passes bpm scaled by speed (the DSP runs post-stretch), sec in map seconds.
		Status st{ .v = 1.0f, .bpm = static_cast<float>(120.0 * speed), .sec = static_cast<float>(mapSec), .playbackSpeed = static_cast<float>(speed) };
		bus.updateByFX(st, act);
		std::vector<float> buf(block * ch, 1.0f);
		stream.processOffline(buf.data(), buf.size());
		out.insert(out.end(), buf.begin(), buf.end());
	}
	// find rising edges (closed -> open) on left channel
	std::vector<double> edges;
	for (std::size_t i = 1; i < total; ++i)
		if (out[(i - 1) * ch] < 0.5f && out[i * ch] >= 0.5f) edges.push_back(static_cast<double>(i));
	// expectation in output frames: trigger at map time T -> file time T+off -> output time (T+off)/speed
	const double period = 0.25 / speed * sr;
	std::vector<double> expect = { (T1 + audioOffset) / speed * sr, (T1 + audioOffset) / speed * sr + period, (T2 + audioOffset) / speed * sr, (T2 + audioOffset) / speed * sr + period };
	std::printf("  offset=%2.0f ms speed=%.1f : edges found=%zu\n", audioOffset * 1000, speed, edges.size());
	for (double e : expect)
	{
		double best = 1e9, bestE = -1;
		for (double a : edges) if (std::fabs(a - e) < std::fabs(best)) { best = a - e; bestE = a; }
		std::printf("     expect %9.1f  got %9.1f  err %+7.1f frames (%+.2f ms)\n", e, bestE, best, best / sr * 1000);
		CHECK(std::fabs(best) <= 1.5, "gate edge err %.1f frames at offset=%.0fms speed=%.1f", best, audioOffset * 1000, speed);
	}
}

int main()
{
	std::printf("== A. smoke (all effects, FX + laser bus) ==\n");
#define S(T) smoke<T>(#T, false); smoke<T>(#T, true);
	S(Retrigger) S(Gate) S(Flanger) S(Bitcrusher) S(Phaser) S(PitchShift) S(Wobble) S(Tapestop) S(Echo) S(Sidechain) S(PeakingFilter) S(HighPassFilter) S(LowPassFilter)
	std::printf("== B. gate edge sample positions (tolerance 1 frame) ==\n");
	for (double off : { 0.0, 0.037 }) for (double sp : { 1.0, 0.5 }) gateEdges(off, sp);
	std::printf("\nRESULT: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
	return g_fail ? 1 : 0;
}

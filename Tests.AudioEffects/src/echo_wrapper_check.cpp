// Checks KsmEchoDSP (the USC-side wrapper, built C++17 against a stand-in DSP base) against ksmaudio's own
// Echo running through the effect bus, and checks start-sample accuracy / block-size invariance.
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "KsmEchoDSP.hpp"
#include "usc/KsmEchoCore.hpp"
#include "ksmaudio/Stream.hpp"
#include "ksmaudio/AudioEffect/AudioEffectBus.hpp"
#include "ksmaudio/AudioEffect/All.hpp"
using namespace ksmaudio;
constexpr uint32 SR = 44100;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<float> noise(std::size_t frames) {
	std::vector<float> x(frames * 2); uint32_t s = 12345;
	for (auto& v : x) { s = s * 1664525u + 1013904223u; v = (float(s >> 8) / float(1 << 24) - 0.5f); }
	return x;
}
static double maxDiff(const std::vector<float>& a, const std::vector<float>& b, std::size_t f0, std::size_t f1) {
	double m = 0; for (std::size_t i = f0 * 2; i < f1 * 2; ++i) m = std::max(m, double(std::fabs(a[i] - b[i]))); return m;
}

int main() {
	const std::size_t total = SR * 4;
	const auto in = noise(total);

	// ---- T1: wrapper == ksmaudio Echo bus, note [noteOn, noteOff), block-aligned, 512-frame blocks ----
	{
		const std::size_t B = 512, noteOn = 86 * B, noteOff = noteOn + 172 * B;
		std::vector<float> bus(in), wr(in);
		Stream stream(SR, 2);
		AudioEffect::AudioEffectBus eb(false, &stream);
		eb.emplaceAudioEffect<Echo>("echo", std::map<std::string, std::string>{ {"wave_length", "1/4"}, {"feedback_level", "60%"}, {"mix", "0%>100%"}, {"update_trigger", "off>on"} },
			std::unordered_map<std::string, std::map<float, std::string>>{}, std::set<float>{});
		AudioEffect::ParamValueSetDict none;
		for (std::size_t f = 0; f < total; f += B) {
			AudioEffect::ActiveAudioEffectDict act;
			if (f >= noteOn && f < noteOff) act.emplace(0, AudioEffect::ActiveAudioEffectInvocation{ &none, 0 });
			eb.updateByFX(AudioEffect::Status{ .v = 1.0f, .bpm = 120.0f, .sec = float(double(f) / SR), .playbackSpeed = 1.0f }, act);
			stream.processOffline(&bus[f * 2], std::min(B, total - f) * 2);
		}
		KsmEchoDSP d(SR); d.SetLength(500.0); d.feedback = 0.6f; d.mix = 1.0f;
		for (std::size_t f = noteOn; f < noteOff; f += B) { d.testBlockStart = f; d.Process(&wr[f * 2], uint32(B)); }
		const double dd = maxDiff(bus, wr, noteOn, noteOff);
		std::printf("T1 wrapper vs ksmaudio Echo bus, %zu frames: max|diff| = %.3g\n", noteOff - noteOn, dd);
		CHECK(dd == 0.0, "wrapper differs from ksmaudio bus (%.3g)", dd);
		CHECK(maxDiff(in, wr, 0, noteOn) == 0.0, "wrapper touched audio before the note");
	}

	// ---- T2: authored start inside a block + irregular block sizes vs a single-call reference ----
	{
		const uint32 startMs = 1000; const std::size_t startF = std::size_t(startMs) * SR / 1000, endF = SR * 3;
		std::vector<float> wr(in), ref(in);
		usc::fx::KsmEchoCore core(SR); core.SetParams(0.5f, 0.6f, 1.0f);
		core.Process(&ref[startF * 2], uint32(endF - startF));
		KsmEchoDSP d(SR); d.startTime = startMs; d.SetLength(500.0); d.feedback = 0.6f; d.mix = 1.0f;
		const std::size_t sizes[] = { 512, 333, 1024, 77, 4096, 255 };
		std::size_t f = 43520; int i = 0; // first block starts before startF
		while (f < endF) { const std::size_t n = std::min(sizes[i++ % 6], endF - f); d.testBlockStart = f; d.Process(&wr[f * 2], uint32(n)); f += n; }
		const double pre = maxDiff(in, wr, 0, startF), post = maxDiff(ref, wr, startF, endF);
		std::printf("T2 start-sample accuracy: pre-start max|diff vs input| = %.3g, post-start max|diff vs reference| = %.3g\n", pre, post);
		CHECK(pre == 0.0, "frames before the authored start were modified");
		CHECK(post == 0.0, "block-size / start handling changed the output");
	}
	std::printf("\nRESULT: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
	return g_fail ? 1 : 0;
}

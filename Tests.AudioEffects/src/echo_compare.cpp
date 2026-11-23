// Echo behaviour comparison: ksmaudio Echo (vendored) vs a transcription of USC's EchoDSP::Process
// (Audio/src/DSP.cpp @ 91699b8, only the mutex and Vector/Math wrappers replaced). Offline, deterministic.
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "ksmaudio/Stream.hpp"
#include "ksmaudio/AudioEffect/AudioEffectBus.hpp"
#include "ksmaudio/AudioEffect/All.hpp"
using namespace ksmaudio;

constexpr std::size_t SR = 44100;

// ---- USC EchoDSP transcription (stereo interleaved float) ----
struct UscEcho {
	std::vector<float> buf; std::size_t off = 0; std::size_t len = 0; float feedback = 0.6f, mix = 1.0f;
	void SetLength(double ms) { double fl = ms / 1000.0 * SR; std::size_t nl = std::size_t(std::max(0.0, fl)) * 2; if (nl == len) return; off = 0; len = nl; buf.assign(len, 0.f); }
	void Process(float* out, std::size_t n) {
		if (buf.empty() || len < 2) return;
		const float g = std::clamp(feedback, 0.0f, 0.999f), wet = std::clamp(mix, 0.0f, 1.0f);
		for (std::size_t i = 0; i < n; i++) {
			float dl = buf[off], dr = buf[off + 1], l = out[i * 2], r = out[i * 2 + 1];
			buf[off] = l + dl * g; buf[off + 1] = r + dr * g;
			out[i * 2] = std::lerp(l, dl, wet); out[i * 2 + 1] = std::lerp(r, dr, wet);
			off += 2; if (off >= len) off = 0;
		}
	}
};

// Input: impulses every 0.1 s from t=0.9 s, amplitude 0.1*(k+1) so output peaks identify their source impulse.
static std::vector<float> makeInput(double dur) {
	std::vector<float> x(std::size_t(dur * SR) * 2, 0.f);
	for (int k = 0; k < 40; ++k) { std::size_t f = std::size_t((0.9 + 0.1 * k) * SR); if (f * 2 + 1 < x.size()) x[f * 2] = x[f * 2 + 1] = 0.1f * (k + 1) / 4.0f; }
	return x;
}
static void dump(const char* name, const std::vector<float>& y, double t0, double t1) {
	std::printf("  %s peaks (t[s]: amp), window %.1f-%.1f s\n   ", name, t0, t1);
	int n = 0;
	for (std::size_t f = std::size_t(t0 * SR); f < std::size_t(t1 * SR) && f * 2 < y.size(); ++f)
		if (std::fabs(y[f * 2]) > 0.004f) { std::printf("%.3f:%.3f  ", double(f) / SR, y[f * 2]); if (++n % 6 == 0) std::printf("\n   "); }
	std::printf("\n");
}

int main() {
	const double dur = 4.0, noteOn = 1.0, noteOff = 3.0, L = 0.5; // wave_length 1/4 @ 120 bpm = 0.5 s
	const std::size_t block = 512, total = std::size_t(dur * SR);
	const auto in = makeInput(dur);

	// ---- ksmaudio Echo: effect lives in the chain from t=0, note-on at 1.0 s ----
	std::vector<float> ks(in);
	{
		Stream stream(SR, 2);
		AudioEffect::AudioEffectBus bus(false, &stream);
		bus.emplaceAudioEffect<Echo>("echo", std::map<std::string, std::string>{ {"wave_length", "1/4"}, {"feedback_level", "60%"}, {"mix", "0%>100%"}, {"update_trigger", "off>on"} },
			std::unordered_map<std::string, std::map<float, std::string>>{}, std::set<float>{});
		AudioEffect::ParamValueSetDict none;
		for (std::size_t f = 0; f < total; f += block) {
			const double t = double(f) / SR;
			AudioEffect::ActiveAudioEffectDict act;
			if (t >= noteOn && t < noteOff) act.emplace(0, AudioEffect::ActiveAudioEffectInvocation{ &none, 0 });
			AudioEffect::Status st{ .v = 1.0f, .bpm = 120.0f, .sec = float(t), .playbackSpeed = 1.0f };
			bus.updateByFX(st, act);
			const std::size_t n = std::min(block, total - f);
			stream.processOffline(&ks[f * 2], n * 2);
		}
	}
	// ---- USC: DSP created at note start (empty buffer), removed at note end ----
	std::vector<float> us(in);
	{
		UscEcho e; e.feedback = 0.6f; e.mix = 1.0f; e.SetLength(L * 1000.0);
		for (std::size_t f = std::size_t(noteOn * SR); f < std::size_t(noteOff * SR); f += block) {
			const std::size_t n = std::min<std::size_t>(block, std::size_t(noteOff * SR) - f);
			e.Process(&us[f * 2], n);
		}
	}
	std::printf("input: impulse k at t=0.9+0.1k s, amp=0.025*(k+1); echo note [%.1f,%.1f) s, loop/delay %.2f s, feedback 60%%, mix 100%%\n", noteOn, noteOff, L);
	dump("INPUT   ", in, 0.9, 2.1);
	dump("ksmaudio", ks, 0.9, 3.6);
	dump("USC     ", us, 0.9, 3.6);
	return 0;
}

/*
	AuditTests: the regression suite for the audio timing and gain fixes.

	What it pins, and why each one is here rather than in the smoke table:

	1. AudioBlockClock    DSP::GetCurrentSample() must be the first frame of the
	                      block being processed. AudioStreamBase::Process decodes a
	                      whole block and only then advances the stream position, so
	                      reading GetSamplePos() from a DSP is one block late. Every
	                      startTime comparison in the tree used to be that late; this
	                      test fails by exactly one block if that ever comes back.
	2. AudioEffectWindow  An effect covers [startTime, endTime) in milliseconds. The
	                      block that contains endTime must render only up to it, and
	                      later blocks must not run the DSP at all. endTime == 0 is
	                      unbounded. Holds used to stop when the game thread got
	                      around to ClearEffect, which is a frame late, and this is
	                      what replaced it.
	3. AudioGateRamp      The clone's gate envelope must equal ksmaudio's
	                      GateDSP::Scale() sample for sample. The reference is
	                      transcribed, not vendored, in src/reference/GateDSP.hpp, so
	                      a bug shared with the clone cannot hide here.
	4. AudioEchoMixLaw    mix 0 is transparent, mix 1 is fully wet, and the delay
	                      line stores dry + delayed * feedback so a repeat decays by
	                      feedback only. The old code scaled the wet signal by mix and
	                      stored it back, losing the dry input and compounding the
	                      mix into the decay.
	5. AudioBqfMixLaw     A biquad must blend dry into the filtered signal, not
	                      replace it. Asserted as a law between three parallel runs,
	                      which is phase-independent: out(mix) == dry + (wet - dry)*mix,
	                      where wet is the mix = 1 run of the same filter on the same
	                      input. Plus the two endpoints: mix 0 is bit-identical to the
	                      input, and a peaking filter at its centre frequency really
	                      does produce its nominal gain.

	Running it
		Tests.Audio/build_audit.sh --build-only
		Tests.Audio/build/Tests.Audio AudioAudit            # all five
		Tests.Audio/build/Tests.Audio AudioGateRamp          # just one

	The names are registered with the repository's TestManager, so the entry point
	does not need a table of its own. A failing check throws TestFailure, which the
	manager prints with its expression and a stack trace, and the process exits
	non-zero.
*/

#include "stdafx.h"
#include "TestAudioBase.hpp"

#include <Tests/TestManager.hpp>

#include "reference/GateDSP.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
	constexpr uint32 kSampleRate = 44100;
	constexpr uint32 kBlockFrames = 384;

	void Require(bool condition, const char *what)
	{
		if (!condition)
		{
			throw TestFailure(String(what));
		}
	}

	void RequireNear(double got, double want, double tolerance, const char *what)
	{
		if (!(std::fabs(got - want) <= tolerance))
		{
			throw TestFailure(Utility::Sprintf("%s (got %.9f, want %.9f, tolerance %.9g)", what, got, want,
											   tolerance));
		}
	}

	// A DSP that reports what the base class told it, and halves the signal so a
	// test can see which frames were processed at all. It deliberately does not
	// check startTime itself: the point of the window test is what
	// AudioBase::ProcessDSPs hands the DSP, not what the DSP decides to do.
	class ProbeDSP final : public DSP
	{
	public:
		struct Call
		{
			uint32 numSamples = 0;
			uint64 clockSample = 0;
		};

		std::vector<Call> calls;

		explicit ProbeDSP(uint32 sampleRate)
		{
			SetSampleRate(sampleRate);
		}

		void Process(float *out, uint32 numSamples) override
		{
			calls.push_back(Call{numSamples, static_cast<uint64>(GetCurrentSample())});
			for (uint32 i = 0; i < numSamples; i++)
			{
				out[i * 2 + 0] *= 0.5f;
				out[i * 2 + 1] *= 0.5f;
			}
		}

		[[nodiscard]] const char *GetName() const override { return "ProbeDSP"; }
	};

	// Renders <frames> frames through the stub in blocks of <blockFrames>, driving
	// the input from gen(frameIndex). Returns the interleaved output; when
	// <input> is not null the interleaved input is appended to it, so a test can
	// compare what went in with what came out.
	template <typename TGen>
	std::vector<float> RenderSignal(TestAudioBase &audio, uint32 frames, uint32 blockFrames, TGen &&gen,
									std::vector<float> *input = nullptr)
	{
		std::vector<float> out;
		out.reserve(size_t(frames) * 2);
		std::vector<float> block(size_t(blockFrames) * 2, 0.0f);

		for (uint32 done = 0; done < frames;)
		{
			const uint32 count = Math::Min(frames - done, blockFrames);
			for (uint32 i = 0; i < count; i++)
			{
				const float sample = gen(done + i);
				block[i * 2 + 0] = sample;
				block[i * 2 + 1] = sample;
				if (input)
				{
					input->push_back(sample);
					input->push_back(sample);
				}
			}
			audio.RenderBlock(block.data(), count);
			out.insert(out.end(), block.begin(), block.begin() + count * 2);
			done += count;
		}

		return out;
	}

	struct FilterRun
	{
		std::vector<float> input;
		std::vector<float> output;
	};

	// One filter at one mix over one input. The DSP is created before the stub so
	// the stub's destructor detaches it while it is still alive, on both the
	// success and the failure path.
	template <typename TDSP, typename TConfig>
	FilterRun RunFilter(float mix, uint32 frames, float frequency, TConfig &&configure)
	{
		auto dsp = std::make_unique<TDSP>(kSampleRate);
		dsp->mix = mix;
		configure(*dsp);

		TestAudioBase audio(kSampleRate);
		audio.Attach(dsp.get());

		const double step = 2.0 * 3.14159265358979323846 * frequency / kSampleRate;
		FilterRun run;
		run.output = RenderSignal(audio, frames, kBlockFrames, [step](uint32 i) {
			return static_cast<float>(std::sin(step * double(i))) * 0.5f;
		}, &run.input);

		audio.DetachAll();
		return run;
	}

	// Peak amplitude ratio between the output and the input over [fromFrame,
	// toFrame), which is the steady-state gain once the filter has settled.
	double MeasureGain(const FilterRun &run, uint32 fromFrame, uint32 toFrame)
	{
		double peakIn = 0.0;
		double peakOut = 0.0;
		for (uint32 i = fromFrame; i < toFrame; i++)
		{
			peakIn = Math::Max(peakIn, double(std::fabs(run.input[i * 2])));
			peakOut = Math::Max(peakOut, double(std::fabs(run.output[i * 2])));
		}
		return peakIn > 0.0 ? peakOut / peakIn : 0.0;
	}

	// ---------------------------------------------------------------------------
	// 1. The block clock
	// ---------------------------------------------------------------------------
	void BlockClockTest(TestContext &)
	{
		ProbeDSP probe(kSampleRate);
		TestAudioBase audio(kSampleRate);
		audio.Attach(&probe);

		// Non-uniform block sizes: a clock that is only set once, or only at the
		// start of a run, shows up immediately.
		const uint32 sizes[] = {384, 100, 7, 384, 1};
		const size_t numSizes = sizeof(sizes) / sizeof(sizes[0]);
		uint64 blockStart = 12345;

		std::vector<float> buffer(size_t(kBlockFrames) * 2, 1.0f);
		for (size_t index = 0; index < numSizes; index++)
		{
			const uint32 size = sizes[index];

			// Model the real decoder's ordering, which is the whole point of the
			// block clock: AudioStreamBase::Process decodes the entire block and
			// only then writes the stream position, so by the time the DSP chain
			// runs, GetSamplePos() already points at the *next* block. RenderBlock
			// keeps the two equal, so it cannot tell the two clocks apart; driving
			// ProcessDSPs with the position ahead of the block start can.
			audio.SetSamplePos(blockStart + size);
			audio.blockStartSample = blockStart;
			Require(audio.GetSamplePos() == blockStart + size, "the stream position is past the block, as in the mixer");

			std::fill(buffer.begin(), buffer.end(), 1.0f);
			audio.ProcessDSPs(buffer.data(), size);

			const uint64 streamPosition = audio.GetSamplePos();
			Require(probe.calls.size() == index + 1, "the DSP is called once per block");

			const ProbeDSP::Call &call = probe.calls.back();
			Require(call.numSamples == size, "the DSP sees the block size it was given");

			// This is the regression: GetCurrentSample() used to return the stream
			// position, which by then is the first frame of the *next* block.
			Require(call.clockSample == blockStart,
					"GetCurrentSample() is the first frame of the block being processed");
			Require(call.clockSample != streamPosition,
					"GetCurrentSample() is not the stream position, which has already moved past the block");
			RequireNear(double(streamPosition - call.clockSample), double(size), 0.0,
						"the stream position leads the block clock by exactly one block");

			blockStart += size;
		}

		audio.DetachAll();
	}

	// ---------------------------------------------------------------------------
	// 2. The effect window
	// ---------------------------------------------------------------------------
	void EffectWindowTest(TestContext &)
	{
		// (a) A block that contains endTime renders only up to it, and the frames
		// after it are untouched.
		{
			ProbeDSP probe(kSampleRate);
			probe.endTime = 100; // ms -> frame 4410

			TestAudioBase audio(kSampleRate);
			audio.Attach(&probe);

			const uint32 firstFrame = 4000;
			const uint32 endFrame = uint32(uint64(probe.endTime) * kSampleRate / 1000);
			audio.SetSamplePos(firstFrame);

			std::vector<float> buffer(size_t(kBlockFrames) * 2, 1.0f);
			uint32 processed = 0;
			for (uint32 block = 0; block < 3; block++)
			{
				std::fill(buffer.begin(), buffer.end(), 1.0f);
				audio.RenderBlock(buffer.data(), kBlockFrames);

				// The probe writes 0.5 where it ran, so a processed frame is exactly
				// 0.5 and an untouched one is still 1.0. They must form a prefix:
				// a hole in the middle, or a touched frame after an untouched one,
				// means the truncation is wrong.
				uint32 touched = 0;
				while (touched < kBlockFrames && std::fabs(buffer[touched * 2] - 0.5f) < 1e-6f)
				{
					touched++;
				}
				for (uint32 i = touched; i < kBlockFrames; i++)
				{
					RequireNear(double(buffer[i * 2]), 1.0, 0.0,
								"no frame after the effect's end is modified");
				}
				processed += touched;
			}

			RequireNear(double(processed), double(endFrame - firstFrame), 0.0,
						"the effect covers exactly [start, end) and not one frame more");
			RequireNear(double(probe.calls[0].numSamples), double(kBlockFrames), 0.0,
						"a block fully inside the window is processed whole");
			RequireNear(double(probe.calls[1].numSamples), double(endFrame - (firstFrame + kBlockFrames)), 0.0,
						"the block containing endTime is truncated to the end frame");
			Require(probe.calls.size() == 2, "no later block runs a DSP whose end has passed");

			audio.DetachAll();
		}

		// (b) endTime == 0 means no authored end: every block runs.
		{
			ProbeDSP probe(kSampleRate);
			probe.endTime = 0;

			TestAudioBase audio(kSampleRate);
			audio.Attach(&probe);

			std::vector<float> buffer(size_t(kBlockFrames) * 2, 1.0f);
			for (uint32 block = 0; block < 3; block++)
			{
				std::fill(buffer.begin(), buffer.end(), 1.0f);
				audio.RenderBlock(buffer.data(), kBlockFrames);
			}

			Require(probe.calls.size() == 3, "endTime == 0 is unbounded");
			for (const ProbeDSP::Call &call : probe.calls)
			{
				Require(call.numSamples == kBlockFrames, "an unbounded effect renders whole blocks");
			}

			audio.DetachAll();
		}

		// (c) An effect scheduled well ahead of the render cursor is skipped, not
		// rendered early. The slack below is the documented tolerance: startTime is
		// chart time and the clock is stream time, so a calibration offset must not
		// turn the start bound into a delay.
		{
			ProbeDSP probe(kSampleRate);
			probe.startTime = 1000; // ms -> frame 44100

			TestAudioBase audio(kSampleRate);
			audio.Attach(&probe);

			std::vector<float> buffer(size_t(kBlockFrames) * 2, 1.0f);
			audio.SetSamplePos(0);
			audio.RenderBlock(buffer.data(), kBlockFrames);

			Require(probe.calls.empty(), "a DSP more than the start slack ahead is not run");
			RequireNear(double(buffer[0]), 1.0, 0.0, "a DSP that is not run leaves the signal alone");

			audio.DetachAll();
		}

		// (d) endTime on a block boundary: the whole block runs, the next one does not.
		{
			ProbeDSP probe(kSampleRate);
			probe.endTime = 800; // ms -> frame 35280

			TestAudioBase audio(kSampleRate);
			audio.Attach(&probe);

			const uint32 endFrame = uint32(uint64(probe.endTime) * kSampleRate / 1000);
			audio.SetSamplePos(endFrame - kBlockFrames);

			std::vector<float> buffer(size_t(kBlockFrames) * 2, 1.0f);
			audio.RenderBlock(buffer.data(), kBlockFrames);
			Require(probe.calls.size() == 1 && probe.calls[0].numSamples == kBlockFrames,
					"the block that ends exactly on endTime runs whole");

			std::fill(buffer.begin(), buffer.end(), 1.0f);
			audio.RenderBlock(buffer.data(), kBlockFrames);
			Require(probe.calls.size() == 1, "the block that starts on endTime does not run");
			RequireNear(double(buffer[0]), 1.0, 0.0, "the block after endTime is untouched");

			audio.DetachAll();
		}
	}

	// ---------------------------------------------------------------------------
	// 3. The gate ramp, against the transcribed reference
	// ---------------------------------------------------------------------------
	void GateRampTest(TestContext &)
	{
		using audio_audit::reference::GateDSP;
		using audio_audit::reference::GateParams;

		// 46.875 ms is 3/64 s: exact in binary, and it lands on 2067 whole frames at
		// 44.1 kHz on both sides (2067.1875 truncated). Any difference between the
		// two renders is therefore the ramp, not a disagreement about the period.
		constexpr double kLengthMs = 46.875;
		const uint32 period = uint32(kLengthMs / 1000.0 * kSampleRate);
		const uint32 compareFrames = period * 2 + 40;

		struct Case
		{
			float rate;
			float mix;
			const char *name;
		};

		// 0.5: an ordinary gate. 12/2067: the shortest window that still releases.
		// 0.25 with mix 0.5: the mix path, so a wrong blend shows up too.
		const Case cases[] = {
			{0.5f, 1.0f, "1/16 gate at rate 0.5"},
			{0.25f, 0.5f, "rate 0.25 at mix 0.5"},
			{12.0f / float(period), 1.0f, "shortest window that still releases"},
		};

		for (const Case &c : cases)
		{
			auto clone = std::make_unique<::GateDSP>(kSampleRate);
			clone->SetLength(kLengthMs);
			clone->SetGating(c.rate);
			clone->mix = c.mix;

			TestAudioBase audio(kSampleRate);
			audio.Attach(clone.get());
			const std::vector<float> cloneOut = RenderSignal(audio, compareFrames, kBlockFrames,
															[](uint32) { return 1.0f; });
			audio.DetachAll();

			GateDSP reference(audio_audit::reference::DSPCommonInfo(kSampleRate, 2));
			GateParams params;
			params.secUntilTrigger = -1.0f;
			params.waveLength = float(kLengthMs / 1000.0);
			params.rate = c.rate;
			params.mix = c.mix;

			std::vector<float> referenceOut(size_t(compareFrames) * 2, 1.0f);
			reference.UpdateParams(params);
			reference.Process(referenceOut.data(), referenceOut.size(), false, params);

			for (uint32 i = 0; i < compareFrames; i++)
			{
				const double got = cloneOut[i * 2];
				const double want = referenceOut[i * 2];
				if (std::fabs(got - want) > 1e-6)
				{
					throw TestFailure(Utility::Sprintf(
						"%s: frame %u of %.1f ms is %.9f, ksmaudio's Scale() gives %.9f",
						c.name, i, kLengthMs, got, want));
				}
			}

			const uint32 halfway = uint32(float(period) * c.rate);
			// The gate writes scale * mix + (1 - mix), so every expected value has to
			// carry the blend as well as the envelope.
			auto blended = [&c](double scale) { return scale * double(c.mix) + (1.0 - double(c.mix)); };

			RequireNear(double(cloneOut[0]), blended(1.0), 1e-6,
						"the gate is at full gain on its first frame: there is no attack ramp");
			if (halfway > 12)
			{
				RequireNear(double(cloneOut[halfway * 2]), blended(12.0 / 13.0), 1e-6,
							"the release begins on the frame after the window at 12/13");
				RequireNear(double(cloneOut[(halfway + 11) * 2]), blended(1.0 / 13.0), 1e-6,
							"the last release frame is 1/13");
				RequireNear(double(cloneOut[(halfway + 12) * 2]), blended(0.0), 1e-6,
							"the frame after the release is silent");
			}
			else
			{
				RequireNear(double(cloneOut[halfway * 2]), blended(0.0), 1e-6,
							"a window of 12 frames or fewer is hard-edged: no release ramp");
			}
		}

		// And the rule itself, independent of rendering: the guard is what makes a
		// short gate keep its level instead of spending it on a ramp wider than the
		// window.
		Require(audio_audit::reference::Scale(12U, 100U, 12U) == 0.0f,
				"reference: a 12-frame window has no release");
		Require(audio_audit::reference::Scale(12U, 100U, 13U) == 1.0f,
				"reference: a 13-frame window is still open on frame 12");
	}

	// ---------------------------------------------------------------------------
	// 4. The echo mix law
	// ---------------------------------------------------------------------------
	void EchoMixLawTest(TestContext &)
	{
		constexpr double kLengthMs = 100.0;
		const uint32 delayFrames = uint32(kLengthMs / 1000.0 * kSampleRate); // 4410

		// (a) mix 0 is transparent, bit for bit.
		{
			auto echo = std::make_unique<EchoDSP>(kSampleRate);
			echo->SetLength(kLengthMs);
			echo->feedback = 0.6f;
			echo->mix = 0.0f;

			TestAudioBase audio(kSampleRate);
			audio.Attach(echo.get());

			const double step = 2.0 * 3.14159265358979323846 * 440.0 / kSampleRate;
			FilterRun run;
			run.output = RenderSignal(audio, delayFrames * 2, kBlockFrames, [step](uint32 i) {
				return static_cast<float>(std::sin(step * double(i))) * 0.5f;
			}, &run.input);
			audio.DetachAll();

			for (size_t i = 0; i < run.output.size(); i++)
			{
				if (run.output[i] != run.input[i])
				{
					throw TestFailure(Utility::Sprintf("echo at mix 0 changed sample %zu: %.9f -> %.9f", i,
													   double(run.input[i]), double(run.output[i])));
				}
			}
		}

		// (b) Impulse response: the delay line stores dry + delayed * feedback, and
		// the output at mix 1 is the stored signal one length ago. Repeat k is
		// therefore feedback^(k-1): the decay is the feedback and nothing else.
		{
			auto echo = std::make_unique<EchoDSP>(kSampleRate);
			echo->SetLength(kLengthMs);
			echo->feedback = 0.6f;
			echo->mix = 1.0f;

			TestAudioBase audio(kSampleRate);
			audio.Attach(echo.get());

			const std::vector<float> out = RenderSignal(audio, delayFrames * 6, kBlockFrames,
														[](uint32 i) { return i == 0 ? 1.0f : 0.0f; });
			audio.DetachAll();

			RequireNear(double(out[0]), 0.0, 0.0, "at mix 1 the first length is silent: the line is empty");
			RequireNear(double(out[delayFrames * 2]), 1.0, 1e-6, "the first repeat is the dry input");
			for (uint32 k = 2; k <= 5; k++)
			{
				const double want = std::pow(0.6, double(k - 1));
				RequireNear(double(out[k * delayFrames * 2]), want, 1e-5,
							"repeat k decays by feedback per repeat, not by feedback * mix");
			}
		}

		// (c) mix 0.5 blends dry and repeat.
		{
			auto echo = std::make_unique<EchoDSP>(kSampleRate);
			echo->SetLength(kLengthMs);
			echo->feedback = 0.6f;
			echo->mix = 0.5f;

			TestAudioBase audio(kSampleRate);
			audio.Attach(echo.get());

			const std::vector<float> out = RenderSignal(audio, delayFrames * 2, kBlockFrames,
														[](uint32 i) { return i == 0 ? 1.0f : 0.0f; });
			audio.DetachAll();

			RequireNear(double(out[0]), 0.5, 1e-6, "mix 0.5 passes half the dry signal");
			RequireNear(double(out[delayFrames * 2]), 0.5, 1e-6, "mix 0.5 passes half the first repeat");
		}

		// (d) feedback 1 must not run away: the loop gain is held just below unity.
		{
			auto echo = std::make_unique<EchoDSP>(kSampleRate);
			echo->SetLength(kLengthMs);
			echo->feedback = 1.0f;
			echo->mix = 1.0f;

			TestAudioBase audio(kSampleRate);
			audio.Attach(echo.get());

			const std::vector<float> out = RenderSignal(audio, delayFrames * 20, kBlockFrames,
														[](uint32 i) { return i == 0 ? 1.0f : 0.0f; });
			audio.DetachAll();

			double peak = 0.0;
			for (float sample : out)
			{
				peak = Math::Max(peak, double(std::fabs(sample)));
			}
			Require(peak <= 1.0 + 1e-6, "feedback 1.0 is clamped so the delay line cannot grow without bound");
			Require(peak > 0.9, "the first repeat is still at full level with feedback 1.0");
		}
	}

	// ---------------------------------------------------------------------------
	// 5. The biquad mix law
	// ---------------------------------------------------------------------------
	void BqfMixLawTest(TestContext &)
	{
		// The law, asserted between three parallel runs of the same filter on the
		// same input: out(mix) == dry + (wet - dry) * mix. This is phase
		// independent, so it holds for a low-pass as well as a peaking filter, and
		// it cannot be satisfied by folding mix into the filter's gain or Q.
		auto checkLaw = [](FilterRun &dry, FilterRun &half, FilterRun &wet, const char *what) {
			for (size_t i = 0; i < dry.output.size(); i++)
			{
				const double want = double(dry.output[i]) +
									(double(wet.output[i]) - double(dry.output[i])) * 0.5;
				if (std::fabs(double(half.output[i]) - want) > 1e-6)
				{
					throw TestFailure(Utility::Sprintf("%s: sample %zu at mix 0.5 is %.9f, the blend law gives %.9f",
													   what, i, double(half.output[i]), want));
				}
			}
		};

		const uint32 frames = kSampleRate; // one second, so the filter settles

		// (a) low-pass: mix 0 transparent, blend law holds.
		{
			auto config = [](BQFDSP &dsp) { dsp.SetLowPass(0.7f, 800.0f); };
			FilterRun dry = RunFilter<BQFDSP>(0.0f, frames, 4000.0f, config);
			FilterRun half = RunFilter<BQFDSP>(0.5f, frames, 4000.0f, config);
			FilterRun wet = RunFilter<BQFDSP>(1.0f, frames, 4000.0f, config);

			for (size_t i = 0; i < dry.output.size(); i++)
			{
				if (dry.output[i] != dry.input[i])
				{
					throw TestFailure(Utility::Sprintf(
						"low-pass at mix 0 changed sample %zu: %.9f -> %.9f", i, double(dry.input[i]),
						double(dry.output[i])));
				}
			}
			checkLaw(dry, half, wet, "low-pass 800 Hz at 4 kHz");

			// Sanity on the endpoints themselves: 4 kHz is well above the cutoff.
			Require(MeasureGain(wet, frames - 2048, frames) < 0.1,
					"the low-pass at mix 1 is still a low-pass");
			Require(MeasureGain(half, frames - 2048, frames) > 0.4,
					"at mix 0.5 the dry signal keeps most of its level");
		}

		// (b) high-pass.
		{
			auto config = [](BQFDSP &dsp) { dsp.SetHighPass(0.7f, 8000.0f); };
			FilterRun dry = RunFilter<BQFDSP>(0.0f, frames, 500.0f, config);
			FilterRun half = RunFilter<BQFDSP>(0.5f, frames, 500.0f, config);
			FilterRun wet = RunFilter<BQFDSP>(1.0f, frames, 500.0f, config);

			checkLaw(dry, half, wet, "high-pass 8 kHz at 500 Hz");
			Require(MeasureGain(wet, frames - 2048, frames) < 0.02,
					"the high-pass at mix 1 rejects 500 Hz");
		}

		// (c) peaking: at the centre frequency the phase is zero, so the nominal
		// gain shows up in the amplitude and the blend law has a known value.
		{
			constexpr float kCentre = 1000.0f;
			auto config = [](BQFDSP &dsp) { dsp.SetPeaking(4.0f, kCentre, 6.0f); };
			FilterRun dry = RunFilter<BQFDSP>(0.0f, frames, kCentre, config);
			FilterRun half = RunFilter<BQFDSP>(0.5f, frames, kCentre, config);
			FilterRun wet = RunFilter<BQFDSP>(1.0f, frames, kCentre, config);

			checkLaw(dry, half, wet, "peaking +6 dB at 1 kHz");

			const double gainWetDb = 20.0 * std::log10(MeasureGain(wet, frames - 2048, frames));
			const double gainHalfDb = 20.0 * std::log10(MeasureGain(half, frames - 2048, frames));
			RequireNear(gainWetDb, 6.0, 0.3, "a +6 dB peaking filter at mix 1 really is +6 dB at its centre");
			// 0.5 * (1 + 10^(6/20)) = +3.51 dB, the number the baseline records.
			RequireNear(gainHalfDb, 20.0 * std::log10(0.5 * (1.0 + std::pow(10.0, 6.0 / 20.0))), 0.05,
						"at mix 0.5 the peaking gain is the lerp of dry and wet, +3.51 dB");
		}

		// (d) the combined filter crossfades once, after both stages: handing mix to
		// each stage would blend twice and a 50% mix would produce 25%.
		{
			auto config = [](CombinedFilterDSP &dsp) { dsp.SetLowPass(0.7f, 800.0f, 4.0f, 6.0f); };
			FilterRun dry = RunFilter<CombinedFilterDSP>(0.0f, frames, 4000.0f, config);
			FilterRun half = RunFilter<CombinedFilterDSP>(0.5f, frames, 4000.0f, config);
			FilterRun wet = RunFilter<CombinedFilterDSP>(1.0f, frames, 4000.0f, config);

			for (size_t i = 0; i < dry.output.size(); i++)
			{
				if (dry.output[i] != dry.input[i])
				{
					throw TestFailure(Utility::Sprintf(
						"combined filter at mix 0 changed sample %zu: %.9f -> %.9f", i, double(dry.input[i]),
						double(dry.output[i])));
				}
			}
			checkLaw(dry, half, wet, "combined low-pass plus peaking");
		}
	}

	struct AuditEntry
	{
		const char *name;
		void (*function)(TestContext &);
	};

	const AuditEntry kAuditEntries[] = {
		{"AudioBlockClock", BlockClockTest},
		{"AudioEffectWindow", EffectWindowTest},
		{"AudioGateRamp", GateRampTest},
		{"AudioEchoMixLaw", EchoMixLawTest},
		{"AudioBqfMixLaw", BqfMixLawTest},
	};

	// Runs every check even when one fails, so a single run reports the whole
	// picture instead of stopping at the first red line, then fails the test.
	void AuditSuiteTest(TestContext &context)
	{
		uint32 failed = 0;
		for (const AuditEntry &entry : kAuditEntries)
		{
			try
			{
				entry.function(context);
				Logf("  %-20s ok", Logger::Severity::Info, entry.name);
			}
			catch (const TestFailure &failure)
			{
				failed++;
				Logf("  %-20s FAIL %s", Logger::Severity::Error, entry.name, failure.expression);
			}
		}

		if (failed)
		{
			throw TestFailure(Utility::Sprintf("%u of %u audio audit checks failed", failed,
											   uint32(sizeof(kAuditEntries) / sizeof(kAuditEntries[0]))));
		}
	}

	TestEntry s_AuditSuite("AudioAudit", AuditSuiteTest);
	TestEntry s_BlockClock("AudioBlockClock", BlockClockTest);
	TestEntry s_EffectWindow("AudioEffectWindow", EffectWindowTest);
	TestEntry s_GateRamp("AudioGateRamp", GateRampTest);
	TestEntry s_EchoMixLaw("AudioEchoMixLaw", EchoMixLawTest);
	TestEntry s_BqfMixLaw("AudioBqfMixLaw", BqfMixLawTest);
}

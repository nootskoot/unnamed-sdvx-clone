/*
	Baseline renderer for w4: the clone's DSPs measured against the ksmaudio
	reference transcriptions, one CSV row per metric.

	This file is built twice by build_baseline.sh:

	  before   against git HEAD's Audio/src/{DSP,AudioBase}.cpp and headers,
	           the tree as it was before any of the timing or mix fixes landed
	  after    against the working tree

	Measuring both is the point. A single run cannot tell a fixed law from a law
	that was always there, and the "after" side is a moving target while the
	other workers are still editing Audio/src/DSP.cpp.

	How the block model works
	-------------------------
	Audio_Impl::Mix decodes a whole block first and only then runs the DSP chain,
	and AudioStreamBase::Process has already written the stream position past the
	dropped block by the time ProcessDSPs runs. The stub below reproduces exactly
	that ordering: before ProcessDSPs it moves the stream position to the END of
	the block and, when the AudioBase under test has one, sets blockStartSample to
	the START of the block. So:

	  before   DSP::GetCurrentSample() reads GetSamplePos()      -> block end
	  after    DSP::GetCurrentSample() reads blockStartSample    -> block start

	which is the defect and the fix, measured rather than asserted.

	Usage
	  Tests.Audio.baseline before out/before.csv
	  Tests.Audio.baseline after  out/after.csv
*/

#include <Shared/Shared.hpp>
#include <Audio/AudioBase.hpp>
#include <Audio/DSP.hpp>

#include "reference/GateDSP.hpp"
#include "reference/RetriggerEchoDSP.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
	constexpr uint32 kSampleRate = 44100;
	// Audio_Impl::m_sampleBufferLength. The whole point of the clock measurement
	// is that this is the quantum the defect is measured in.
	constexpr uint32 kBlock = 384;

	// Block start of the block currently being handed to ProcessDSPs. The probes
	// read it so a call can be placed on the timeline without trusting the
	// clock that is itself under test.
	uint64 g_blockStart = 0;

	template <typename T, typename = void>
	struct HasBlockStartSample : std::false_type
	{
	};

	template <typename T>
	struct HasBlockStartSample<T, std::void_t<decltype(std::declval<T &>().blockStartSample)>> : std::true_type
	{
	};

	double Db(double ratio, double floorRatio = 1e-9)
	{
		return 20.0 * std::log10(std::max(ratio, floorRatio));
	}

	// ------------------------------------------------------------------
	// AudioBase stub
	// ------------------------------------------------------------------

	class Stub final : public AudioBase
	{
	public:
		explicit Stub(uint32 sampleRate = kSampleRate) : m_rate(sampleRate ? sampleRate : kSampleRate) {}

		~Stub() override
		{
			DetachAll();
		}

		Stub(const Stub &) = delete;
		Stub &operator=(const Stub &) = delete;

		void Process(float *out, uint32 numSamples) override { ProcessDSPs(out, numSamples); }

		[[nodiscard]] int32 GetPosition() const override
		{
			return static_cast<int32>(m_pos * 1000 / m_rate);
		}

		[[nodiscard]] uint32 GetSampleRate() const override { return m_rate; }
		[[nodiscard]] uint64 GetSamplePos() const override { return m_pos; }
		[[nodiscard]] float *GetPCM() override { return nullptr; }
		[[nodiscard]] uint64 GetPCMCount() const override { return 0; }
		void PreRenderDSPs(Vector<DSP *> &) override {}

		void Attach(DSP *dsp)
		{
			if (!dsp)
				return;
			DSPs.AddUnique(dsp);
			dsp->SetAudioBase(this);
		}

		void DetachAll()
		{
			for (DSP *dsp : DSPs)
			{
				if (dsp)
					dsp->RemoveAudioBase();
			}
			DSPs.clear();
		}

		// One mixer block: decoded first (position lands past it), then the chain.
		template <typename B = AudioBase>
		void RenderBlock(float *out, uint32 frames)
		{
			m_pos = m_blockStart + frames;
			if constexpr (HasBlockStartSample<B>::value)
			{
				// Dependent on B, so this branch is only checked when the AudioBase
				// under test actually has the field. The HEAD build has none.
				static_cast<B *>(this)->blockStartSample = m_blockStart;
			}
			g_blockStart = m_blockStart;
			ProcessDSPs(out, frames);
			m_blockStart += frames;
		}

		uint64 m_blockStart = 0;

	private:
		uint32 m_rate;
		uint64 m_pos = 0;
	};

	// ------------------------------------------------------------------
	// Probes
	// ------------------------------------------------------------------

	class ClockProbe final : public DSP
	{
	public:
		struct Observed
		{
			uint32 current;
			uint32 start;
			uint32 frames;
		};

		void Process(float *, uint32 numSamples) override
		{
			Observed o{};
			o.current = GetCurrentSample();
			o.start = GetStartSample();
			o.frames = numSamples;
			observations.push_back(o);
		}

		[[nodiscard]] const char *GetName() const override { return "ClockProbe"; }

		std::vector<Observed> observations;
	};

	class WindowProbe final : public DSP
	{
	public:
		struct Call
		{
			uint64 blockStart;
			uint32 frames;
		};

		void Process(float *, uint32 numSamples) override { calls.push_back({g_blockStart, numSamples}); }
		[[nodiscard]] const char *GetName() const override { return "WindowProbe"; }

		std::vector<Call> calls;
	};

	// ------------------------------------------------------------------
	// Gate envelope
	// ------------------------------------------------------------------

	struct Envelope
	{
		int period = 0;
		int firstNonZero = 0;
		int openFrames = 0;
		int releaseFrames = 0;
		int riseFrames = 0;
		int partialFrames = 0;
		int zeroFrames = 0;
		int smearFrames = 0; // nonzero frames that are not part of the open window
		double peakDb = 0.0;
		double energyDb = 0.0;
	};

	// Classifies one steady-state period of a gain trace.
	//   rise    partial-gain frames before the first full-gain frame
	//   release partial-gain frames after the last full-gain frame
	Envelope Classify(const std::vector<float> &trace, int from, int to)
	{
		Envelope e{};
		e.period = to - from;
		if (e.period <= 0)
			return e;

		int firstOpen = -1;
		int lastOpen = -1;
		for (int i = from; i < to; i++)
		{
			const float v = trace[static_cast<size_t>(i)];
			if (v >= 0.999f)
			{
				if (firstOpen < 0)
					firstOpen = i;
				lastOpen = i;
				e.openFrames++;
			}
			else if (v > 1e-4f)
			{
				e.partialFrames++;
			}
			else
			{
				e.zeroFrames++;
			}
		}

		for (int i = from; i < to; i++)
		{
			if (trace[static_cast<size_t>(i)] > 1e-4f)
			{
				e.firstNonZero = i - from;
				break;
			}
		}

		if (firstOpen >= 0)
		{
			for (int i = from; i < firstOpen; i++)
				if (trace[static_cast<size_t>(i)] > 1e-4f)
					e.riseFrames++;
			for (int i = lastOpen + 1; i < to; i++)
				if (trace[static_cast<size_t>(i)] > 1e-4f)
					e.releaseFrames++;
		}
		else
		{
			e.riseFrames = -1;
			e.releaseFrames = -1;
		}

		e.smearFrames = e.partialFrames;

		double peak = 0.0;
		double sumSquares = 0.0;
		for (int i = from; i < to; i++)
		{
			const double v = trace[static_cast<size_t>(i)];
			peak = std::max(peak, v);
			sumSquares += v * v;
		}
		e.peakDb = Db(peak);
		e.energyDb = 10.0 * std::log10(std::max(sumSquares / e.period, 1e-12));
		return e;
	}

	// Renders `frames` frames of a constant 1.0 signal through blockFn and
	// returns the per-frame output, which for a multiplicative effect is its gain.
	std::vector<float> RenderTrace(int frames, const std::function<void(float *, uint32)> &blockFn)
	{
		std::vector<float> trace(static_cast<size_t>(frames), 0.0f);
		std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
		for (int done = 0; done < frames; done += static_cast<int>(kBlock))
		{
			const uint32 count = static_cast<uint32>(std::min<int>(static_cast<int>(kBlock), frames - done));
			std::fill(buf.begin(), buf.end(), 1.0f);
			blockFn(buf.data(), count);
			for (uint32 i = 0; i < count; i++)
				trace[static_cast<size_t>(done) + i] = buf[static_cast<size_t>(i) * 2];
		}
		return trace;
	}

	// ------------------------------------------------------------------
	// CSV sink
	// ------------------------------------------------------------------

	FILE *g_out = nullptr;

	void Row(const char *build, const char *section, const char *label, const char *metric, double value)
	{
		if (!g_out)
			return;
		fprintf(g_out, "%s,%s,%s,%s,%.6f\n", build, section, label, metric, value);
	}

	void RowS(const char *build, const char *section, const char *label, const char *metric, const char *value)
	{
		if (!g_out)
			return;
		fprintf(g_out, "%s,%s,%s,%s,%s\n", build, section, label, metric, value);
	}

	// ------------------------------------------------------------------
	// M1: block clock
	// ------------------------------------------------------------------

	void MeasureClock(const char *build)
	{
		ClockProbe probe;
		{
			Stub stub;
			stub.Attach(&probe);
			std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
			for (int b = 0; b < 3; b++)
				stub.RenderBlock(buf.data(), kBlock);
			stub.DetachAll();
		}

		for (size_t i = 0; i < probe.observations.size(); i++)
		{
			const char *label = i == 0 ? "block0" : (i == 1 ? "block1" : "block2");
			char metric[64];
			snprintf(metric, sizeof(metric), "currentSample_call%zu", i);
			Row(build, "clock", label, metric, static_cast<double>(probe.observations[i].current));
			snprintf(metric, sizeof(metric), "expectedBlockStart_call%zu", i);
			Row(build, "clock", label, metric, static_cast<double>(i) * kBlock);
		}

		// A DSP created mid-stream and attached: what does it see on its first block?
		ClockProbe late;
		{
			Stub stub;
			stub.m_blockStart = 44100; // one second in
			stub.Attach(&late);
			std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
			stub.RenderBlock(buf.data(), kBlock);
			stub.DetachAll();
		}
		Row(build, "clock", "mid_stream_1s", "currentSample_firstCall",
			static_cast<double>(late.observations.empty() ? 0u : late.observations[0].current));
		Row(build, "clock", "mid_stream_1s", "expectedBlockStart", 44100.0);
	}

	// ------------------------------------------------------------------
	// M2: gate envelope
	// ------------------------------------------------------------------

	struct GateCase
	{
		const char *name;
		double bpm;
		int divider; // 0 = sixteenth, 1 = eighth, 2 = thirty-second
		float rate;
	};

	void MeasureGate(const char *build)
	{
		const GateCase cases[] = {
			{"G1_200bpm_16_50", 200.0, 0, 0.50f},
			{"G2_200bpm_16_75", 200.0, 0, 0.75f},
			{"G3_200bpm_8_50", 200.0, 1, 0.50f},
			{"G4_150bpm_16_25", 150.0, 0, 0.25f},
			{"G5_200bpm_32_50", 200.0, 2, 0.50f},
			{"G6_200bpm_16_05", 200.0, 0, 0.05f},
		};

		for (const GateCase &c : cases)
		{
			// A 1/4 note is 60000/BPM ms; a 1/16 is a quarter of that.
			const double quarterMs = 60000.0 / c.bpm;
			const double periodMs = c.divider == 0 ? quarterMs / 4.0 : (c.divider == 1 ? quarterMs / 2.0 : quarterMs / 8.0);
			const int periodFrames = static_cast<int>(periodMs / 1000.0 * kSampleRate);
			const int frames = periodFrames * 3;

			// Clone
			{
				GateDSP gate(kSampleRate);
				gate.mix = 1.0f;
				gate.low = 0.0f;
				gate.SetGating(c.rate);
				gate.SetLength(periodMs);

				Stub stub;
				stub.Attach(&gate);
				std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
				std::vector<float> trace;
				trace.reserve(static_cast<size_t>(frames));
				for (int done = 0; done < frames; done += static_cast<int>(kBlock))
				{
					const uint32 count = static_cast<uint32>(std::min<int>(static_cast<int>(kBlock), frames - done));
					std::fill(buf.begin(), buf.end(), 1.0f);
					stub.RenderBlock(buf.data(), count);
					for (uint32 i = 0; i < count; i++)
						trace.push_back(buf[static_cast<size_t>(i) * 2]);
				}
				stub.DetachAll();

				const Envelope e = Classify(trace, periodFrames, periodFrames * 2);
				Row(build, "gate", c.name, "periodFrames", static_cast<double>(periodFrames));
				Row(build, "gate", c.name, "firstNonZero", static_cast<double>(e.firstNonZero));
				Row(build, "gate", c.name, "openFrames", static_cast<double>(e.openFrames));
				Row(build, "gate", c.name, "releaseFrames", static_cast<double>(e.releaseFrames));
				Row(build, "gate", c.name, "riseFrames", static_cast<double>(e.riseFrames));
				Row(build, "gate", c.name, "partialFrames", static_cast<double>(e.partialFrames));
				Row(build, "gate", c.name, "zeroFrames", static_cast<double>(e.zeroFrames));
				Row(build, "gate", c.name, "peakDb", e.peakDb);
				Row(build, "gate", c.name, "energyDb", e.energyDb);
			}

			// Reference
			{
				audio_audit::reference::GateDSP ref({kSampleRate, 2});
				audio_audit::reference::GateParams params;
				params.waveLength = static_cast<float>(periodMs / 1000.0);
				params.rate = c.rate;
				params.mix = 1.0f;
				ref.UpdateParams(params);

				std::vector<float> trace = RenderTrace(frames, [&](float *buf, uint32 count) {
					ref.Process(buf, static_cast<size_t>(count) * 2, false, params);
				});

				const Envelope e = Classify(trace, periodFrames, periodFrames * 2);
				const std::string label = std::string(c.name) + "_ref";
				Row(build, "gate", label.c_str(), "periodFrames", static_cast<double>(periodFrames));
				Row(build, "gate", label.c_str(), "firstNonZero", static_cast<double>(e.firstNonZero));
				Row(build, "gate", label.c_str(), "openFrames", static_cast<double>(e.openFrames));
				Row(build, "gate", label.c_str(), "releaseFrames", static_cast<double>(e.releaseFrames));
				Row(build, "gate", label.c_str(), "riseFrames", static_cast<double>(e.riseFrames));
				Row(build, "gate", label.c_str(), "partialFrames", static_cast<double>(e.partialFrames));
				Row(build, "gate", label.c_str(), "zeroFrames", static_cast<double>(e.zeroFrames));
				Row(build, "gate", label.c_str(), "peakDb", e.peakDb);
				Row(build, "gate", label.c_str(), "energyDb", e.energyDb);
			}
		}
	}

	// ------------------------------------------------------------------
	// M3: echo
	// ------------------------------------------------------------------

	struct EchoCase
	{
		const char *name;
		double lengthMs;
		float feedback;
		float mix;
	};

	void MeasureEcho(const char *build)
	{
		const EchoCase cases[] = {
			{"E1_150ms_fb060_mix050", 150.0, 0.60f, 0.50f},
			{"E2_150ms_fb060_mix100", 150.0, 0.60f, 1.00f},
			{"E3_075ms_fb080_mix050", 75.0, 0.80f, 0.50f},
			{"E4_300ms_fb040_mix070", 300.0, 0.40f, 0.70f},
			{"E5_150ms_fb060_mix000", 150.0, 0.60f, 0.00f},
		};

		for (const EchoCase &c : cases)
		{
			const uint32 loopFrames = static_cast<uint32>(c.lengthMs / 1000.0 * kSampleRate);
			const int repeats = 24;
			const int frames = static_cast<int>(loopFrames) * repeats;

			// Clone
			{
				EchoDSP echo(kSampleRate);
				echo.SetLength(c.lengthMs);
				echo.feedback = c.feedback;
				echo.mix = c.mix;

				Stub stub;
				stub.Attach(&echo);
				std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
				std::vector<float> out(static_cast<size_t>(frames), 0.0f);
				int produced = 0;
				for (int done = 0; done < frames; done += static_cast<int>(kBlock))
				{
					const uint32 count = static_cast<uint32>(std::min<int>(static_cast<int>(kBlock), frames - done));
					std::fill(buf.begin(), buf.end(), 0.0f);
					// Single-frame impulse at frame 0.
					if (done == 0)
					{
						buf[0] = 1.0f;
						buf[1] = 1.0f;
					}
					stub.RenderBlock(buf.data(), count);
					for (uint32 i = 0; i < count; i++)
						out[static_cast<size_t>(produced++)] = buf[static_cast<size_t>(i) * 2];
				}
				stub.DetachAll();

				Row(build, "echo", c.name, "levelDryFrame0", Db(std::fabs(out[0])));
				if (getenv("BASELINE_DEBUG"))
					fprintf(stderr, "[dbg] %s %s first=%g,%g,%g,%g\n", build, c.name, out[0], out[1], out[2], out[3]);
				double previous = 0.0;
				int toSixty = -1;
				double sumDecay = 0.0;
				int decayCount = 0;
				for (int k = 1; k < repeats; k++)
				{
					double peak = 0.0;
					const size_t from = static_cast<size_t>(k) * loopFrames;
					const size_t to = std::min<size_t>(from + loopFrames, out.size());
					for (size_t i = from; i < to; i++)
						peak = std::max(peak, static_cast<double>(std::fabs(out[i])));
					if (k <= 6)
					{
						char metric[48];
						snprintf(metric, sizeof(metric), "repeat%dDb", k);
						Row(build, "echo", c.name, metric, Db(peak));
					}
					if (previous > 0.0 && peak > 0.0)
					{
						sumDecay += 20.0 * std::log10(peak / previous);
						decayCount++;
					}
					if (toSixty < 0 && peak < 0.001)
						toSixty = k;
					previous = peak;
				}
				Row(build, "echo", c.name, "decayPerRepeatDb", decayCount ? sumDecay / decayCount : 0.0);
				Row(build, "echo", c.name, "repeatsTo60dB", static_cast<double>(toSixty));
				Row(build, "echo", c.name, "tailSeconds",
					toSixty < 0 ? -1.0 : static_cast<double>(toSixty) * c.lengthMs / 1000.0);

				// Largest sample-to-sample jump right after each loop boundary.
				double worstStep = 0.0;
				for (int k = 1; k < 6; k++)
				{
					const size_t at = static_cast<size_t>(k) * loopFrames;
					if (at + 1 < out.size())
						worstStep = std::max(worstStep, static_cast<double>(std::fabs(out[at] - out[at - 1])));
				}
				Row(build, "echo", c.name, "loopBoundaryStep", worstStep);
			}

			// Reference
			{
				audio_audit::reference::RetriggerEchoDSP ref({kSampleRate, 2});
				const auto params = audio_audit::reference::MakeEchoParams(
					static_cast<float>(c.lengthMs / 1000.0), c.feedback, c.mix);

				std::vector<float> out(static_cast<size_t>(frames), 0.0f);
				int produced = 0;
				std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
				for (int done = 0; done < frames; done += static_cast<int>(kBlock))
				{
					const uint32 count = static_cast<uint32>(std::min<int>(static_cast<int>(kBlock), frames - done));
					std::fill(buf.begin(), buf.end(), 0.0f);
					if (done == 0)
					{
						buf[0] = 1.0f;
						buf[1] = 1.0f;
					}
					ref.Process(buf.data(), static_cast<size_t>(count) * 2, false, params);
					for (uint32 i = 0; i < count; i++)
						out[static_cast<size_t>(produced++)] = buf[static_cast<size_t>(i) * 2];
				}

				const std::string label = std::string(c.name) + "_ref";
				Row(build, "echo", label.c_str(), "levelDryFrame0", Db(std::fabs(out[0])));
				double previous = 0.0;
				int toSixty = -1;
				double sumDecay = 0.0;
				int decayCount = 0;
				for (int k = 1; k < repeats; k++)
				{
					double peak = 0.0;
					const size_t from = static_cast<size_t>(k) * loopFrames;
					const size_t to = std::min<size_t>(from + loopFrames, out.size());
					for (size_t i = from; i < to; i++)
						peak = std::max(peak, static_cast<double>(std::fabs(out[i])));
					if (k <= 6)
					{
						char metric[48];
						snprintf(metric, sizeof(metric), "repeat%dDb", k);
						Row(build, "echo", label.c_str(), metric, Db(peak));
					}
					if (previous > 0.0 && peak > 0.0)
					{
						sumDecay += 20.0 * std::log10(peak / previous);
						decayCount++;
					}
					if (toSixty < 0 && peak < 0.001)
						toSixty = k;
					previous = peak;
				}
				Row(build, "echo", label.c_str(), "decayPerRepeatDb", decayCount ? sumDecay / decayCount : 0.0);
				Row(build, "echo", label.c_str(), "repeatsTo60dB", static_cast<double>(toSixty));
				Row(build, "echo", label.c_str(), "tailSeconds",
					toSixty < 0 ? -1.0 : static_cast<double>(toSixty) * c.lengthMs / 1000.0);
			}
		}
	}

	// ------------------------------------------------------------------
	// M3b: does the dry signal survive at all?
	// ------------------------------------------------------------------

	// An impulse cannot answer this: after the first frame the dry input is zero
	// anyway. A continuous tone can. If the effect overwrites its output with the
	// wet signal, mix 0 silences the channel and the level collapses.
	void MeasureEchoDry(const char *build)
	{
		struct Case
		{
			const char *name;
			float mix;
		};
		const Case cases[] = {
			{"D1_tone_mix000", 0.00f},
			{"D2_tone_mix050", 0.50f},
			{"D3_tone_mix100", 1.00f},
		};

		const int frames = static_cast<int>(kSampleRate * 1.0);
		const int settle = static_cast<int>(kSampleRate * 0.25);

		for (const Case &c : cases)
		{
			EchoDSP echo(kSampleRate);
			echo.SetLength(150.0);
			echo.feedback = 0.6f;
			echo.mix = c.mix;

			Stub stub;
			stub.Attach(&echo);
			std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
			double phase = 0.0;
			double inSquares = 0.0;
			double outSquares = 0.0;
			int counted = 0;
			const double step = 2.0 * 3.14159265358979323846 * 1000.0 / kSampleRate;
			const double ref = 1e-9;

			for (int done = 0; done < frames; done += static_cast<int>(kBlock))
			{
				const uint32 count = static_cast<uint32>(std::min<int>(static_cast<int>(kBlock), frames - done));
				for (uint32 i = 0; i < count; i++)
				{
					const float v = static_cast<float>(std::sin(phase));
					buf[static_cast<size_t>(i) * 2] = v;
					buf[static_cast<size_t>(i) * 2 + 1] = v;
					phase += step;
				}
				stub.RenderBlock(buf.data(), count);
				for (uint32 i = 0; i < count; i++)
				{
					if (done + static_cast<int>(i) < settle)
						continue;
					const double in = std::sin(phase - step * (count - i)); // not used, kept out of the metric
					(void)in;
					const double o = buf[static_cast<size_t>(i) * 2];
					outSquares += o * o;
					inSquares += 0.5; // a unit sine's mean square
					counted++;
				}
			}
			stub.DetachAll();

			const double outRms = counted ? std::sqrt(outSquares / counted) : 0.0;
			const double inRms = counted ? std::sqrt(inSquares / counted) : 1.0;
			Row(build, "echo", c.name, "outRmsDb", Db(outRms / inRms, ref));
		}
	}

	// ------------------------------------------------------------------
	// M4: biquad mix law
	// ------------------------------------------------------------------

	double SineGain(DSP &dsp, double freq, int settleFrames, int measureFrames)
	{
		Stub stub;
		stub.Attach(&dsp);
		std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
		double phase = 0.0;
		const double step = 2.0 * 3.14159265358979323846 * freq / kSampleRate;

		double sumSquares = 0.0;
		int counted = 0;
		const int total = settleFrames + measureFrames;
		for (int done = 0; done < total; done += static_cast<int>(kBlock))
		{
			const uint32 count = static_cast<uint32>(std::min<int>(static_cast<int>(kBlock), total - done));
			for (uint32 i = 0; i < count; i++)
			{
				const float v = static_cast<float>(std::sin(phase));
				buf[static_cast<size_t>(i) * 2] = v;
				buf[static_cast<size_t>(i) * 2 + 1] = v;
				phase += step;
			}
			stub.RenderBlock(buf.data(), count);
			for (uint32 i = 0; i < count; i++)
			{
				if (done + static_cast<int>(i) >= settleFrames)
				{
					const double v = buf[static_cast<size_t>(i) * 2];
					sumSquares += v * v;
					counted++;
				}
			}
		}
		stub.DetachAll();
		if (!counted)
			return 0.0;
		const double rms = std::sqrt(sumSquares / counted);
		const double inputRms = 1.0 / std::sqrt(2.0);
		return Db(rms / inputRms);
	}

	struct BqfCase
	{
		const char *name;
		int kind; // 0 peaking, 1 lowpass, 2 highpass
		float q;
		float freq;
		float gain;
		double probeHz;
		float mix;
	};

	void MeasureBqf(const char *build)
	{
		const BqfCase cases[] = {
			{"P_1k_p6_q4_mix000", 0, 4.0f, 1000.0f, 6.0f, 1000.0, 0.00f},
			{"P_1k_p6_q4_mix050", 0, 4.0f, 1000.0f, 6.0f, 1000.0, 0.50f},
			{"P_1k_p6_q4_mix100", 0, 4.0f, 1000.0f, 6.0f, 1000.0, 1.00f},
			{"L_800_q07_mix000", 1, 0.7f, 800.0f, 0.0f, 4000.0, 0.00f},
			{"L_800_q07_mix050", 1, 0.7f, 800.0f, 0.0f, 4000.0, 0.50f},
			{"H_8k_q07_mix000", 2, 0.7f, 8000.0f, 0.0f, 500.0, 0.00f},
			{"H_8k_q07_mix050", 2, 0.7f, 8000.0f, 0.0f, 500.0, 0.50f},
		};

		const int settle = static_cast<int>(kSampleRate * 0.5);
		const int measure = static_cast<int>(kSampleRate * 0.25);

		for (const BqfCase &c : cases)
		{
			BQFDSP dsp(kSampleRate);
			if (c.kind == 0)
				dsp.SetPeaking(c.q, c.freq, c.gain);
			else if (c.kind == 1)
				dsp.SetLowPass(c.q, c.freq);
			else
				dsp.SetHighPass(c.q, c.freq);
			dsp.mix = c.mix;

			Row(build, "bqf", c.name, "gainDb", SineGain(dsp, c.probeHz, settle, measure));
		}
	}

	// ------------------------------------------------------------------
	// M6: onset relative to startTime
	// ------------------------------------------------------------------

	// A constant input cannot show when a gate opens, only when it closes: an
	// open gate multiplies by 1.0. So this measures the first frame the gate
	// closes, against the frame its own period says it should close on.
	void MeasureOnset(const char *build)
	{
		const uint32 startMs = 1000;
		const uint32 startSample = static_cast<uint32>(static_cast<double>(startMs) / 1000.0 * kSampleRate);
		const uint32 periodFrames = static_cast<uint32>(75.0 / 1000.0 * kSampleRate); // 1/16 at 200 BPM
		const uint32 halfway = periodFrames / 2;

		GateDSP gate(kSampleRate);
		gate.mix = 1.0f;
		gate.low = 0.0f;
		gate.SetGating(0.5f);
		gate.SetLength(75.0);
		gate.startTime = startMs;

		Stub stub;
		stub.Attach(&gate);
		std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
		long firstClosed = -1;
		int frame = 0;
		const int total = static_cast<int>(kSampleRate * 1.2);
		for (int done = 0; done < total; done += static_cast<int>(kBlock))
		{
			const uint32 count = static_cast<uint32>(std::min<int>(static_cast<int>(kBlock), total - done));
			std::fill(buf.begin(), buf.end(), 1.0f);
			stub.RenderBlock(buf.data(), count);
			for (uint32 i = 0; i < count; i++, frame++)
			{
				if (firstClosed < 0 && std::fabs(buf[static_cast<size_t>(i) * 2] - 1.0f) > 1e-4f)
					firstClosed = frame;
			}
		}
		stub.DetachAll();

		const long expected = static_cast<long>(startSample) + static_cast<long>(halfway);
		Row(build, "onset", "gate_startTime_1000ms", "firstClosedFrame", static_cast<double>(firstClosed));
		Row(build, "onset", "gate_startTime_1000ms", "expectedClosedFrame", static_cast<double>(expected));
		Row(build, "onset", "gate_startTime_1000ms", "closedEarlyByFrames",
			static_cast<double>(expected - firstClosed));
	}

	// ------------------------------------------------------------------
	// M5: effect window
	// ------------------------------------------------------------------

	void MeasureWindow(const char *build)
	{
		WindowProbe probe;
		probe.startTime = 1000;
		probe.endTime = 1500;

		Stub stub;
		stub.Attach(&probe);
		std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
		const int totalMs = 2000;
		const int blocks = static_cast<int>(static_cast<double>(totalMs) / 1000.0 * kSampleRate / kBlock);
		for (int b = 0; b < blocks; b++)
			stub.RenderBlock(buf.data(), kBlock);
		stub.DetachAll();

		const uint64 startSample = static_cast<uint64>(1000.0 / 1000.0 * kSampleRate);
		const uint64 endSample = static_cast<uint64>(1500.0 / 1000.0 * kSampleRate);

		uint64 total = 0;
		uint64 beforeStart = 0;
		uint64 afterEnd = 0;
		for (const WindowProbe::Call &call : probe.calls)
		{
			total += call.frames;
			const uint64 callEnd = call.blockStart + call.frames;
			if (callEnd <= startSample)
				beforeStart += call.frames;
			if (call.blockStart >= endSample)
				afterEnd += call.frames;
			else if (callEnd > endSample)
				afterEnd += callEnd - endSample;
		}
		Row(build, "window", "hold_1000_to_1500ms", "framesProcessedTotal", static_cast<double>(total));
		Row(build, "window", "hold_1000_to_1500ms", "framesBeforeStart", static_cast<double>(beforeStart));
		Row(build, "window", "hold_1000_to_1500ms", "framesAfterEnd", static_cast<double>(afterEnd));
		Row(build, "window", "hold_1000_to_1500ms", "expectedFrames", (1500.0 - 1000.0) / 1000.0 * kSampleRate);

		// endTime == 0 must stay unbounded.
		WindowProbe unbounded;
		unbounded.startTime = 0;
		unbounded.endTime = 0;
		Stub stub2;
		stub2.Attach(&unbounded);
		for (int b = 0; b < blocks; b++)
			stub2.RenderBlock(buf.data(), kBlock);
		stub2.DetachAll();
		uint64 unboundedTotal = 0;
		for (const WindowProbe::Call &call : unbounded.calls)
			unboundedTotal += call.frames;
		Row(build, "window", "no_end_time", "framesProcessedTotal", static_cast<double>(unboundedTotal));
		Row(build, "window", "no_end_time", "expectedFrames", static_cast<double>(blocks) * kBlock);
	}
}

int main(int argc, char **argv)
{
	const char *build = argc > 1 ? argv[1] : "unknown";
	const char *path = argc > 2 ? argv[2] : nullptr;

	if (path)
	{
		g_out = fopen(path, "w");
		if (!g_out)
		{
			fprintf(stderr, "baseline: cannot open %s for writing\n", path);
			return 2;
		}
	}
	else
	{
		g_out = stdout;
	}

	fprintf(g_out, "build,section,case,metric,value\n");
	RowS(build, "meta", "harness", "blockFrames", std::to_string(kBlock).c_str());
	RowS(build, "meta", "harness", "sampleRate", std::to_string(kSampleRate).c_str());
#ifdef USC_BASELINE_HAS_BLOCK_START
	RowS(build, "meta", "harness", "audioBaseHasBlockStartSample", "1");
#else
	RowS(build, "meta", "harness", "audioBaseHasBlockStartSample", "0");
#endif

	MeasureClock(build);
	MeasureGate(build);
	MeasureEcho(build);
	MeasureEchoDry(build);
	MeasureBqf(build);
	MeasureOnset(build);
	MeasureWindow(build);

	if (path)
		fclose(g_out);

	return 0;
}

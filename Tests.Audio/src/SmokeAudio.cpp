#include "stdafx.h"
#include "SmokeAudio.hpp"
#include "TestAudioBase.hpp"

#include <Audio/DSP.hpp>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace
{
	constexpr uint32 kSampleRate = 44100;
	constexpr uint32 kBlockFrames = 384;
	constexpr uint32 kBlocks = 8;

	struct SmokeResult
	{
		String name;
		bool ok = true;
		float peak = 0.0f;
		float rms = 0.0f;
		uint32 blocksRendered = 0;
		// A DSP that must pass signal at the settings below. Silence from one of
		// these is a failure, not a quiet render: the silent-failure class of bug
		// (a DSP that returns early on a null buffer, an unset length or a stale
		// mix) shows up as peak == 0 and nothing else. Set false only where
		// silence is the correct output for the settings given.
		bool expectAudible = true;
	};

	// Renders <kBlocks> blocks through one DSP attached to the stub AudioBase.
	// Ownership of the DSP passes in as a unique_ptr so it is destroyed before the
	// stub AudioBase: AudioBase::~AudioBase asserts DSPs is empty and DSP::~DSP
	// asserts it is unbound, and the stub detaches on destruction.
	template <typename TDSP, typename TSetup>
	SmokeResult RunOwned(const char *label, std::unique_ptr<TDSP> dsp, TSetup &&setup)
	{
		SmokeResult result;
		result.name = label;

		dsp->mix = 1.0f;
		setup(*dsp);

		TestAudioBase audio(kSampleRate);
		audio.Attach(dsp.get());

		std::vector<float> buffer(kBlockFrames * 2, 0.0f);
		double sumSquares = 0.0;
		uint64 sampleCount = 0;

		for (uint32 b = 0; b < kBlocks; b++)
		{
			audio.FillSine(buffer.data(), kBlockFrames, 440.0f, 0.5f);
			audio.RenderBlock(buffer.data(), kBlockFrames);
			result.blocksRendered++;

			for (uint32 i = 0; i < kBlockFrames * 2; i++)
			{
				const float sample = buffer[i];
				if (!std::isfinite(sample))
				{
					result.ok = false;
					continue;
				}
				const float magnitude = std::fabs(sample);
				if (magnitude > result.peak)
					result.peak = magnitude;
				sumSquares += double(sample) * double(sample);
				sampleCount++;
			}
		}

		result.rms = sampleCount ? float(std::sqrt(sumSquares / double(sampleCount))) : 0.0f;

		// Silence is only a pass where a DSP is meant to be silent here.
		if (result.expectAudible && !(result.peak > 0.0f))
			result.ok = false;

		audio.DetachAll();
		return result;
	}

	// DSPs whose constructor takes the sample rate (everything but PanDSP).
	template <typename TDSP, typename TSetup>
	SmokeResult RunOne(const char *label, TSetup &&setup)
	{
		return RunOwned<TDSP>(label, std::make_unique<TDSP>(kSampleRate), std::forward<TSetup>(setup));
	}

	// DSPs with no constructor of their own, so the implicit default one is used.
	template <typename TDSP, typename TSetup>
	SmokeResult RunOneDefault(const char *label, TSetup &&setup)
	{
		return RunOwned<TDSP>(label, std::make_unique<TDSP>(), std::forward<TSetup>(setup));
	}

	void PrintResult(const SmokeResult &result)
	{
		Logf("  %-22s %s  blocks=%u  peak=%.4f  rms=%.4f",
			 result.ok ? Logger::Severity::Info : Logger::Severity::Error,
			 result.name.c_str(), result.ok ? "ok  " : "FAIL", result.blocksRendered,
			 result.peak, result.rms);
	}
}

int RunAudioSmoke()
{
	Logf("Tests.Audio: DSP smoke render (%u Hz, %u frames/block, %u blocks)",
		 Logger::Severity::Info, kSampleRate, kBlockFrames, kBlocks);

	std::vector<SmokeResult> results;

	results.push_back(RunOne<BQFDSP>("BQFDSP", [](BQFDSP &dsp) {
		dsp.SetLowPass(1.0f, 1000.0f);
	}));

	results.push_back(RunOne<CombinedFilterDSP>("CombinedFilterDSP", [](CombinedFilterDSP &dsp) {
		dsp.SetLowPass(1.0f, 1000.0f, 1.0f, 0.0f);
	}));

	results.push_back(RunOne<GateDSP>("GateDSP", [](GateDSP &dsp) {
		dsp.SetLength(100.0);
		dsp.SetGating(0.5f);
	}));

	results.push_back(RunOne<BitCrusherDSP>("BitCrusherDSP", [](BitCrusherDSP &dsp) {
		dsp.SetPeriod(0.25f);
	}));

	// Fully wet (mix = 1, set by RunOwned) with a 20 ms delay: the repeat lands
	// 882 frames in, well inside the 3072 frames rendered, so the wet path is
	// actually exercised. At the 80 ms used before, the delay was longer than the
	// whole render and a wet-only echo was correctly but uselessly silent.
	results.push_back(RunOne<EchoDSP>("EchoDSP", [](EchoDSP &dsp) {
		dsp.SetLength(20.0);
		dsp.feedback = 0.5f;
	}));

	results.push_back(RunOne<RetriggerDSP>("RetriggerDSP", [](RetriggerDSP &dsp) {
		dsp.SetLength(100.0);
		dsp.SetGating(0.5f);
	}));

	results.push_back(RunOne<TapeStopDSP>("TapeStopDSP", [](TapeStopDSP &dsp) {
		dsp.SetKsmSpeed(0.5f);
	}));

	results.push_back(RunOne<WobbleDSP>("WobbleDSP", [](WobbleDSP &dsp) {
		dsp.SetLength(100.0);
	}));

	results.push_back(RunOne<PhaserDSP>("PhaserDSP", [](PhaserDSP &dsp) {
		dsp.SetLength(100.0);
		dsp.SetStage(6);
	}));

	results.push_back(RunOne<FlangerDSP>("FlangerDSP", [](FlangerDSP &dsp) {
		dsp.SetLength(100.0);
		dsp.SetDelayRange(10, 200);
		dsp.SetFeedback(0.5f);
		dsp.SetStereoWidth(0.5f);
		dsp.SetVolume(1.0f);
	}));

	results.push_back(RunOne<SidechainDSP>("SidechainDSP", [](SidechainDSP &dsp) {
		dsp.SetLength(200.0);
		dsp.SetAttackTime(10.0);
		dsp.SetHoldTime(20.0);
		dsp.SetReleaseTime(80.0);
	}));

	results.push_back(RunOne<LimiterDSP>("LimiterDSP", [](LimiterDSP &) {
	}));

	results.push_back(RunOneDefault<PanDSP>("PanDSP", [](PanDSP &dsp) {
		dsp.panning = 0.5f;
	}));

	results.push_back(RunOne<PitchShiftDSP>("PitchShiftDSP", [](PitchShiftDSP &dsp) {
		dsp.amount = 1.0f;
	}));

	uint32 failures = 0;
	for (const SmokeResult &result : results)
	{
		PrintResult(result);
		if (!result.ok)
			failures++;
	}

	// A DSP that ignores the signal entirely is as broken as one that overflows it.
	// Failures here are a non-finite sample, or silence from a DSP the table marks
	// expectAudible at these settings.
	if (failures != 0)
	{
		Logf("Tests.Audio: %u DSP(s) failed (non-finite output, or silent where the table expects signal)",
			 Logger::Severity::Error, failures);
		return 1;
	}

	Logf("Tests.Audio: all %u DSPs rendered %u blocks cleanly", Logger::Severity::Info,
		 uint32(results.size()), kBlocks);
	return 0;
}

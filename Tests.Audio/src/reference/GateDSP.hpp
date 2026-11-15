#pragma once
/*
	Golden reference: ksmaudio GateDSP, transcribed.

	Source, read 2026-11-14 from kshootmania/ksmaudio at
	5c2466d181aba37e3dc3c3eba6368f59b6843bcd:
		src/AudioEffect/DSP/GateDSP.cpp
		include/ksmaudio/AudioEffect/DSP/GateDSP.hpp
		include/ksmaudio/AudioEffect/Params/GateParams.hpp

	Kept here so the clone's GateDSP can be compared against the arithmetic it is
	meant to reproduce, rather than against a description of it.

	Semantics that the clone does not currently share, all visible in Scale():

	1. There is no attack ramp. A gated frame is at full gain from the first
	   frame of the window. The clone ramps in over 5% of the period.
	2. The release sits *after* the window, not inside it. Full gain lasts for
	   all numNonZeroFrames, then the gain falls over kDeclickFrames = 12 frames
	   with the divisor (kDeclickFrames + 1) = 13, so the last release frame is
	   1/13 and the frame after it is silence. The clone spends its ramp inside
	   the open window, which is why a short gate loses most of its level.
	3. The release is skipped entirely when numNonZeroFrames <= 12. Short gates
	   are therefore hard-edged on purpose. Adding a ramp there would make them
	   quieter than upstream, which is the opposite of the reported symptom.
	4. mix is applied per frame as Lerp(1, scale, mix), including while bypassed
	   the phase still advances (AdvanceBatch) so a re-enable lands on the beat.

	Units: upstream waveLength is in SECONDS (numPeriodFrames = waveLength *
	sampleRate). The clone's GateDSP::SetLength takes milliseconds. Any harness
	that feeds both must convert, or every gate comes out 1000x too slow.
*/

#include <cstddef>
#include "ReferenceCommon.hpp"

namespace audio_audit::reference
{
	// up: GateDSPParams
	struct GateParams
	{
		float secUntilTrigger = -1.0f; // Negative value is ignored
		float waveLength = 0.0f;       // seconds
		float rate = 0.5f;             // fraction of the period that is open
		float mix = 0.9f;
	};

	// up: GateDSP.cpp, anonymous namespace, verbatim arithmetic.
	// Exposed (and constexpr) so the tests can assert the boundary behaviour
	// without rendering audio.
	constexpr float Scale(std::size_t framesSincePrevTrigger, std::size_t numPeriodFrames,
		std::size_t numNonZeroFrames)
	{
		if (numPeriodFrames == 0U)
		{
			return 1.0f;
		}

		const std::size_t framesSincePeriodStart = framesSincePrevTrigger % numPeriodFrames;
		if (framesSincePeriodStart < numNonZeroFrames)
		{
			return 1.0f;
		}
		else if (framesSincePeriodStart < numNonZeroFrames + kDeclickFrames &&
			numNonZeroFrames > kDeclickFrames)
		{
			return static_cast<float>(kDeclickFrames - (framesSincePeriodStart - numNonZeroFrames)) /
				(kDeclickFrames + 1U);
		}
		else
		{
			return 0.0f;
		}
	}

	// Compile-time proof of the four claims above. If anyone "fixes" the ramp
	// into an attack ramp or drops the guard, this file stops building.
	namespace assertions
	{
		constexpr bool Near(float a, float b, float eps = 1e-6f)
		{
			return (a > b ? a - b : b - a) <= eps;
		}

		// 1. Full gain across the whole open window, first frame included.
		static_assert(Scale(0U, 100U, 50U) == 1.0f, "gate opens at full gain, no attack ramp");
		static_assert(Scale(49U, 100U, 50U) == 1.0f, "last open frame is still full gain");

		// 2. Release starts after the window and lasts kDeclickFrames, divided by 13.
		static_assert(Near(Scale(50U, 100U, 50U), 12.0f / 13.0f), "first release frame is 12/13");
		static_assert(Near(Scale(61U, 100U, 50U), 1.0f / 13.0f), "last release frame is 1/13");
		static_assert(Scale(62U, 100U, 50U) == 0.0f, "silence after the release");

		// 3. No release when the open window is 12 frames or shorter.
		static_assert(Scale(12U, 100U, 12U) == 0.0f, "no declick when numNonZeroFrames == 12");
		static_assert(Scale(11U, 100U, 12U) == 1.0f, "still full gain at frame 11 of a 12-frame window");
		static_assert(Scale(12U, 100U, 13U) == 1.0f, "13-frame window: frame 12 is still open");
		static_assert(Near(Scale(13U, 100U, 13U), 12.0f / 13.0f), "13-frame window does release");

		// Degenerate period: upstream returns full gain rather than dividing by zero.
		static_assert(Scale(0U, 0U, 0U) == 1.0f, "zero period is unity, not silence");
	}

	class GateDSP
	{
	private:
		const DSPCommonInfo m_info;
		SimpleTriggerHandler m_triggerHandler;

	public:
		explicit GateDSP(const DSPCommonInfo &info)
			: m_info(info)
		{
		}

		// up: GateDSP::process
		void Process(float *pData, std::size_t dataSize, bool bypass, const GateParams &params)
		{
			assert(dataSize % m_info.numChannels == 0);

			const std::size_t frameSize = dataSize / m_info.numChannels;

			if (bypass || params.mix == 0.0f)
			{
				m_triggerHandler.AdvanceBatch(frameSize);
				return;
			}

			const std::size_t numPeriodFrames = static_cast<std::size_t>(params.waveLength * m_info.sampleRate);
			const std::size_t numNonZeroFrames = static_cast<std::size_t>(numPeriodFrames * params.rate);

			for (std::size_t i = 0U; i < frameSize; ++i)
			{
				const float scale = Lerp(1.0f,
					Scale(m_triggerHandler.FramesSincePrevTrigger(), numPeriodFrames, numNonZeroFrames),
					params.mix);
				for (std::size_t ch = 0U; ch < m_info.numChannels; ++ch)
				{
					*pData *= scale;
					++pData;
				}
				m_triggerHandler.Advance();
			}
		}

		void UpdateParams(const GateParams &params)
		{
			m_triggerHandler.SetFramesUntilTrigger(params.secUntilTrigger, m_info.sampleRate);
		}

		void Reset()
		{
			m_triggerHandler.Reset();
		}

		// Test hooks. Not upstream API, but the harness has to be able to assert
		// that the phase came from the chart clock rather than from construction.
		[[nodiscard]] std::size_t FramesSincePrevTrigger() const
		{
			return m_triggerHandler.FramesSincePrevTrigger();
		}
		[[nodiscard]] std::ptrdiff_t FramesUntilTrigger() const
		{
			return m_triggerHandler.FramesUntilTrigger();
		}
		[[nodiscard]] std::size_t SampleRate() const { return m_info.sampleRate; }
		[[nodiscard]] std::size_t NumChannels() const { return m_info.numChannels; }
	};
}

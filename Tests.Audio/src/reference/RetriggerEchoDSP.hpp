#pragma once
/*
	Golden reference: ksmaudio RetriggerEchoDSP, transcribed.

	Source, read 2026-11-14 from kshootmania/ksmaudio at
	5c2466d181aba37e3dc3c3eba6368f59b6843bcd:
		src/AudioEffect/DSP/RetriggerEchoDSP.cpp
		include/ksmaudio/AudioEffect/DSP/RetriggerEchoDSP.hpp
		include/ksmaudio/AudioEffect/Params/RetriggerEchoParams.hpp
		include/ksmaudio/AudioEffect/detail/LinearBuffer.hpp

	This one class serves both upstream effects. EchoParams.renderByFX() sets
	rate = 1.0 and isEcho = true and takes feedback from the chart;
	RetriggerParams sets isEcho = false and feedbackLevel = 1.0. The topology is
	identical, which is why the clone's two separate DSPs (EchoDSP, RetriggerDSP)
	drift from it in different ways.

	Semantics the clone's EchoDSP does not share:

	1. The buffer is written with the DRY input first, then read: the wet material
	   is blended over the input with Lerp(input, wet, mix). The dry signal
	   survives at every mix value. The clone overwrites the output with
	   wet * mix once the buffer has wrapped once, so its echo is quieter than
	   upstream and, at mix < 1, has no dry at all.
	2. Feedback is a per-loop gain. At each loop boundary (read cursor stepping
	   over a whole numLoopFrames) m_currentFadeOutScale *= feedbackLevel, and in
	   echo mode the wet sample is additionally scaled by
	   (1.25 * loopFrames - k) / (1.25 * loopFrames). Nothing is written back into
	   the buffer. The clone writes out * feedback into the buffer every sample,
	   which compounds the feedback once per loop and scales it by mix.
	3. The loop length is measured from the block start, not from the note start:
	   the read cursor counts frames since the last cursor reset, and the trigger
	   handler resets it. That is the sample-accurate retrigger the clone lacks.
	4. A trigger falling inside a block splits the block in two and resets both
	   cursors at the split, so the retrigger lands on the exact frame. The clone
	   can only retrigger on a block boundary.
	5. Declick: 12 frames in, 12 frames out, with the same
	   numNonZeroFrames > 12 && numNonZeroFrames != numLoopFrames guard as the
	   gate. rate == 1.0 (echo) disables the declick entirely, by design.

	Units: waveLength is in SECONDS here, same as the gate.

	Buffer size is fixed at 10 seconds of frames, as upstream.
*/

#include <cstddef>
#include "ReferenceCommon.hpp"

namespace audio_audit::reference
{
	// up: RetriggerEchoDSPParams
	struct RetriggerEchoParams
	{
		float secUntilTrigger = -1.0f; // Negative value is ignored
		bool updateTrigger = false;
		float waveLength = 0.0f;       // seconds
		float rate = 0.7f;             // fraction of the loop that is audible
		bool isEcho = false;           // true for Echo, false for Retrigger
		float feedbackLevel = 1.0f;    // per-loop gain in echo mode
		float mix = 1.0f;
	};

	// Convenience constructors for the two parameter sets upstream derives in
	// Params/RetriggerEchoParams.hpp, so a harness can build either effect from
	// one place and still be comparing like with like.
	inline RetriggerEchoParams MakeEchoParams(float waveLengthSec, float feedbackLevel, float mix)
	{
		RetriggerEchoParams p;
		p.waveLength = waveLengthSec;
		p.rate = 1.0f;        // Echo pins rate to 1.0
		p.isEcho = true;
		p.feedbackLevel = feedbackLevel;
		p.mix = mix;
		return p;
	}

	inline RetriggerEchoParams MakeRetriggerParams(float waveLengthSec, float rate, float mix)
	{
		RetriggerEchoParams p;
		p.waveLength = waveLengthSec;
		p.rate = rate;
		p.isEcho = false;
		p.feedbackLevel = 1.0f; // Retrigger pins feedback to 1.0
		p.mix = mix;
		return p;
	}

	class RetriggerEchoDSP
	{
	private:
		const DSPCommonInfo m_info;
		LinearBuffer<float> m_linearBuffer;
		std::ptrdiff_t m_framesUntilTrigger = -1;

	public:
		explicit RetriggerEchoDSP(const DSPCommonInfo &info)
			: m_info(info)
			, m_linearBuffer(
				static_cast<std::size_t>(info.sampleRate) * 10 * info.numChannels, // 10 seconds
				info.numChannels)
		{
		}

		// up: RetriggerEchoDSP::process
		void Process(float *pData, std::size_t dataSize, bool bypass, const RetriggerEchoParams &params)
		{
			assert(dataSize % m_info.numChannels == 0);

			const std::size_t frameSize = dataSize / m_info.numChannels;
			const std::size_t numLoopFrames = static_cast<std::size_t>(params.waveLength * m_info.sampleRate);
			const std::size_t numNonZeroFrames = static_cast<std::size_t>(numLoopFrames * params.rate);

			if (m_framesUntilTrigger >= 0 && static_cast<std::size_t>(m_framesUntilTrigger) < frameSize)
			{
				// The trigger update falls inside this block: process the part before
				// it, reset the cursors, then the part after it.

				const std::size_t formerSize = static_cast<std::size_t>(m_framesUntilTrigger) * m_info.numChannels;
				m_linearBuffer.Write(pData, formerSize);
				m_linearBuffer.Read(pData, formerSize, numLoopFrames, numNonZeroFrames,
					params.isEcho, params.feedbackLevel, params.mix, bypass);

				m_linearBuffer.ResetReadWriteCursors();
				m_framesUntilTrigger = -1;

				const std::size_t latterSize = dataSize - formerSize;
				m_linearBuffer.Write(pData + formerSize, latterSize);
				m_linearBuffer.Read(pData + formerSize, latterSize, numLoopFrames, numNonZeroFrames,
					params.isEcho, params.feedbackLevel, params.mix, bypass);
			}
			else
			{
				// No trigger update in this block: process it in one pass.
				m_linearBuffer.Write(pData, dataSize);
				m_linearBuffer.Read(pData, dataSize, numLoopFrames, numNonZeroFrames,
					params.isEcho, params.feedbackLevel, params.mix, bypass);

				if (m_framesUntilTrigger >= static_cast<std::ptrdiff_t>(frameSize))
				{
					m_framesUntilTrigger -= static_cast<std::ptrdiff_t>(frameSize);
				}
			}
		}

		// up: RetriggerEchoDSP::updateParams
		void UpdateParams(const RetriggerEchoParams &params)
		{
			if (params.secUntilTrigger >= 0.0f) // negative values are ignored
			{
				const std::ptrdiff_t newFramesUntilTrigger =
					static_cast<std::ptrdiff_t>(params.secUntilTrigger * static_cast<float>(m_info.sampleRate));

				// Only shrink: avoids dropping a trigger when the next time arrives
				// just before the current one elapses.
				if (m_framesUntilTrigger < 0 || m_framesUntilTrigger > newFramesUntilTrigger)
				{
					m_framesUntilTrigger = newFramesUntilTrigger;
				}
			}

			// Note-triggered update ("update_trigger" going off>on, or on the note).
			if (params.updateTrigger)
			{
				m_linearBuffer.ResetReadWriteCursors();
			}
		}

		void Reset()
		{
			m_linearBuffer.ResetReadWriteCursors();
			m_framesUntilTrigger = -1;
		}

		// Test hooks.
		[[nodiscard]] std::ptrdiff_t FramesUntilTrigger() const { return m_framesUntilTrigger; }
		[[nodiscard]] std::size_t BufferNumFrames() const { return m_linearBuffer.NumFrames(); }
		[[nodiscard]] float FadeOutScale() const { return m_linearBuffer.CurrentFadeOutScale(); }
		[[nodiscard]] std::size_t SampleRate() const { return m_info.sampleRate; }
		[[nodiscard]] std::size_t NumChannels() const { return m_info.numChannels; }
	};
}

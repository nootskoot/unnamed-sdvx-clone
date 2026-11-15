#pragma once
/*
	Golden reference: shared ksmaudio primitives, transcribed.

	Source, read 2026-11-14 from kshootmania/ksmaudio at
	5c2466d181aba37e3dc3c3eba6368f59b6843bcd:
		include/ksmaudio/AudioEffect/detail/DSPSimpleTriggerHandler.hpp
		include/ksmaudio/AudioEffect/detail/LinearBuffer.hpp

	These types are deliberately independent of the clone's Audio/DSP.hpp and
	AudioBase.hpp. If the reference side of a comparison shared the clone's DSP
	base, a bug in that base would cancel out and the comparison would pass.
	Nothing here is vendored from ksmaudio's build; it is a transcription of the
	arithmetic, kept in this repository's style (PascalCase entry points, tabs).

	Only what GateDSP and RetriggerEchoDSP touch is transcribed. The parts of
	upstream LinearBuffer that exist for other DSPs (the plain copy-at-cursor
	read overload, the raw buffer() accessors) are left out on purpose.
*/

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

namespace audio_audit::reference
{
	// up: GateDSP.cpp, anonymous namespace
	inline constexpr std::size_t kDeclickFrames = 12U;

	// up: LinearBuffer.hpp
	inline constexpr std::size_t kLinearBufferDeclickFrames = 12U;

	// up: LinearBuffer.hpp
	// Fade-out length multiplier for echo mode, relative to the loop length
	// (longer than the loop length, so level never reaches zero at a loop end).
	inline constexpr float kLinearBufferEchoFadeOutLengthRate = 1.25f;

	// up: std::lerp(*pData, wet, mix), reproduced for C++17 targets.
	// Exact at the endpoints, monotonic in between. Endpoint exactness matters:
	// mix == 0 must leave a sample bit-identical, and mix == 1 must return wet.
	inline float Lerp(float a, float b, float t)
	{
		if (t <= 0.0f) return a;
		if (t >= 1.0f) return b;
		return a + (b - a) * t;
	}

	// up: DSPCommonInfo in AudioEffect.hpp, trimmed to the fields these two DSPs read.
	struct DSPCommonInfo
	{
		std::size_t sampleRate = 0U;
		std::size_t numChannels = 0U;

		constexpr DSPCommonInfo() = default;
		constexpr DSPCommonInfo(std::size_t sampleRate_, std::size_t numChannels_)
			: sampleRate(sampleRate_)
			, numChannels(numChannels_)
		{
		}
	};

	// up: detail/DSPSimpleTriggerHandler.hpp, transcribed as-is.
	// The countdown only ever shrinks (see SetFramesUntilTrigger), which is what
	// stops a retrigger from being skipped when a new, later time arrives just
	// before the current one elapses.
	class SimpleTriggerHandler
	{
	private:
		std::size_t m_framesSincePrevTrigger = 0U;
		std::ptrdiff_t m_framesUntilTrigger = -1;

	public:
		[[nodiscard]] const std::size_t &FramesSincePrevTrigger() const
		{
			return m_framesSincePrevTrigger;
		}

		void Advance()
		{
			++m_framesSincePrevTrigger;
			if (m_framesUntilTrigger < 0)
			{
				return;
			}
			if (--m_framesUntilTrigger == 0)
			{
				m_framesSincePrevTrigger = 0U;
			}
		}

		void AdvanceBatch(std::size_t frames)
		{
			if (m_framesUntilTrigger < 0)
			{
				m_framesSincePrevTrigger += frames;
			}
			else if (m_framesUntilTrigger > static_cast<std::ptrdiff_t>(frames))
			{
				m_framesSincePrevTrigger += frames;
				m_framesUntilTrigger -= static_cast<std::ptrdiff_t>(frames);
			}
			else
			{
				m_framesSincePrevTrigger = frames - static_cast<std::size_t>(m_framesUntilTrigger);
				m_framesUntilTrigger = -1;
			}
		}

		void Reset()
		{
			m_framesSincePrevTrigger = 0U;
			m_framesUntilTrigger = -1;
		}

		void SetFramesUntilTrigger(float secUntilTrigger, std::size_t sampleRate)
		{
			if (secUntilTrigger >= 0.0f) // Negative value is ignored
			{
				const std::ptrdiff_t newFramesUntilTrigger =
					static_cast<std::ptrdiff_t>(secUntilTrigger * static_cast<float>(sampleRate));

				// Only shrink, never grow: avoids dropping a trigger when the next
				// one is delivered a moment before the current one elapses.
				if (m_framesUntilTrigger < 0 || m_framesUntilTrigger > newFramesUntilTrigger)
				{
					m_framesUntilTrigger = newFramesUntilTrigger;
					if (m_framesUntilTrigger == 0)
					{
						m_framesSincePrevTrigger = 0U;
					}
				}
			}
		}

		[[nodiscard]] std::ptrdiff_t FramesUntilTrigger() const noexcept
		{
			return m_framesUntilTrigger;
		}
	};

	// up: detail/LinearBuffer.hpp, transcribed as-is.
	// The clone's EchoDSP and RetriggerDSP are the two things this replaces.
	// Note the ordering inside a block: Write() stores the *dry input* first,
	// then Read() blends the delayed material over it with Lerp(input, wet, mix).
	// The dry signal therefore survives, and feedback is a per-loop gain applied
	// to the fade-out scale, not a coefficient added back into the buffer.
	template <typename T>
	class LinearBuffer
	{
		static_assert(std::is_arithmetic_v<T>,
			"Value type of LinearBuffer is required to be arithmetic");

	private:
		std::vector<T> m_buffer;
		std::size_t m_readCursorFrame = 0U;
		std::size_t m_writeCursorFrame = 0U;
		float m_currentFadeOutScale = 1.0f;
		const std::size_t m_numFrames;
		const std::size_t m_numChannels;

	public:
		explicit LinearBuffer(std::size_t size, std::size_t numChannels)
			: m_buffer(size, T{0})
			, m_numFrames(numChannels == 0U ? 0U : size / numChannels)
			, m_numChannels(numChannels)
		{
			assert(m_numChannels > 0U);
			assert(size % m_numChannels == 0U);
			m_buffer.shrink_to_fit();
		}

		void Write(const T *pData, std::size_t size)
		{
			assert(size % m_numChannels == 0U);
			if (m_numFrames == 0U)
			{
				return;
			}
			const std::size_t frameSize = size / m_numChannels;
			const std::size_t numWriteFrames = (std::min)(frameSize, m_numFrames - m_writeCursorFrame);
			if (numWriteFrames == 0U)
			{
				return;
			}
			std::memcpy(&m_buffer[m_writeCursorFrame * m_numChannels], pData,
				sizeof(T) * numWriteFrames * m_numChannels);
			m_writeCursorFrame += numWriteFrames;
		}

		void Read(T *pData, std::size_t size, std::size_t numLoopFrames, std::size_t numNonZeroFrames,
			bool isEcho = false, float fadeOutFeedbackLevel = 1.0f, float mix = 1.0f, bool bypass = false)
		{
			assert(size % m_numChannels == 0U);
			const std::size_t frameSize = size / m_numChannels;

			if (m_numFrames == 0U)
			{
				return;
			}

			if (numLoopFrames == 0U || numLoopFrames > m_numFrames)
			{
				// Unusable loop length (an unset wave_length, say). Advance the read
				// cursor anyway so playback timing does not shift.
				m_readCursorFrame += frameSize;
				return;
			}

			if (bypass || mix == 0.0f)
			{
				// Same reasoning: keep the timing, change nothing.
				const std::size_t loopTimingCount =
					(m_readCursorFrame + frameSize) / numLoopFrames - m_readCursorFrame / numLoopFrames;
				m_readCursorFrame += frameSize;
				for (std::size_t i = 0U; i < loopTimingCount; ++i)
				{
					m_currentFadeOutScale *= fadeOutFeedbackLevel;
				}
				return;
			}

			if (numNonZeroFrames > numLoopFrames)
			{
				numNonZeroFrames = numLoopFrames;
			}

			// The guard that matters for both retrigger and gate de-click:
			// no ramp at all unless the open window is longer than the ramp.
			const bool canDeclick =
				numNonZeroFrames > kLinearBufferDeclickFrames && numNonZeroFrames != numLoopFrames;

			for (std::size_t frameIdx = 0U; frameIdx < frameSize; ++frameIdx)
			{
				const std::size_t readCursorFrameWithLoop = m_readCursorFrame % numLoopFrames;

				for (std::size_t ch = 0U; ch < m_numChannels; ++ch)
				{
					float wet;
					if (canDeclick && readCursorFrameWithLoop < kLinearBufferDeclickFrames)
					{
						// Declick (start)
						const float rate =
							static_cast<float>(readCursorFrameWithLoop + 1U) / (kLinearBufferDeclickFrames + 1U);
						wet = m_buffer[readCursorFrameWithLoop * m_numChannels + ch] * rate;
					}
					else if (readCursorFrameWithLoop < numNonZeroFrames)
					{
						wet = m_buffer[readCursorFrameWithLoop * m_numChannels + ch];
					}
					else if (canDeclick && readCursorFrameWithLoop < numNonZeroFrames + kLinearBufferDeclickFrames)
					{
						// Declick (end)
						const float rate = static_cast<float>(
							kLinearBufferDeclickFrames - (readCursorFrameWithLoop - numNonZeroFrames)) /
							(kLinearBufferDeclickFrames + 1U);
						wet = m_buffer[readCursorFrameWithLoop * m_numChannels + ch] * rate;
					}
					else
					{
						wet = T{0};
					}

					if (isEcho)
					{
						const float fadeOutLengthFrames = numLoopFrames * kLinearBufferEchoFadeOutLengthRate;
						const float fadeOutScale =
							(fadeOutLengthFrames - readCursorFrameWithLoop) / fadeOutLengthFrames * m_currentFadeOutScale;
						wet *= fadeOutScale;
					}

					*pData = Lerp(*pData, wet, mix);
					++pData;
				}
				++m_readCursorFrame;

				if (m_readCursorFrame % numLoopFrames == 0U)
				{
					m_currentFadeOutScale *= fadeOutFeedbackLevel;
				}
			}
		}

		void ResetFadeOutScale()
		{
			m_currentFadeOutScale = 1.0f;
		}

		void ResetReadCursor()
		{
			m_readCursorFrame = 0U;
			ResetFadeOutScale();
		}

		void ResetReadWriteCursors()
		{
			m_readCursorFrame = 0U;
			m_writeCursorFrame = 0U;
			ResetFadeOutScale();
		}

		[[nodiscard]] std::size_t Size() const { return m_buffer.size(); }
		[[nodiscard]] std::size_t NumFrames() const { return m_numFrames; }
		[[nodiscard]] float CurrentFadeOutScale() const { return m_currentFadeOutScale; }
	};
}

/*
	TestAudioBase: an AudioBase that renders DSPs with no audio device.

	Why this exists: the live path (AudioBase::AddDSP) takes Audio_Impl::lock and
	needs a running audio backend, so a DSP cannot be exercised without one.
	This stub lets a test construct a DSP directly, attach it here and call
	Process() / ProcessDSPs() against a sample position the test controls.

	Do not use AddDSP() from a test. It dereferences AudioBase::audio.
	Use Attach() instead.
*/
#pragma once

#include <Shared/Shared.hpp>
#include <Audio/AudioBase.hpp>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

namespace TestsAudio
{
	// True once AudioBase carries the block-start clock that the timing fix adds.
	// Detected instead of assumed so this harness compiles both before and after
	// that change lands, whichever order the two land in.
	template <typename T, typename = void>
	struct HasBlockStartSample : std::false_type
	{
	};

	template <typename T>
	struct HasBlockStartSample<T, std::void_t<decltype(std::declval<T &>().blockStartSample)>> : std::true_type
	{
	};
}

class TestAudioBase final : public AudioBase
{
public:
	explicit TestAudioBase(uint32 sampleRate = 44100) : m_sampleRate(sampleRate ? sampleRate : 44100)
	{
	}

	~TestAudioBase() override
	{
		// AudioBase::~AudioBase asserts DSPs is empty, and DSP::~DSP asserts the
		// DSP is no longer bound to an AudioBase. Both are the test's job here.
		DetachAll();
	}

	TestAudioBase(const TestAudioBase &) = delete;
	TestAudioBase &operator=(const TestAudioBase &) = delete;

	// ---- AudioBase ----

	void Process(float *out, uint32 numSamples) override
	{
		ProcessDSPs(out, numSamples);
	}

	[[nodiscard]] int32 GetPosition() const override
	{
		return static_cast<int32>(m_samplePos * 1000 / m_sampleRate);
	}

	[[nodiscard]] uint32 GetSampleRate() const override
	{
		return m_sampleRate;
	}

	// The value DSP::GetCurrentSample() reads. Settable, which is the whole point
	// of this stub: a test decides where in the stream a block begins.
	[[nodiscard]] uint64 GetSamplePos() const override
	{
		return m_samplePos;
	}

	[[nodiscard]] float *GetPCM() override
	{
		return nullptr;
	}

	[[nodiscard]] uint64 GetPCMCount() const override
	{
		return 0;
	}

	// The offline harness renders through RenderBlock() below. Pre-rendering a
	// stem is the job of the code under test, not of the stub.
	void PreRenderDSPs(Vector<DSP *> & /*dsps*/) override
	{
	}

	// ---- Harness API ----

	// Sets the playback position in samples, and the block-start clock if the
	// AudioBase under test has one.
	template <typename B = AudioBase>
	void SetSamplePos(uint64 position)
	{
		m_samplePos = position;
		if constexpr (TestsAudio::HasBlockStartSample<B>::value)
		{
			using ClockType = std::remove_reference_t<decltype(B::blockStartSample)>;
			this->blockStartSample = static_cast<ClockType>(position);
		}
	}

	// Convenience: position in milliseconds at the stub's sample rate.
	template <typename B = AudioBase>
	void SetPositionMs(double ms)
	{
		SetSamplePos<B>(static_cast<uint64>(std::llround(ms * m_sampleRate / 1000.0)));
	}

	void AdvanceSamples(uint64 frames)
	{
		SetSamplePos(m_samplePos + frames);
	}

	// Attaches a DSP without touching Audio_Impl::lock. Ordering matches
	// AudioBase::AddDSP so processing order in a test equals the live order.
	void Attach(DSP *dsp)
	{
		if (!dsp)
			return;

		DSPs.AddUnique(dsp);
		DSPs.Sort([](DSP *l, DSP *r) {
			if (l->priority == r->priority)
				return l < r;
			return l->priority < r->priority;
		});
		dsp->SetAudioBase(this);

		// Hand the DSP the current block start immediately, so GetCurrentSample()
		// is correct on its first Process() call rather than one block late.
		SetSamplePos(m_samplePos);
	}

	void Detach(DSP *dsp)
	{
		if (!dsp)
			return;
		DSPs.Remove(dsp);
		dsp->RemoveAudioBase();
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

	// Renders one block the way Audio_Impl::Mix does: the DSPs see the block
	// start, then the stream position advances past it.
	void RenderBlock(float *out, uint32 numFrames)
	{
		SetSamplePos(m_samplePos);
		ProcessDSPs(out, numFrames);
		SetSamplePos(m_samplePos + numFrames);
		m_framesRendered += numFrames;
	}

	// Fills an interleaved stereo buffer with a sine, so a DSP has something to
	// act on. Phase continues across calls.
	void FillSine(float *out, uint32 numFrames, float frequency, float amplitude = 0.5f)
	{
		const double step = 2.0 * 3.14159265358979323846 * frequency / m_sampleRate;
		for (uint32 i = 0; i < numFrames; i++)
		{
			const float sample = static_cast<float>(std::sin(m_phase)) * amplitude;
			out[i * 2] = sample;
			out[i * 2 + 1] = sample;
			m_phase += step;
			if (m_phase > 6.28318530717958647692)
				m_phase -= 6.28318530717958647692;
		}
	}

	[[nodiscard]] uint32 GetFramesRendered() const
	{
		return m_framesRendered;
	}

private:
	uint32 m_sampleRate = 44100;
	uint64 m_samplePos = 0;
	double m_phase = 0.0;
	uint32 m_framesRendered = 0;
};

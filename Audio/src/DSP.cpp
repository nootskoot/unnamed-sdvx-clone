#include "stdafx.h"
#include "DSP.hpp"
#include "AudioOutput.hpp"
#include "Audio_Impl.hpp"

void BQF::SetLowPass(float q, float freq, float sampleRate)
{
	// Limit q
    freq = Math::Clamp(freq, 1.0f, sampleRate * 0.49f);
	q = Math::Max(q, 0.01f);

	// Sampling frequency
	double w0 = (2 * Math::pi * freq) / sampleRate;
	double cw0 = cos(w0);
	float alpha = (float)(sin(w0) / (2 * q));

	b0 = (float)((1 - cw0) / 2);
	b1 = (float)(1 - cw0);
	b2 = (float)((1 - cw0) / 2);
	a0 = 1 + alpha;
	a1 = (float)(-2 * cw0);
	a2 = 1 - alpha;
}
void BQF::SetHighPass(float q, float freq, float sampleRate)
{
	// Limit q
    freq = Math::Clamp(freq, 1.0f, sampleRate * 0.49f);
	q = Math::Max(q, 0.01f);

	assert(freq < sampleRate);
	double w0 = (2 * Math::pi * freq) / sampleRate;
	double cw0 = cos(w0);
	float alpha = (float)(sin(w0) / (2 * q));

	b0 = (float)((1 + cw0) / 2);
	b1 = (float)-(1 + cw0);
	b2 = float((1 + cw0) / 2);
	a0 = 1 + alpha;
	a1 = (float)(-2 * cw0);
	a2 = 1 - alpha;
}
void BQF::SetAllPass(float q, float freq, float sampleRate)
{
	// Limit q
    freq = Math::Clamp(freq, 1.0f, sampleRate * 0.49f);
	q = Math::Max(q, 0.01f);

	double w0 = (2 * Math::pi * freq) / sampleRate;
	double cw0 = cos(w0);
	float alpha = (float)(sin(w0) / (2 * q));

	b0 = 1 - alpha;
	b1 = (float)(-2 * cw0);
	b2 = 1 + alpha;
	a0 = 1 + alpha;
	a1 = (float)(-2 * cw0);
	a2 = 1 - alpha;
}
void BQF::SetPeaking(float q, float freq, float gain, float sampleRate)
{
	// Limit q
    freq = Math::Clamp(freq, 1.0f, sampleRate * 0.49f);
	q = Math::Max(q, 0.01f);

	double w0 = (2 * Math::pi * freq) / sampleRate;
	double cw0 = cos(w0);
	float alpha = (float)(sin(w0) / (2 * q));
	double A = pow(10, (gain / 40));

	b0 = 1 + (float)(alpha * A);
	b1 = -2 * (float)cw0;
	b2 = 1 - (float)(alpha * A);
	a0 = 1 + (float)(alpha / A);
	a1 = -2 * (float)cw0;
	a2 = 1 - (float)(alpha / A);
}
void BQF::SetHighShelf(float q, float freq, float gain, float sampleRate)
{
	// Limit q
    freq = Math::Clamp(freq, 1.0f, sampleRate * 0.49f);
	q = Math::Max(q, 0.01f);

	double w0 = (2 * Math::pi * freq) / sampleRate;
	double cw0 = cos(w0);
	float alpha = (float)(sin(w0) / (2 * q));
	double A = pow(10, (gain / 40));
	double _2sqrtAalpha = 2 * sqrt(A) * alpha;

	b0 = A * ((A+1) + (A-1)*cw0 + _2sqrtAalpha);
	b1 = -2 * A * ((A-1) + (A+1)*cw0);
	b2 = A * ((A+1) + (A-1)*cw0 - _2sqrtAalpha);
	a0 = (A+1) - (A-1)*cw0 + _2sqrtAalpha;
	a1 = 2 * ((A-1) - (A+1)*cw0);
	a2 = (A+1) - (A-1)*cw0 - _2sqrtAalpha;
}
// RBJ audio-EQ cookbook low shelf, using the same Q convention as SetHighShelf.
void BQF::SetLowShelf(float q, float freq, float gain, float sampleRate)
{
    freq = Math::Clamp(freq, 1.f, sampleRate * .49f);
    q = Math::Max(q, .01f);
    const double w = 2 * Math::pi * freq / sampleRate;
    const double c = std::cos(w), A = std::pow(10., gain / 40.);
    const double t = std::sqrt(A) * std::sin(w) / q;
    b0 = A * ((A+1) - (A-1)*c + t);
    b1 = 2*A * ((A-1) - (A+1)*c);
    b2 = A * ((A+1) - (A-1)*c - t);
    a0 = (A+1) + (A-1)*c + t;
    a1 = -2 * ((A-1) + (A+1)*c);
    a2 = (A+1) + (A-1)*c - t;
}
float BQF::Update(float in)
{
	float filtered =
		(b0 / a0) * in +
		(b1 / a0) * zb[0] +
		(b2 / a0) * zb[1] -
		(a1 / a0) * za[0] -
		(a2 / a0) * za[1];

	// Shift delay buffers
	zb[1] = zb[0];
	zb[0] = in;

	// Feedback the calculated value into the IIR delay buffers
	za[1] = za[0];
	za[0] = filtered;

	return filtered;
}

void PanDSP::Process(float *out, uint32 numSamples)
{
	for (uint32 i = 0; i < numSamples; i++)
	{
		if (panning > 0)
			out[i * 2 + 0] = (out[i * 2 + 0] * (1.0f - panning)) * mix + out[i * 2 + 0] * (1 - mix);
		if (panning < 0)
			out[i * 2 + 1] = (out[i * 2 + 1] * (1.0f + panning)) * mix + out[i * 2 + 1] * (1 - mix);
	}
}

BQFDSP::BQFDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
}
void BQFDSP::Process(float *out, uint32 numSamples)
{
	for (uint32 c = 0; c < 2; c++)
	{
		for (uint32 i = 0; i < numSamples; i++)
		{
			out[i * 2 + c] = m_filters[c].Update(out[i * 2 + c]);
		}
	}
}
void BQFDSP::SetLowPass(float q, float freq)
{
	for (uint32 c = 0; c < 2; c++)
	{
		m_filters[c].SetLowPass(q, freq, (float)m_sampleRate);
	}
}
void BQFDSP::SetHighPass(float q, float freq)
{
	for (uint32 c = 0; c < 2; c++)
	{
		m_filters[c].SetHighPass(q, freq, (float)m_sampleRate);
	}
}
void BQFDSP::SetPeaking(float q, float freq, float gain)
{
	for (uint32 c = 0; c < 2; c++)
	{
		m_filters[c].SetPeaking(q, freq, gain, (float)m_sampleRate);
	}
}

CombinedFilterDSP::CombinedFilterDSP(uint32 sampleRate) : DSP(), a(sampleRate), peak(sampleRate)
{
	SetSampleRate(sampleRate);
}
void CombinedFilterDSP::SetLowPass(float q, float freq, float peakQ, float peakGain)
{
	a.SetLowPass(q, freq);
	peak.SetPeaking(peakQ, freq, peakGain);
}
void CombinedFilterDSP::SetHighPass(float q, float freq, float peakQ, float peakGain)
{
	a.SetHighPass(q, freq);
	peak.SetPeaking(peakQ, freq, peakGain);
}
void CombinedFilterDSP::Process(float *out, uint32 numSamples)
{
	a.mix = mix;
	peak.mix = mix;
	a.Process(out, numSamples);
	peak.Process(out, numSamples);
}

LimiterDSP::LimiterDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
}
void LimiterDSP::Process(float *out, uint32 numSamples)
{
	const float secondsPerSample = 1.0f / (float)m_sampleRate;
	for (uint32 i = 0; i < numSamples; i++)
	{
		float currentGain = 1.0f;
		if (m_currentReleaseTimer < releaseTime)
		{
			float t = (1.0f - m_currentReleaseTimer / releaseTime);
			currentGain = (1.0f / m_currentMaxVolume) * t + (1.0f - t);
		}

		float maxVolume = Math::Max(std::abs(out[i * 2]), std::abs(out[i * 2 + 1]));

		float currentMax = 1.0f / currentGain;
		if (maxVolume > currentMax)
		{
			m_currentMaxVolume = maxVolume;
			m_currentReleaseTimer = 0.0f;
            currentGain = 1.0f / maxVolume;
		}
		else
		{
			m_currentReleaseTimer += secondsPerSample;
		}
        // Apply the new peak reduction to this frame, not the next one.
        out[i * 2] *= currentGain * 0.9f;
        out[i * 2 + 1] *= currentGain * 0.9f;
	}
}

BitCrusherDSP::BitCrusherDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
}
void BitCrusherDSP::SetPeriod(float period /*= 0*/)
{
	// Scale period with sample rate
	assert(m_sampleRate > 0);
	double f = m_sampleRate / 44100.0;

	m_increment = (uint32)((double)(1 << 16));
	m_period = (uint32)(f * period * (double)(1 << 16));
}
void BitCrusherDSP::Process(float *out, uint32 numSamples)
{
    if (m_period <= m_increment) { m_currentDuration=0; return; }
	for (uint32 i = 0; i < numSamples; i++)
	{
		m_currentDuration += m_increment;
		if (m_currentDuration >= m_period)
		{
			m_sampleBuffer[0] = out[i * 2];
			m_sampleBuffer[1] = out[i * 2 + 1];
			m_currentDuration %= m_period;
		}

		out[i * 2] = m_sampleBuffer[0] * mix + out[i * 2] * (1.0f - mix);
		out[i * 2 + 1] = m_sampleBuffer[1] * mix + out[i * 2 + 1] * (1.0f - mix);
	}
}

GateDSP::GateDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
}
void GateDSP::SetLength(double length)
{
	double flength = length / 1000.0 * m_sampleRate;
	m_length = (uint32)flength;
	SetGating(m_gating);
}
void GateDSP::SetGating(float gating)
{
	gating = Math::Clamp(gating, 0.f, 1.f);
	float flength = (float)m_length;
	m_gating = gating;
	m_halfway = (uint32)(flength * gating);
    // Keep both ramps inside their respective on/off windows. Endpoints
    // (fully closed/open) have no ramps and must never divide by zero.
    m_fadeIn = std::min({uint32(flength * 0.05f), m_halfway / 2,
        (m_length - m_halfway) / 2});
    m_fadeOut = m_halfway - m_fadeIn;
	if (m_length) m_currentSample %= m_length;
}
void GateDSP::Process(float *out, uint32 numSamples)
{
	if (m_length == 0)
		return;

	const uint32 startSample = GetStartSample();
	const uint32 currentSample = GetCurrentSample();

    const bool chartClock = !m_measureTimes.empty() && m_audioBase;
    const double sourceRate = chartClock ? m_audioBase->GetSampleRate() : m_sampleRate;
    const double speed = chartClock ? std::max(.001f, m_audioBase->PlaybackSpeed) : 1.0;
    const double nowMs = currentSample * 1000.0 / sourceRate - m_clockOffset;
    auto measure = std::upper_bound(m_measureTimes.begin(), m_measureTimes.end(), nowMs);
	for (uint32 i = 0; i < numSamples; i++)
	{
		if (currentSample + i < startSample)
		{
			continue;
		}
        if (chartClock) {
            const double time = nowMs + i * speed * 1000.0 / m_sampleRate;
            while (measure != m_measureTimes.end() && *measure <= time) ++measure;
            const double origin = measure == m_measureTimes.begin() ? m_measureTimes.front() : *std::prev(measure);
            const double elapsed = std::max(0.0, (time-origin) * m_sampleRate / (1000.0*speed));
            m_currentSample = uint32(std::fmod(elapsed, double(m_length)));
        }
        float c = m_currentSample < m_halfway ? 1.0f : 0.0f;
        if (m_fadeIn != 0) {
            if (m_currentSample < m_halfway && m_currentSample > m_fadeOut)
                c = float(m_halfway - m_currentSample) / float(m_fadeIn);
            else if (m_currentSample >= m_length - m_fadeIn)
                c = float(m_currentSample - (m_length - m_fadeIn)) / float(m_fadeIn);
        }

		// Multiply volume
		c = (c * (1 - low) + low); // Range [low, 1]
		c = c * mix + (1.0f - mix);
		out[i * 2] *= c;
		out[i * 2 + 1] *= c;

		m_currentSample++;
		m_currentSample %= m_length;
	}
}

TapeStopDSP::TapeStopDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
}
void TapeStopDSP::SetLength(double length)
{
	assert(m_sampleRate > 0);
	std::lock_guard<std::mutex> guard(m_stateLock);
	const uint32 newLength = (uint32)(length / 1000.0 * m_sampleRate);
	// This is the legacy (linear) mode. If the DSP was in KSM-curve mode, an explicit length has to
	// switch it back, otherwise Process() keeps using the curve and silently ignores this value.
	const bool leavingKsmCurve = m_ksmCurve;
	if (!leavingKsmCurve && m_length == newLength) return;
	m_ksmCurve = false;
	m_length = newLength;
	m_Reset();
	m_sampleBuffer.reserve(2 * m_length + 100);
}
void TapeStopDSP::SetKsmSpeed(float speed)
{
	assert(m_sampleRate > 0);
	speed = Math::Clamp(speed, 0.0f, 1.0f);
	std::lock_guard<std::mutex> guard(m_stateLock);
	if (m_ksmCurve && m_ksmSpeed == speed) return;
	if (!m_ksmCurve) m_Reset();
	m_ksmCurve = true;
	m_ksmSpeed = speed;
	// The per-frame decrement is at least scale * 0.5 / 500000, which bounds how long the buffer can grow.
	const float scale = speed * 100.0f + 1.0f;
	const size_t maxFrames = (size_t)(1.0e6f / scale) + 100;
	m_sampleBuffer.reserve(2 * maxFrames);
}
void TapeStopDSP::m_Reset()
{
	m_sampleIdx = 0.0;
	m_currentSample = 0;
	m_playSpeed = 1.0f;
	m_ksmFrames = 0;
	m_sampleBuffer.clear();
}
void TapeStopDSP::Process(float *out, uint32 numSamples)
{
	// Audio thread. Setters on the game thread clear/reserve m_sampleBuffer, so hold the state lock.
	std::lock_guard<std::mutex> guard(m_stateLock);
	if (!m_ksmCurve && !m_length) return;
	if (!trigger)
	{
		// Untriggered: transparent, and the slowdown restarts from full speed on the next trigger.
		if (m_currentSample != 0 || m_sampleIdx != 0.0 || m_ksmFrames != 0)
			m_Reset();
		return;
	}
	const uint32 startSample = GetStartSample();
	const uint32 currentSample = GetCurrentSample();
	const float sampleRateScale = (float)m_sampleRate / 44100.0f;

	for (uint32 i = 0; i < numSamples; i++)
	{
		if (currentSample + i < startSample)
		{
			continue;
		}
		const float dryL = out[i * 2];
		const float dryR = out[i * 2 + 1];

		float playSpeed;
		if (m_ksmCurve)
		{
			// ksmaudio TapestopDSP: the speed falls by a per-frame amount that eases off over time
			const float scale = Math::Max(m_ksmSpeed * 100.0f, 0.0f) + 1.0f;
			const float rate = powf(2.5f, -(float)m_ksmFrames / 8000.0f / sampleRateScale) + 0.5f;
			m_playSpeed = Math::Max(m_playSpeed - scale * rate / 500000.0f, 0.0f);
			m_ksmFrames++;
			playSpeed = m_playSpeed;
		}
		else
		{
			playSpeed = m_currentSample < m_length ? 1.0f - (float)m_currentSample / (float)m_length : 0.0f;
		}

		float wetL = 0.0f;
		float wetR = 0.0f;
		if (playSpeed > 0.0f)
		{
			// Store samples for later
			m_sampleBuffer.Add(dryL);
			m_sampleBuffer.Add(dryR);

			// Read position into the buffer (never ahead of what has been written). The position is
			// fractional while the tape is slowing down, so interpolate between neighbouring frames
			// like ksmaudio (TimeModulator uses lerpedDelay) instead of repeating whole samples.
			const uint32 lastFrame = (uint32)(m_sampleBuffer.size() / 2 - 1);
			const double readPos = Math::Min(m_sampleIdx, (double)lastFrame);
			const uint32 i0 = (uint32)floor(readPos);
			const uint32 i1 = Math::Min(i0 + 1, lastFrame);
			const float frac = (float)(readPos - (double)i0);
			// Like KSM, fade only the wet signal out over the last quarter of the slowdown.
			const float fade = playSpeed < 0.25f ? playSpeed * 4.0f : 1.0f;
			wetL = (m_sampleBuffer[i0 * 2] + (m_sampleBuffer[i1 * 2] - m_sampleBuffer[i0 * 2]) * frac) * fade;
			wetR = (m_sampleBuffer[i0 * 2 + 1] + (m_sampleBuffer[i1 * 2 + 1] - m_sampleBuffer[i0 * 2 + 1]) * frac) * fade;

			// Increase index
			m_sampleIdx += playSpeed;
			m_currentSample++;
		}
		// Once the tape has stopped only the wet part is silent; the dry part stays at (1 - mix).
		out[i * 2] = wetL * mix + dryL * (1.0f - mix);
		out[i * 2 + 1] = wetR * mix + dryR * (1.0f - mix);
	}
}

RetriggerDSP::RetriggerDSP(uint32 sampleRate) : DSP()
{
    SetSampleRate(sampleRate);
    // Bounded storage allocated outside the audio callback, matching a ten-second capture window.
    m_sampleBuffer.resize(size_t(sampleRate) * 10 * 2);
}
void RetriggerDSP::SetLength(double length)
{
    m_length = uint32(Math::Clamp(length * m_sampleRate / 1000.0, 0.0, double(m_sampleBuffer.size()/2)));
    SetGating(m_gating);
}
void RetriggerDSP::SetResetDuration(uint32 duration)
{
    m_resetDuration = uint32(double(duration) * m_sampleRate / 1000.0);
}
void RetriggerDSP::SetGating(float gating)
{
    m_gating = Math::Clamp(gating, 0.f, 1.f);
    m_gateLength = uint32(m_length * m_gating);
}
void RetriggerDSP::SetMaxLength(uint32) {}
void RetriggerDSP::Process(float* out, uint32 numSamples)
{
    if (!m_audioBase || !numSamples) return;
    const double sourceRate = m_audioBase->GetSampleRate();
    const double step = sourceRate / m_sampleRate * std::max(.001f, m_audioBase->PlaybackSpeed);
    double now = m_audioBase->GetSamplePos();
    const double trigger = double(startTime) * sourceRate / 1000.0;
    const double origin = double(lastTimingPoint) * sourceRate / 1000.0;
    const double reset = double(m_resetDuration) * step;
    // A discontinuous transport cannot reuse capture from before a seek/restart.
    if (m_expectedSource < 0 || std::abs(now-m_expectedSource) > std::max(2.0, step*2)) {
        m_captureFrames=0; m_hasBucket=false;
    } else {
        // Source positions are integer frames; preserve fractional resampling phase.
        now=m_expectedSource;
    }
    for (uint32 i=0;i<numSamples;++i) {
        const double position=now+i*step;
        if(position<trigger) continue;
        const double bucket=reset>0?std::floor((position-origin)/reset):0;
        if(m_seenTrigger!=startTime || (reset>0 && m_hasBucket && bucket!=m_resetBucket)) {
            m_captureFrames=0;
            m_seenTrigger=startTime;
        }
        m_resetBucket=bucket; m_hasBucket=true;
        // Capture the incoming chain signal even while the effect is mixed out.
        if(m_captureFrames<m_sampleBuffer.size()/2) {
            m_sampleBuffer[size_t(m_captureFrames)*2]=out[i*2];
            m_sampleBuffer[size_t(m_captureFrames)*2+1]=out[i*2+1];
        }
        if(m_length) {
            const size_t phase=size_t(m_captureFrames % m_length);
            for(int ch=0;ch<2;++ch) {
                const float wet=phase<m_gateLength?m_sampleBuffer[phase*2+ch]:0.f;
                out[i*2+ch] += (wet-out[i*2+ch])*mix;
            }
        }
        ++m_captureFrames;
    }
    m_expectedSource=now+numSamples*step;
}

WobbleDSP::WobbleDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
}
void WobbleDSP::SetLength(double length)
{
	double flength = length / 1000.0 * m_sampleRate;
	m_length = (uint32)flength;
}
void WobbleDSP::Process(float *out, uint32 numSamples)
{
	if (m_length == 0)
		return;

	static Interpolation::CubicBezier easing(Interpolation::EaseInExpo);
	const uint32 startSample = GetStartSample();
	const uint32 currentSample = GetCurrentSample();

	for (uint32 i = 0; i < numSamples; i++)
	{
		if (currentSample + i < startSample)
		{
			continue;
		}
		float f = std::abs(2.0f * ((float)m_currentSample / (float)m_length) - 1.0f);
		f = easing.Sample(f);
		float freq = fmin + (fmax - fmin) * f;

		for (uint32 c = 0; c < 2; c++) {
			m_filters[c].SetLowPass(q, freq, (float)m_sampleRate);
			out[i * 2 + c] = m_filters[c].Update(out[i * 2 + c]) * mix + out[i * 2 + c] * (1.0f - mix);
		}

		m_currentSample++;
		m_currentSample %= m_length;
	}
}

PhaserDSP::PhaserDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
}
void PhaserDSP::SetLength(double length)
{
	double flength = length / 1000.0 * m_sampleRate;
	m_length = (uint32)flength;
}
void PhaserDSP::SetStage(uint32 stage)
{
	stage = Math::Clamp(stage, 0u, 12u);
	m_stage = (stage / 2) * 2;
}
void PhaserDSP::Process(float *out, uint32 numSamples)
{
    if (m_length == 0) return;
	if (m_stage == 0 || mix == 0) {
        if (numSamples) { za[0]=out[(numSamples-1)*2]; za[1]=out[(numSamples-1)*2+1]; }
        if (m_length) m_currentSample = (m_currentSample + numSamples) % m_length;
        return;
    }

	const uint32 startSample = GetStartSample();
	const uint32 currentSample = GetCurrentSample();

	// logarithmic center
	float freqCenter = sqrt(fmin*fmax);
	for (uint32 c = 0; c < 2; c++) {
		m_hiShelf[c].SetHighShelf(1.5f, freqCenter, hiCutGain, (float)m_sampleRate);
	}

	for (uint32 i = 0; i < numSamples; i++)
	{
		if (currentSample + i < startSample)
		{
			continue;
		}

		float fLeft = std::abs(2.0f * (((float)m_currentSample / (float)m_length)) - 1.0f);
		float fRight = std::abs(2.0f * fmodf(((float)m_currentSample / (float)m_length) + stereoWidth, 1.0f) - 1.0f);
		float f[2] = {fLeft, fRight};
		for (uint32 c = 0; c < 2; c++) {
			// logarithmic interpolation
			float freq = pow(fmin, f[c]) * pow(fmax, 1-f[c]);
			float output = feedback * za[c] + out[i * 2 + c];
			for (uint32 j = 0; j < m_stage; j++) {
				m_apf[j][c].SetAllPass(q, freq, (float)m_sampleRate);
				output = m_apf[j][c].Update(output);
			}
			za[c] = output;
            // KSM applies the high-cut to wet audio only, then mixes at mix/2.
            const float wet = m_hiShelf[c].Update(output);
            out[i * 2 + c] += (wet - out[i * 2 + c]) * (mix * .5f);
		}
		if (m_length) { m_currentSample++; m_currentSample %= m_length; }
	}
}

FlangerDSP::FlangerDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
    for (auto& shelf : m_feedbackShelf) shelf.SetLowShelf(.5f, 250.f, -20.f, float(sampleRate));
}
void FlangerDSP::SetLength(double length)
{
	double flength = length / 1000.0 * m_sampleRate;
	m_length = (uint32)flength;
}
void FlangerDSP::SetDelayRange(uint32 offset, uint32 depth)
{
	// Assuming 44100hz is the base sample rate
	const uint32 max = offset + depth;
	const uint32 min = offset;

	const float mult = (float)m_sampleRate / 44100.f;
	m_min = static_cast<uint32>(min * mult);
	m_max = static_cast<uint32>(max * mult);
	m_bufferLength = (m_max + 1) * 2;
	m_sampleBuffer.resize(m_bufferLength);
    m_bufferOffset %= m_bufferLength;
}
void FlangerDSP::SetFeedback(float feedback)
{
	m_feedback = feedback;
}
void FlangerDSP::SetStereoWidth(float stereoWidth)
{
	m_stereoWidth = stereoWidth;
}
void FlangerDSP::SetVolume(float volume)
{
	m_volume = volume;
}
void FlangerDSP::Process(float *out, uint32 numSamples)
{
	if (m_bufferLength <= 0 || m_length == 0)
		return;
	float *data = m_sampleBuffer.data();
	if (data == nullptr)
		return;

	const uint32 startSample = GetStartSample();
	const uint32 currentSample = GetCurrentSample();
	const uint32 depth = m_max - m_min;

	for (uint32 i = 0; i < numSamples; i++)
	{
		if (currentSample + i < startSample)
		{
			continue;
		}
		// Determine where we want to sample past samples
		float f = fmodf(((float)m_time / ((float)m_length * 2)), 1.f);
		f = fabsf(f * 2 - 1);
		uint32 dLeft = (uint32)(m_max - (depth * f));
		uint32 dRight = dLeft + (uint32)(m_stereoWidth * depth);
		// "fold" dRight to fit [m_min,m_max] range: matches KSM behavior
		if (dRight > m_max) dRight = m_max - (dRight - m_max);

		// TODO: clean up?
		int32 samplePosLeft = ((int)m_bufferOffset - (int)dLeft * 2) % (int)m_bufferLength;
		if (samplePosLeft < 0) samplePosLeft = m_bufferLength + samplePosLeft;
		int32 samplePosRight = ((int)m_bufferOffset - (int)dRight * 2 + 1) % (int)m_bufferLength;
		if (samplePosRight < 0) samplePosRight = m_bufferLength + samplePosRight;

        // Read before writing: a zero delay can address the write slot.
        const float delayed[2] = { data[samplePosLeft], data[samplePosRight] };
        const float gain = 1.f + mix * (m_volume - 1.f);
        for (uint32 channel = 0; channel < 2; ++channel) {
            const float dry = out[i * 2 + channel];
            // KSM's feedback path removes low-frequency buildup. Do not
            // hard-clip the effect here; output limiting belongs to the mixer.
            data[m_bufferOffset + channel] = m_feedbackShelf[channel].Update(
                (dry + m_feedback * delayed[channel]) * gain);
            out[i * 2 + channel] = (dry + mix * delayed[channel]) * gain;
        }

		m_bufferOffset += 2;
		if (m_bufferOffset >= m_bufferLength)
			m_bufferOffset = 0;
		if (m_length) m_time++;
	}
}

EchoDSP::EchoDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
}
void EchoDSP::SetLength(double length)
{
	double flength = length / 1000.0 * m_sampleRate;
	const uint32 newLength = uint32(std::max(0.0, flength)) * 2;
    if (newLength == m_bufferLength) return;
    m_bufferOffset = 0;
    m_sampleBuffer.clear();
	m_bufferLength = newLength;
	m_sampleBuffer.resize(m_bufferLength);
	memset(m_sampleBuffer.data(), 0, sizeof(float) * m_bufferLength);
	m_numLoops = 0;
}
void EchoDSP::Process(float *out, uint32 numSamples)
{
	float *data = m_sampleBuffer.data();
	if (!data || m_bufferLength < 2)
		return;

	const uint32 startSample = GetStartSample();
	const uint32 currentSample = GetCurrentSample();

	for (uint32 i = 0; i < numSamples; i++)
	{
		if (currentSample + i < startSample)
		{
			continue;
		}

		float l0 = data[m_bufferOffset + 0];
		float l1 = data[m_bufferOffset + 1];

		if (m_numLoops > 0)
		{
			// Send echo to output
			out[i * 2] = l0 * mix;
			out[i * 2 + 1] = l1 * mix;
		}

		// Inject new sample
		data[m_bufferOffset + 0] = out[i * 2] * feedback;
		data[m_bufferOffset + 1] = out[i * 2 + 1] * feedback;

		m_bufferOffset += 2;
		if (m_bufferOffset >= m_bufferLength)
		{
			m_bufferOffset = 0;
			m_numLoops++;
		}
	}
}

SidechainDSP::SidechainDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
}
void SidechainDSP::SetLength(double length)
{
	double flength = length / 1000.0 * m_sampleRate;
	m_length = (uint32)flength;
    m_time = m_length ? m_time % m_length : 0;
}
void SidechainDSP::SetAttackTime(double length)
{
	double flength = length / 1000.0 * m_sampleRate;
	m_attackTime = (uint32)flength;
}
void SidechainDSP::SetHoldTime(double length)
{
	double flength = length / 1000.0 * m_sampleRate;
	m_holdTime = (uint32)flength;
}
void SidechainDSP::SetReleaseTime(double length)
{
	double flength = length / 1000.0 * m_sampleRate;
	m_releaseTime = (uint32)flength;
}
void SidechainDSP::Process(float *out, uint32 numSamples)
{
	if (m_length == 0)
		return;

	const uint32 startSample = GetStartSample();
	const uint32 currentSample = GetCurrentSample();
	for (uint32 i = 0; i < numSamples; i++)
	{
		if (currentSample + i < startSample)
		{
			continue;
		}
		float volume;
		if (m_time < m_attackTime)
		{
			volume = 1.0f - ((float)m_time / (float)m_attackTime);
		}
		else if (m_time < m_attackTime + m_holdTime)
		{
			volume = 0.0f;
		}
		else if (m_time < m_attackTime + m_holdTime + m_releaseTime)
		{
			volume = ((float)(m_time - m_attackTime - m_holdTime) / (float)m_releaseTime);
		}
		else
		{
			volume = 1.0f;
		}
		// range from 1/ratio (volume=0) to 1 (volume=1)
		float sampleGain = mix * ((1.0f/std::max(ratio, 1.0f)) + (1.0f - 1.0f/ratio)*volume) + (1.0f - mix);
		out[i * 2 + 0] *= sampleGain;
		out[i * 2 + 1] *= sampleGain;
		if (++m_time > m_length)
		{
			m_time = 0;
		}
	}
}

#include "SoundTouch.h"
using namespace soundtouch;

class PitchShiftDSP_Impl
{
public:
	float pitch = 0.0f;
	bool init = false;

private:
	SoundTouch m_soundtouch;
	Vector<float> m_receiveBuffer;

public:
	PitchShiftDSP_Impl() = default;
	~PitchShiftDSP_Impl() = default;
	void Init(uint32 sampleRate)
	{
		m_soundtouch.setChannels(2);
		m_soundtouch.setSampleRate(sampleRate);
        init = true;
		m_soundtouch.setSetting(SETTING_USE_AA_FILTER, 0);
		m_soundtouch.setSetting(SETTING_SEQUENCE_MS, 5);
		//m_soundtouch.setSetting(SETTING_SEEKWINDOW_MS, 10);
		//m_soundtouch.setSetting(SETTING_OVERLAP_MS, 10);
	}
	void Process(float *out, uint32 numSamples, float mix)
	{
		m_receiveBuffer.resize(numSamples * 2);
		m_soundtouch.setPitchSemiTones(pitch);
		m_soundtouch.putSamples(out, numSamples);
		uint32 receivedSamples = m_soundtouch.receiveSamples(m_receiveBuffer.data(), numSamples);
		if (receivedSamples > 0)
		{
			for (uint32 i=0; i<receivedSamples*2; ++i) out[i] += (m_receiveBuffer[i]-out[i])*mix;
		}
	}
};

PitchShiftDSP::PitchShiftDSP(uint32 sampleRate) : DSP()
{
	SetSampleRate(sampleRate);
	m_impl = new PitchShiftDSP_Impl();
}
PitchShiftDSP::~PitchShiftDSP()
{
	delete m_impl;
}
void PitchShiftDSP::Process(float *out, uint32 numSamples)
{
    if (chunkSize > 0) {
        const uint32 size = std::max(2u, uint32(std::clamp(chunkSize, 2, 44100) * (double(m_sampleRate)/44100.0)));
        const uint32 capacity = std::max(8u, size*4);
        if (m_grainBuffer.size() != capacity*2) {
            m_grainBuffer.assign(capacity*2,0.f); m_grainCursor=0; m_grainPhase=0;
        }
        const double speed = std::pow(2.0, std::clamp(double(amount),-48.0,48.0)/12.0);
        const double fade = std::clamp(double(overlap),0.001,0.5);
        auto read = [&](double delay,int ch) {
            double index=std::fmod(double(m_grainCursor)+capacity-delay, double(capacity));
            if(index<0) index+=capacity;
            const uint32 lo=uint32(index); const float t=float(index-lo);
            return m_grainBuffer[lo*2+ch]*(1-t)+m_grainBuffer[((lo+1)%capacity)*2+ch]*t;
        };
        for(uint32 i=0;i<numSamples;++i) {
            for(int ch=0;ch<2;++ch) m_grainBuffer[m_grainCursor*2+ch]=out[i*2+ch];
            const double p=m_grainPhase, p2=std::fmod(p+0.5,1.0);
            auto window=[&](double phase){return std::min(1.0,std::min(phase,1-phase)/fade);};
            const double w1=window(p),w2=window(p2),sum=std::max(1e-9,w1+w2);
            if(amount!=0 && mix!=0) for(int ch=0;ch<2;++ch) {
                const float wet=float((read(1+p*size,ch)*w1+read(1+p2*size,ch)*w2)/sum);
                out[i*2+ch]+=(wet-out[i*2+ch])*mix;
            }
            m_grainPhase=std::fmod(m_grainPhase+(1-speed)/size+1.0,1.0);
            m_grainCursor=(m_grainCursor+1)%capacity;
        }
        return;
    }
	m_impl->pitch = Math::Clamp(amount, -48.0f, 48.0f);
	if (!m_impl->init)
		m_impl->Init(m_sampleRate);
	m_impl->Process(out, numSamples, mix);
}

#include "ksmaudio/AudioEffect/DSP/PitchShiftDSP.hpp"
#include <cmath>
#include <algorithm>
#include <cassert>

namespace
{
	// ローパスフィルターのカットオフ周波数係数
	// ナイキスト周波数ちょうどになるとクリッピング発生するため、余裕を持たせる
	constexpr float kLowpassCutoffCoeff = 0.95f;
}

namespace ksmaudio::AudioEffect
{
	PitchShiftDSP::PitchShiftDSP(const DSPCommonInfo& info)
		: m_info(info)
		, m_delayBuffer(info.numChannels)
		, m_lowpassFilter(info.numChannels)
	{
		m_start = kDelayBufferMax - m_chunkSize;
		m_prevStart = m_prevPrevStart = kDelayBufferMax - m_chunkSize - static_cast<std::size_t>(m_overlap * m_chunkSize);
	}

	void PitchShiftDSP::process(float* pData, std::size_t dataSize, bool bypass, const PitchShiftDSPParams& params)
	{
		if (m_info.isUnsupported || bypass)
		{
			return;
		}

		assert(dataSize % m_info.numChannels == 0);
		const std::size_t numFrames = dataSize / m_info.numChannels;

		m_pitch = params.pitch;
		const float mix = params.mix;
		m_playSpeed = std::pow(2.0f, m_pitch / 12.0f);
		const std::size_t newChunkSize = static_cast<std::size_t>(params.chunkSize * m_info.sampleRateScale);
		const float newOverlap = std::clamp(params.overlap, 0.0f, 0.5f);

		// チャンクサイズやオーバーラップが変更された場合、バッファ位置を再初期化
		if (newChunkSize != m_chunkSize || newOverlap != m_overlap)
		{
			m_chunkSize = newChunkSize;
			m_overlap = newOverlap;
			m_count = 0;
			m_start = kDelayBufferMax - m_chunkSize;
			m_prevStart = m_prevPrevStart = kDelayBufferMax - m_chunkSize - static_cast<std::size_t>(m_overlap * m_chunkSize);
			m_thirdChunkBlendStep = std::nullopt;
		}

		if (m_chunkSize < 2)
		{
			// チャンクが小さすぎる場合は処理できないためbypass扱い
			return;
		}

		if (m_pitch != m_pitchPrev)
		{
			if (m_playSpeed > 1.0f)
			{
				const float cutoffFreq = m_info.sampleRateFloat / 2 / m_playSpeed * kLowpassCutoffCoeff;
				for (std::size_t ch = 0; ch < m_lowpassFilter.size(); ++ch)
				{
					for (std::size_t i = 0; i < kNumLowpassFilters; ++i)
					{
						m_lowpassFilter[ch][i].setLowPassFilter(cutoffFreq, 0.707f, m_info.sampleRateFloat);
					}
				}
			}
			else
			{
				const float cutoffFreq = m_info.sampleRateFloat * 0.5f * kLowpassCutoffCoeff;
				for (std::size_t ch = 0; ch < m_lowpassFilter.size(); ++ch)
				{
					for (std::size_t i = 0; i < kNumLowpassFilters; ++i)
					{
						m_lowpassFilter[ch][i].setLowPassFilter(cutoffFreq, 0.707f, m_info.sampleRateFloat);
					}
				}
			}
		}

		const std::size_t overlapSample = static_cast<std::size_t>(m_overlap * m_chunkSize);
		if (m_chunkSize <= overlapSample)
		{
			return;
		}

		assert(m_delayBuffer.size() == m_info.numChannels);
		assert(m_lowpassFilter.size() == m_info.numChannels);
		for (std::size_t i = 0; i < numFrames; ++i)
		{
			for (std::size_t ch = 0; ch < m_info.numChannels; ++ch)
			{
				// ピッチを上げる場合、6次の急峻なLPF適用後の入力を使うことで折り返しノイズの発生を抑える
				float inputSample = *pData;
				if (m_playSpeed > 1.0f)
				{
					for (std::size_t i = 0; i < kNumLowpassFilters; ++i)
					{
						inputSample = m_lowpassFilter[ch][i].process(inputSample);
					}
				}
				m_delayBuffer[ch][m_cursor] = inputSample;

				const std::size_t countTimesPlaySpeed = static_cast<std::size_t>(m_count * m_playSpeed);
				const std::size_t step = countTimesPlaySpeed % (m_chunkSize - overlapSample);

				if (m_pitch == 0.0f || mix == 0.0f)
				{
					++pData;
					continue;
				}

				float output;
				if (overlapSample == 0)
				{
					// オーバーラップなしの場合はクロスフェードせず現在のチャンクをそのまま出力
					const std::size_t currentIdx = (kDelayBufferMax + m_start - m_chunkSize + step) % kDelayBufferMax;
					output = m_delayBuffer[ch][currentIdx];
				}
				else if (countTimesPlaySpeed <= overlapSample)
				{
					const float rate = static_cast<float>(step) / overlapSample;
					const std::size_t currentIdx = (kDelayBufferMax + m_start - m_chunkSize + step) % kDelayBufferMax;
					const std::size_t prevIdx = (m_prevStart + step) % kDelayBufferMax;
					if (!m_thirdChunkBlendStep.has_value())
					{
						output = m_delayBuffer[ch][currentIdx] * rate + m_delayBuffer[ch][prevIdx] * (1.0f - rate);
					}
					else
					{
						const std::size_t prevPrevIdx = (m_prevPrevStart + step) % kDelayBufferMax;
						const float rate2 = (std::min)(static_cast<float>(m_thirdChunkBlendStep.value()) / overlapSample, 1.0f);
						output = m_delayBuffer[ch][currentIdx] * rate + (m_delayBuffer[ch][prevIdx] * rate2 + m_delayBuffer[ch][prevPrevIdx] * (1.0f - rate2)) * (1.0f - rate);
						if (ch == m_info.numChannels - 1)
						{
							++m_thirdChunkBlendStep.value();
						}
					}
				}
				else if (step <= overlapSample)
				{
					const float rate = static_cast<float>(step) / overlapSample;
					const std::size_t currentIdx = (kDelayBufferMax + m_start - m_chunkSize + step) % kDelayBufferMax;
					const std::size_t overlapIdx = (kDelayBufferMax + m_start - overlapSample + step) % kDelayBufferMax;
					output = m_delayBuffer[ch][currentIdx] * rate + m_delayBuffer[ch][overlapIdx] * (1.0f - rate);
				}
				else
				{
					const std::size_t currentIdx = (kDelayBufferMax + m_start - m_chunkSize + step) % kDelayBufferMax;
					output = m_delayBuffer[ch][currentIdx];
				}

				output = output * mix + *pData * (1.0f - mix);
				*pData = output;
				++pData;
			}

			++m_count;
			if (m_count > m_chunkSize - overlapSample)
			{
				const std::size_t countTimesPlaySpeed = static_cast<std::size_t>((m_count - 1) * m_playSpeed);
				const std::size_t step = countTimesPlaySpeed % (m_chunkSize - overlapSample);
				if (step <= overlapSample)
				{
					m_prevPrevStart = (kDelayBufferMax + m_start - overlapSample + step) % kDelayBufferMax;
					m_prevStart = (kDelayBufferMax + m_start - m_chunkSize + step) % kDelayBufferMax;
					m_thirdChunkBlendStep = step + 1;
				}
				else
				{
					m_prevPrevStart = m_prevStart = (kDelayBufferMax + m_start - m_chunkSize + step) % kDelayBufferMax;
					m_thirdChunkBlendStep = std::nullopt;
				}
				m_count = 0;
				m_start = m_cursor;
			}
			++m_cursor;
			if (m_cursor >= kDelayBufferMax)
			{
				m_cursor = 0;
			}
		}
		m_pitchPrev = m_pitch;
	}

	void PitchShiftDSP::updateParams(const PitchShiftDSPParams& params)
	{
	}
}

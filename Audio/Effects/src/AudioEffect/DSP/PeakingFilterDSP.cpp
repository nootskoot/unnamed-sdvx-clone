#include "ksmaudio/AudioEffect/DSP/PeakingFilterDSP.hpp"
#include "ksmaudio/AudioEffect/detail/FreqTableLookup.hpp"
#include <utility>

namespace ksmaudio::AudioEffect
{
	namespace
	{
		// 1フレーム(1/44100秒)あたりのvの線形イージングの速さ(最大変化量)
		constexpr float kVEasingSpeed = 0.01f;

		// フィルタ適用可能な最低周波数
		// (低周波を強調すると波形の振幅が過剰に大きくなるためしきい値を設けている)
		constexpr float kFreqThresholdMin = 100.0f;

		// 余韻の長さ(秒)
		constexpr float kFilterReleaseSec = 0.05f;

		// HSP版の計算式から事前計算した周波数テーブル
		// HSP版: https://github.com/kshootmania/ksm-v1/blob/08275836547c7792a6d4f59037e56e947f2979c3/src/scene/play/play_audio_effects.hsp#L974
		constexpr float kFreqTable[detail::kFreqTableSize] = {
			50.0000f, 50.1190f, 50.4762f, 51.0714f, 51.9046f, 52.9758f, 54.2850f, 55.8320f,
			57.6169f, 59.6395f, 61.8997f, 64.3974f, 67.1325f, 70.1048f, 73.3143f, 76.7608f,
			80.4440f, 84.3638f, 88.5201f, 92.9126f, 97.5411f, 102.4055f, 107.5053f, 112.8405f,
			118.4107f, 124.2157f, 130.2552f, 136.5289f, 143.0366f, 149.7778f, 156.7523f, 163.9597f,
			171.3997f, 179.0719f, 186.9760f, 195.1115f, 203.4782f, 212.0755f, 220.9030f, 229.9604f,
			239.2472f, 248.7630f, 258.5072f, 268.4795f, 278.6793f, 289.1061f, 299.7595f, 310.6389f,
			321.7439f, 333.0738f, 344.6281f, 356.4063f, 368.4078f, 380.6321f, 393.0785f, 405.7464f,
			418.6352f, 431.7444f, 445.0732f, 458.6210f, 472.3873f, 486.3713f, 500.5723f, 514.9897f,
			529.6227f, 544.4708f, 559.5331f, 574.8089f, 590.2975f, 605.9982f, 621.9102f, 638.0328f,
			654.3651f, 670.9064f, 687.6559f, 704.6127f, 721.7762f, 739.1454f, 756.7195f, 774.4977f,
			792.4791f, 810.6629f, 829.0482f, 847.6341f, 866.4197f, 885.4042f, 904.5866f, 923.9660f,
			943.5414f, 963.3120f, 983.2768f, 1003.4349f, 1023.7852f, 1044.3269f, 1065.0589f, 1085.9802f,
			1107.0899f, 1128.3869f, 1149.8702f, 1171.5389f, 1193.3918f, 1215.4279f, 1237.6462f, 1260.0457f,
			1282.6252f, 1305.3837f, 1328.3201f, 1351.4333f, 1374.7223f, 1398.1858f, 1421.8229f, 1445.6323f,
			1469.6130f, 1493.7638f, 1518.0835f, 1542.5711f, 1567.2254f, 1592.0451f, 1617.0291f, 1642.1763f,
			1667.4855f, 1692.9554f, 1718.5848f, 1744.3726f, 1770.3175f, 1796.4183f, 1822.6737f, 1849.0826f,
			1875.6437f, 1902.3557f, 1929.2174f, 1956.2275f, 1983.3848f, 2010.6879f, 2038.1357f, 2065.7267f,
			2093.4597f, 2121.3334f, 2149.3465f, 2177.4977f, 2205.7857f, 2234.2091f, 2262.7667f, 2291.4570f,
			2320.2787f, 2349.2305f, 2378.3111f, 2407.5190f, 2436.8530f, 2466.3116f, 2495.8935f, 2525.5972f,
			2555.4216f, 2585.3650f, 2615.4262f, 2645.6037f, 2675.8962f, 2706.3022f, 2736.8203f, 2767.4491f,
			2798.1873f, 2829.0333f, 2859.9857f, 2891.0432f, 2922.2042f, 2953.4674f, 2984.8313f, 3016.2945f,
			3047.8554f, 3079.5127f, 3111.2649f, 3143.1105f, 3175.0481f, 3207.0762f, 3239.1933f, 3271.3980f,
			3303.6888f, 3336.0642f, 3368.5227f, 3401.0629f, 3433.6832f, 3466.3821f, 3499.1583f, 3532.0101f,
			3564.9362f, 3597.9349f, 3631.0048f, 3664.1443f, 3697.3521f, 3730.6265f, 3763.9661f, 3797.3694f,
			3830.8348f, 3864.3608f, 3897.9460f, 3931.5887f, 3965.2875f, 3999.0409f, 4032.8474f, 4066.7054f,
			4100.6133f, 4134.5698f, 4168.5732f, 4202.6221f, 4238.2521f, 4331.2183f, 4424.2254f, 4517.2718f,
			4610.3561f, 4703.4766f, 4796.6319f, 4889.8204f, 4983.0406f, 5076.2910f, 5169.5701f, 5262.8763f,
			5356.2081f, 5449.5640f, 5542.9424f, 5636.3419f, 5729.7609f, 5823.1979f, 5916.6514f, 6010.1198f,
			6103.6017f, 6197.0955f, 6290.5997f, 6384.1128f, 6477.6333f, 6571.1596f, 6664.6903f, 6758.2238f,
			6851.7587f, 6945.2934f, 7038.8264f, 7132.3563f, 7225.8816f, 7319.4006f, 7412.9120f, 7506.4143f,
			7599.9059f, 7693.3854f, 7786.8513f, 7880.3021f, 7973.7364f, 8067.1526f, 8160.5493f, 8253.9250f,
			8347.2783f, 8440.6076f, 8533.9116f, 8627.1888f, 8720.4376f, 8813.6567f, 8906.8447f, 9000.0000f,
		};

		// HSP版の計算式から事前計算したgainテーブル(dB単位)
		// HSP版: https://github.com/kshootmania/ksm-v1/blob/08275836547c7792a6d4f59037e56e947f2979c3/src/scene/play/play_audio_effects.hsp#L974
		constexpr float kGainTable[detail::kFreqTableSize] = {
			0.0000f, 0.7994f, 1.5987f, 2.3981f, 3.1974f, 3.9968f, 4.7961f, 5.5955f,
			6.3948f, 7.1942f, 7.9936f, 8.7929f, 9.5923f, 10.3916f, 11.1910f, 11.9903f,
			12.7897f, 13.5890f, 14.3884f, 15.1878f, 15.9871f, 16.7865f, 17.5858f, 18.3852f,
			19.1845f, 19.9839f, 20.7832f, 21.5826f, 22.3820f, 23.1813f, 23.9807f, 24.7800f,
			25.5794f, 26.3787f, 27.1781f, 27.9774f, 28.7768f, 29.5762f, 30.3755f, 30.6222f,
			30.6894f, 30.7567f, 30.8239f, 30.8911f, 30.9584f, 31.0256f, 31.0928f, 31.1601f,
			31.2273f, 31.2945f, 31.3618f, 31.4290f, 31.4962f, 31.5635f, 31.6307f, 31.6980f,
			31.7652f, 31.8324f, 31.8997f, 31.9669f, 32.0341f, 32.1014f, 32.1686f, 32.2358f,
			32.3031f, 32.3703f, 32.4375f, 32.5048f, 32.5720f, 32.6392f, 32.7065f, 32.7737f,
			32.8410f, 32.9082f, 32.9754f, 33.0427f, 33.1099f, 33.1771f, 33.2444f, 33.3116f,
			33.3788f, 33.4461f, 33.5133f, 33.5805f, 33.6478f, 33.7150f, 33.7823f, 33.8495f,
			33.9167f, 33.9840f, 33.9488f, 33.8816f, 33.8143f, 33.7471f, 33.6799f, 33.6126f,
			33.5454f, 33.4782f, 33.4109f, 33.3437f, 33.2764f, 33.2092f, 33.1420f, 33.0747f,
			33.0075f, 32.9403f, 32.8730f, 32.8058f, 32.7386f, 32.6713f, 32.6041f, 32.5369f,
			32.4696f, 32.4024f, 32.3352f, 32.2679f, 32.2007f, 32.1334f, 32.0662f, 31.9990f,
			31.9317f, 31.8645f, 31.7973f, 31.7300f, 31.6628f, 31.5956f, 31.5283f, 31.4611f,
			31.3939f, 31.3266f, 31.2594f, 31.1921f, 31.1249f, 31.0577f, 30.9904f, 30.9232f,
			30.8560f, 30.7887f, 30.7215f, 30.6543f, 30.5870f, 30.5198f, 30.4526f, 30.3853f,
			30.3181f, 30.2509f, 30.1836f, 30.1164f, 30.0491f, 29.9819f, 29.9147f, 29.8474f,
			29.7802f, 29.7130f, 29.6457f, 29.5785f, 29.5113f, 29.4440f, 29.3768f, 29.3096f,
			29.2423f, 29.1751f, 29.1078f, 29.0406f, 28.9734f, 28.9061f, 28.8389f, 28.7717f,
			28.7044f, 28.6372f, 28.5700f, 28.5027f, 28.4355f, 28.3683f, 28.3010f, 28.2338f,
			28.1666f, 28.0993f, 28.0321f, 27.9648f, 27.8976f, 27.8304f, 27.7631f, 27.6959f,
			27.6287f, 27.5614f, 27.4942f, 27.4270f, 27.3597f, 27.2925f, 27.2253f, 27.1580f,
			27.0908f, 27.0235f, 26.9563f, 26.8891f, 26.8218f, 26.7546f, 26.6874f, 26.6201f,
			26.5529f, 26.4857f, 26.4184f, 26.3512f, 26.2804f, 26.0759f, 25.8714f, 25.6668f,
			25.4623f, 25.2578f, 25.0533f, 24.8488f, 24.6443f, 24.4398f, 24.2353f, 24.0308f,
			23.8263f, 23.6218f, 23.4173f, 23.2128f, 23.0082f, 22.8037f, 22.5992f, 22.3947f,
			22.1902f, 21.9857f, 21.7812f, 21.5767f, 21.3722f, 21.1677f, 20.9632f, 20.7587f,
			20.5541f, 20.3496f, 20.1451f, 19.9406f, 19.7361f, 19.5316f, 19.3271f, 19.1226f,
			18.9181f, 18.7136f, 18.5091f, 18.3046f, 18.1001f, 17.8955f, 17.6910f, 17.4865f,
			17.2820f, 17.0775f, 16.8730f, 16.6685f, 16.4640f, 16.2595f, 16.0550f, 15.8505f,
		};

	}

	namespace detail
	{
		PeakingFilterValueController::PeakingFilterValueController()
			: m_vEasing(kVEasingSpeed)
		{
		}

		void PeakingFilterValueController::updateGain(float v, float vAtFreq, float vAtFreqMax)
		{
			if (v != m_prevVInGainUpdate)
			{
				// gainの値はイージング適用前のものをもとに計算する
				// (直角時に一瞬低域が強調されてノイズが入るのを回避するため)
				const float scaledV = std::lerp(vAtFreq, vAtFreqMax, v);
				m_baseGainDb = detail::FreqTableClampedLookup(kGainTable, scaledV);
				m_prevVInGainUpdate = v;
				m_updated = true;
			}
		}

		void PeakingFilterValueController::updateFreq(float v, float vAtFreq, float vAtFreqMax)
		{
			// 値が飛ぶことでノイズが入らないようvの値に対して線形のイージングを入れる
			const bool valueUpdated = m_vEasing.update(v);
			if (valueUpdated)
			{
				const float easedV = m_vEasing.value();
				const float scaledV = std::lerp(vAtFreq, vAtFreqMax, easedV);
				m_freq = detail::FreqTableForwardLookup(kFreqTable, scaledV);
				m_updated = true;
			}
		}

		bool PeakingFilterValueController::popUpdated()
		{
			return std::exchange(m_updated, false);
		}

		float PeakingFilterValueController::baseGainDb() const
		{
			return m_baseGainDb;
		}

		float PeakingFilterValueController::freq() const
		{
			return m_freq;
		}

		bool PeakingFilterValueController::mixSkipped() const
		{
			// 低周波数に対しては適用しない
			return m_freq < kFreqThresholdMin;
		}

		PeakingFilterRelease::PeakingFilterRelease(std::size_t sampleRate)
			: m_filterReleaseFrames(static_cast<std::size_t>(static_cast<float>(sampleRate) * kFilterReleaseSec))
			, m_mixSkippedFrames(m_filterReleaseFrames) // 最初に余韻が入らないよう閾値以上から始める
		{
		}

		void PeakingFilterRelease::update(float freq, float baseGainDb, float mix, bool mixSkipped)
		{
			if (mixSkipped)
			{
				++m_mixSkippedFrames;
			}
			else
			{
				m_freq = freq;
				m_baseGainDb = baseGainDb;
				m_mix = mix;
				m_mixSkippedFrames = 0U;
			}
		}

		bool PeakingFilterRelease::hasValue() const
		{
			return m_mixSkippedFrames < m_filterReleaseFrames;
		}

		float PeakingFilterRelease::freq() const
		{
			return m_freq;
		}

		float PeakingFilterRelease::baseGainDb() const
		{
			const float scale = std::clamp(1.0f - static_cast<float>(m_mixSkippedFrames) / m_filterReleaseFrames, 0.0f, 1.0f);
			return m_baseGainDb * scale;
		}

		float PeakingFilterRelease::mix() const
		{
			return m_mix;
		}
	}

	PeakingFilterDSP::PeakingFilterDSP(const DSPCommonInfo& info)
		: m_info(info)
		, m_release(info.sampleRate)
	{
	}

	void PeakingFilterDSP::process(float* pData, std::size_t dataSize, bool bypass, const PeakingFilterDSPParams& params)
	{
		if (m_info.isUnsupported)
		{
			return;
		}

		const bool isBypassed = bypass || params.mix == 0.0f; // 切り替え時のノイズ回避のためにbypass状態でも処理自体はする

		// freq/freqMaxのvを計算
		if (params.freq != m_prevFreq)
		{
			m_prevVAtFreq = detail::FreqTableInverseLookup(kFreqTable, params.freq);
			m_prevFreq = params.freq;
		}
		if (params.freqMax != m_prevFreqMax)
		{
			m_prevVAtFreqMax = detail::FreqTableInverseLookup(kFreqTable, params.freqMax);
			m_prevFreqMax = params.freqMax;
		}

		assert(dataSize % m_info.numChannels == 0U);
		const std::size_t frameSize = dataSize / m_info.numChannels;
		const bool filterParamsUpdated = params.gainRate != m_prevGainRate || params.bandwidth != m_prevBandwidth;
		m_prevGainRate = params.gainRate;
		m_prevBandwidth = params.bandwidth;
		m_valueController.updateGain(params.v, m_prevVAtFreq, m_prevVAtFreqMax);
		for (std::size_t i = 0U; i < frameSize; ++i)
		{
			m_valueController.updateFreq(params.v, m_prevVAtFreq, m_prevVAtFreqMax);
			const bool mixSkipped = isBypassed || m_valueController.mixSkipped();
			const bool shouldUseRelease = mixSkipped && params.releaseEnabled && m_release.hasValue();
			if (shouldUseRelease)
			{
				// 余韻を適用する必要がある場合はフィルタ係数を毎回更新
				for (std::size_t ch = 0U; ch < m_info.numChannels; ++ch)
				{
					m_peakingFilters[ch].setPeakingFilter(m_release.freq(), params.bandwidth, m_release.baseGainDb() * params.gainRate, m_info.sampleRateFloat);
				}
			}
			else
			{
				const bool valueUpdated = m_valueController.popUpdated();
				if (valueUpdated || (filterParamsUpdated && i == 0U))
				{
					// 値が更新された場合はフィルタ係数を更新
					for (std::size_t ch = 0U; ch < m_info.numChannels; ++ch)
					{
						m_peakingFilters[ch].setPeakingFilter(m_valueController.freq(), params.bandwidth, m_valueController.baseGainDb() * params.gainRate, m_info.sampleRateFloat);
					}
				}
			}

			// 各チャンネルにフィルタを適用
			for (std::size_t ch = 0U; ch < m_info.numChannels; ++ch)
			{
				const float wet = m_peakingFilters[ch].process(*pData);
				if (shouldUseRelease)
				{
					*pData = std::lerp(*pData, wet, m_release.mix());
				}
				else if (!mixSkipped)
				{
					*pData = std::lerp(*pData, wet, params.mix);
				}
				++pData;
			}

			m_release.update(m_valueController.freq(), m_valueController.baseGainDb(), params.mix, mixSkipped);
		}
	}

	void PeakingFilterDSP::updateParams(const PeakingFilterDSPParams& params)
	{
		// 特に何もしない
	}
}

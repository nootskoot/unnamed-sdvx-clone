#include "ksmaudio/AudioEffect/DSP/LowPassFilterDSP.hpp"
#include "ksmaudio/AudioEffect/detail/FreqTableLookup.hpp"

namespace ksmaudio::AudioEffect
{
	namespace
	{
		// 1フレーム(1/44100秒)あたりのvの線形イージングの速さ(最大変化量)
		constexpr float kVEasingSpeed = 0.01f;

		// フィルタ適用可能な最高周波数
		// (高周波を強調すると耳障りになるためしきい値を設けている)
		constexpr float kFreqThresholdMax = 14800.0f;

		// HSP版の計算式から事前計算した周波数テーブル
		// HSP版: https://github.com/kshootmania/ksm-v1/blob/08275836547c7792a6d4f59037e56e947f2979c3/src/scene/play/play_audio_effects.hsp#L957
		constexpr float kFreqTable[detail::kFreqTableSize] = {
			15000.0000f, 14889.7005f, 14779.4103f, 14669.1387f, 14558.8948f, 14448.6880f, 14338.5275f, 14228.4226f,
			14118.3825f, 14008.4164f, 13898.5335f, 13788.7429f, 13679.0539f, 13569.4756f, 13460.0171f, 13350.6874f,
			13241.4957f, 13132.4509f, 13023.5621f, 12914.8382f, 12806.2881f, 12697.9208f, 12589.7450f, 12481.7697f,
			12374.0034f, 12266.4550f, 12159.1331f, 12052.0464f, 11945.2034f, 11838.6125f, 11732.2824f, 11626.2213f,
			11520.4376f, 11414.9396f, 11309.7356f, 11204.8336f, 11100.2418f, 10995.9681f, 10892.0206f, 10788.4072f,
			10685.1356f, 10582.2136f, 10479.6488f, 10377.4489f, 10275.6213f, 10174.1736f, 10073.1130f, 9972.4467f,
			9872.1821f, 9772.3262f, 9672.8860f, 9573.8684f, 9475.2803f, 9377.1285f, 9279.4195f, 9182.1600f,
			9085.3564f, 8989.0151f, 8893.1424f, 8797.7445f, 8702.8275f, 8608.3972f, 8514.4597f, 8421.0208f,
			8328.0860f, 8235.6610f, 8143.7513f, 8052.3623f, 7961.4992f, 7871.1671f, 7781.3712f, 7692.1164f,
			7603.4075f, 7515.2493f, 7427.6464f, 7340.6034f, 7254.1245f, 7168.2142f, 7082.8766f, 6998.1158f,
			6913.9357f, 6830.3402f, 6747.3331f, 6664.9180f, 6583.0983f, 6501.8775f, 6421.2589f, 6341.2456f,
			6261.8407f, 6183.0471f, 6104.8678f, 6027.3053f, 5950.3623f, 5874.0413f, 5798.3447f, 5723.2748f,
			5648.8336f, 5575.0232f, 5501.8456f, 5429.3025f, 5357.3957f, 5286.1267f, 5215.4970f, 5145.5080f,
			5076.1609f, 5007.4569f, 4939.3970f, 4871.9821f, 4805.2130f, 4739.0904f, 4673.6149f, 4608.7870f,
			4544.6071f, 4481.0754f, 4418.1921f, 4355.9572f, 4294.3708f, 4233.4326f, 4173.1424f, 4113.4999f,
			4054.5045f, 3996.1557f, 3938.4528f, 3881.3952f, 3824.9819f, 3769.2119f, 3714.0842f, 3659.5977f,
			3605.7511f, 3552.5431f, 3499.9721f, 3448.0368f, 3396.7355f, 3346.0664f, 3296.0279f, 3246.6179f,
			3197.8345f, 3149.6757f, 3102.1392f, 3055.2230f, 3008.9247f, 2963.2418f, 2918.1719f, 2873.7126f,
			2829.8610f, 2786.6146f, 2743.9705f, 2701.9259f, 2660.4778f, 2619.6233f, 2579.3593f, 2539.6827f,
			2500.5902f, 2462.0785f, 2424.1444f, 2386.7844f, 2349.9950f, 2313.7728f, 2278.1141f, 2243.0152f,
			2208.4726f, 2174.4823f, 2141.0406f, 2108.1436f, 2075.7874f, 2043.9680f, 2012.6814f, 1981.9235f,
			1951.6902f, 1921.9773f, 1892.7805f, 1864.0958f, 1835.9186f, 1808.2448f, 1781.0699f, 1754.3895f,
			1728.1991f, 1702.4943f, 1677.2705f, 1652.5232f, 1628.2479f, 1604.4398f, 1581.0943f, 1558.2068f,
			1535.7725f, 1513.7868f, 1492.2449f, 1471.1420f, 1450.4733f, 1430.2340f, 1410.4194f, 1391.0244f,
			1372.0444f, 1353.4743f, 1335.3094f, 1317.5446f, 1300.1752f, 1283.1961f, 1266.6024f, 1250.3893f,
			1234.5517f, 1219.0847f, 1203.9833f, 1189.2427f, 1174.8577f, 1160.8236f, 1147.1352f, 1133.7877f,
			1120.7761f, 1108.0955f, 1095.7409f, 1083.7073f, 1071.9899f, 1060.5837f, 1049.4838f, 1038.6854f,
			1028.1834f, 1017.9731f, 1008.0496f, 998.4080f, 989.0435f, 979.9513f, 971.1266f, 962.5645f,
			954.2605f, 946.2096f, 938.4072f, 930.8486f, 923.5292f, 916.4442f, 909.5890f, 902.9591f,
			896.5499f, 890.3568f, 884.3754f, 878.6011f, 873.0295f, 867.6561f, 862.4766f, 857.4866f,
			852.6818f, 848.0579f, 843.6106f, 839.3358f, 835.2291f, 831.2866f, 827.5040f, 823.8772f,
			820.4024f, 817.0754f, 813.8923f, 810.8492f, 807.9423f, 805.1676f, 802.5214f, 800.0000f,
		};

	}

	LowPassFilterDSP::LowPassFilterDSP(const DSPCommonInfo& info)
		: m_info(info)
		, m_vEasing(kVEasingSpeed)
	{
	}

	void LowPassFilterDSP::process(float* pData, std::size_t dataSize, bool bypass, const LowPassFilterDSPParams& params)
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
		float scaledV = std::lerp(m_prevVAtFreq, m_prevVAtFreqMax, m_vEasing.value());
		float freq = detail::FreqTableForwardLookup(kFreqTable, scaledV);
		bool mixSkipped = isBypassed || freq > kFreqThresholdMax; // 高周波数に対しては適用しない
		const bool qUpdated = params.q != m_prevQ;
		m_prevQ = params.q;
		for (std::size_t i = 0U; i < frameSize; ++i)
		{
			// 値が飛ぶことでノイズが入らないようvの値に対して線形のイージングを入れる
			const bool vUpdated = m_vEasing.update(params.v);
			if (vUpdated)
			{
				scaledV = std::lerp(m_prevVAtFreq, m_prevVAtFreqMax, m_vEasing.value());
				freq = detail::FreqTableForwardLookup(kFreqTable, scaledV);
				mixSkipped = isBypassed || freq > kFreqThresholdMax; // 高周波数に対しては適用しない
			}

			// 各チャンネルにフィルタを適用
			for (std::size_t ch = 0U; ch < m_info.numChannels; ++ch)
			{
				if (vUpdated || (qUpdated && i == 0U))
				{
					m_lowPassFilters[ch].setLowPassFilter(freq, params.q, m_info.sampleRateFloat);
				}

				const float wet = m_lowPassFilters[ch].process(*pData);
				if (!mixSkipped)
				{
					*pData = std::lerp(*pData, wet, params.mix);
				}
				++pData;
			}
		}
	}

	void LowPassFilterDSP::updateParams(const LowPassFilterDSPParams& params)
	{
		// 特に何もしない
	}
}

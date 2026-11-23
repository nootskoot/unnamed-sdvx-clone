#include "ksmaudio/AudioEffect/DSP/HighPassFilterDSP.hpp"
#include "ksmaudio/AudioEffect/detail/FreqTableLookup.hpp"

namespace ksmaudio::AudioEffect
{
	namespace
	{
		// 1フレーム(1/44100秒)あたりのvの線形イージングの速さ(最大変化量)
		constexpr float kVEasingSpeed = 0.01f;

		// フィルタ適用可能な最低周波数
		// (低周波を強調すると波形の振幅が過剰に大きくなるためしきい値を設けている)
		constexpr float kFreqThresholdMin = 200.0f;

		// HSP版の計算式から事前計算した周波数テーブル
		// HSP版: https://github.com/kshootmania/ksm-v1/blob/08275836547c7792a6d4f59037e56e947f2979c3/src/scene/play/play_audio_effects.hsp#L955
		constexpr float kFreqTable[detail::kFreqTableSize] = {
			100.0000f, 100.0352f, 100.1407f, 100.3166f, 100.5628f, 100.8794f, 101.2663f, 101.7235f,
			102.2511f, 102.8490f, 103.5171f, 104.2556f, 105.0644f, 105.9434f, 106.8927f, 107.9123f,
			109.0021f, 110.1621f, 111.3922f, 112.6926f, 114.0631f, 115.5037f, 117.0145f, 118.5953f,
			120.2462f, 121.9671f, 123.7581f, 125.6190f, 127.5499f, 129.5507f, 131.6214f, 133.7619f,
			135.9723f, 138.2524f, 140.6023f, 143.0220f, 145.5113f, 148.0702f, 150.6988f, 153.3969f,
			156.1645f, 159.0016f, 161.9081f, 164.8840f, 167.9293f, 171.0438f, 174.2276f, 177.4806f,
			180.8027f, 184.1939f, 187.6541f, 191.1833f, 194.7815f, 198.4485f, 202.1843f, 205.9889f,
			209.8622f, 213.8041f, 217.8146f, 221.8936f, 226.0410f, 230.2569f, 234.5410f, 238.8934f,
			243.3140f, 247.8026f, 252.3594f, 256.9840f, 261.6766f, 266.4370f, 271.2652f, 276.1611f,
			281.1245f, 286.1554f, 291.2538f, 296.4196f, 301.6526f, 306.9529f, 312.3202f, 317.7546f,
			323.2560f, 328.8242f, 334.4592f, 340.1608f, 345.9291f, 351.7638f, 357.6650f, 363.6325f,
			369.6663f, 375.7661f, 381.9320f, 388.1639f, 394.4616f, 400.8251f, 407.2542f, 413.7488f,
			420.3089f, 426.9344f, 433.6250f, 440.3809f, 447.2017f, 454.0875f, 461.0381f, 468.0534f,
			475.1333f, 482.2776f, 489.4864f, 496.7594f, 504.0966f, 511.4978f, 518.9629f, 526.4919f,
			534.0845f, 541.7406f, 549.4603f, 557.2432f, 565.0894f, 572.9986f, 580.9708f, 589.0058f,
			597.1035f, 605.2638f, 613.4866f, 621.7717f, 630.1190f, 638.5283f, 646.9996f, 655.5327f,
			664.1274f, 672.7837f, 681.5014f, 690.2803f, 699.1204f, 708.0214f, 716.9833f, 726.0059f,
			735.0891f, 744.2326f, 753.4365f, 762.7005f, 772.0245f, 781.4084f, 790.8519f, 800.3550f,
			809.9175f, 819.5392f, 829.2201f, 838.9599f, 848.7585f, 858.6157f, 868.5315f, 878.5055f,
			888.5378f, 898.6281f, 908.7762f, 918.9821f, 929.2455f, 939.5663f, 949.9443f, 960.3794f,
			970.8714f, 981.4201f, 992.0254f, 1002.6871f, 1013.4050f, 1024.1790f, 1035.0089f, 1045.8946f,
			1056.8358f, 1067.8324f, 1078.8842f, 1089.9911f, 1101.1529f, 1112.3693f, 1123.6403f, 1134.9656f,
			1146.3451f, 1157.7786f, 1169.2659f, 1180.8068f, 1192.4012f, 1204.0489f, 1215.7496f, 1227.5033f,
			1239.3097f, 1251.1686f, 1263.0799f, 1275.0434f, 1287.0588f, 1299.1261f, 1311.2449f, 1323.4152f,
			1335.6366f, 1347.9091f, 1360.2325f, 1372.6065f, 1385.0310f, 1397.5057f, 1410.0305f, 1422.6051f,
			1435.2295f, 1447.9033f, 1460.6264f, 1473.3986f, 1486.2196f, 1499.0894f, 1512.0076f, 1524.9741f,
			1537.9886f, 1551.0511f, 1564.1612f, 1577.3187f, 1590.5235f, 1603.7754f, 1617.0741f, 1630.4195f,
			1643.8112f, 1657.2492f, 1670.7332f, 1684.2630f, 1697.8384f, 1711.4591f, 1725.1250f, 1738.8359f,
			1752.5915f, 1766.3916f, 1780.2360f, 1794.1245f, 1808.0568f, 1822.0328f, 1836.0522f, 1850.1149f,
			1864.2205f, 1878.3689f, 1892.5598f, 1906.7931f, 1921.0685f, 1935.3858f, 1949.7447f, 1964.1450f,
			1978.5866f, 1993.0691f, 2007.5924f, 2022.1563f, 2036.7604f, 2051.4046f, 2066.0886f, 2080.8123f,
			2095.5754f, 2110.3776f, 2125.2187f, 2140.0985f, 2155.0168f, 2169.9733f, 2184.9678f, 2200.0000f,
		};

	}

	HighPassFilterDSP::HighPassFilterDSP(const DSPCommonInfo& info)
		: m_info(info)
		, m_vEasing(kVEasingSpeed)
	{
	}

	void HighPassFilterDSP::process(float* pData, std::size_t dataSize, bool bypass, const HighPassFilterDSPParams& params)
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
		bool mixSkipped = isBypassed || freq < kFreqThresholdMin; // 低周波数に対しては適用しない
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
				mixSkipped = isBypassed || freq < kFreqThresholdMin; // 低周波数に対しては適用しない
			}

			// 各チャンネルにフィルタを適用
			for (std::size_t ch = 0U; ch < m_info.numChannels; ++ch)
			{
				if (vUpdated || (qUpdated && i == 0U))
				{
					m_highPassFilters[ch].setHighPassFilter(freq, params.q, m_info.sampleRateFloat);
				}

				const float wet = m_highPassFilters[ch].process(*pData);
				if (!mixSkipped)
				{
					*pData = std::lerp(*pData, wet, params.mix);
				}
				++pData;
			}
		}
	}

	void HighPassFilterDSP::updateParams(const HighPassFilterDSPParams& params)
	{
		// 特に何もしない
	}
}

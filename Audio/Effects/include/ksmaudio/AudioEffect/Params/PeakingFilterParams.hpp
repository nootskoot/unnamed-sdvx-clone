#pragma once
#include <algorithm>
#include <cmath>
#include <cassert>
#include "ksmaudio/AudioEffect/AudioEffectParam.hpp"

namespace ksmaudio::AudioEffect
{
	struct PeakingFilterDSPParams
	{
		float v = 0.0f;
		float freq = 50.0f;
		float freqMax = 9000.0f;
		float gainRate = 0.5f;
		float bandwidth = 1.2f;
		float mix = 1.0f;
		bool releaseEnabled = false;
	};

	struct PeakingFilterParams
	{
		Param v = DefineParam(Type::kRate, "0%-100%");
		Param freq = DefineParam(Type::kFreq, "50Hz");
		Param freqMax = DefineParam(Type::kFreq, "9000Hz");
		Param bandwidth = DefineParam(Type::kFloat, "1.2");
		Param gain = DefineParam(Type::kRate, "50%");
		Param mix = DefineParam(Type::kRate, "0%>100%");

		const std::unordered_map<ParamID, Param*> dict = {
			{ ParamID::kV, &v },
			{ ParamID::kFreq, &freq },
			{ ParamID::kFreqMax, &freqMax },
			{ ParamID::kBandwidth, &bandwidth },
			{ ParamID::kGain, &gain },
			{ ParamID::kMix, &mix },
		};

		PeakingFilterDSPParams renderByFX(const Status& status, std::optional<std::size_t> laneIdx)
		{
			const bool isOn = laneIdx.has_value();
			return {
				.v = GetValue(v, status, isOn),
				.freq = GetValue(freq, status, isOn),
				.freqMax = GetValue(freqMax, status, isOn),
				.gainRate = GetValue(gain, status, isOn),
				.bandwidth = GetValue(bandwidth, status, isOn),
				.mix = GetValue(mix, status, isOn),
				.releaseEnabled = false, // FXでは余韻を無効にする
			};
		}

		PeakingFilterDSPParams renderByLaser(const Status& status, bool isOn)
		{
			return {
				.v = GetValue(v, status, isOn),
				.freq = GetValue(freq, status, isOn),
				.freqMax = GetValue(freqMax, status, isOn),
				.gainRate = GetValue(gain, status, isOn),
				.bandwidth = GetValue(bandwidth, status, isOn),
				.mix = GetValue(mix, status, isOn),
				.releaseEnabled = true, // LASERでは有効。v1ではp/fp音源からリアルタイムエフェクトへのフォールバック時は余韻無効化していたが、差異が軽微のためv2では常に有効とする
			};
		}
	};
}

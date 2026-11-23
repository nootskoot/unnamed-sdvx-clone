#pragma once
#include <algorithm>
#include <cmath>
#include <cassert>
#include "ksmaudio/AudioEffect/AudioEffectParam.hpp"

namespace ksmaudio::AudioEffect
{
	struct LowPassFilterDSPParams
	{
		float v = 0.0f;
		float freq = 15000.0f;
		float freqMax = 800.0f;
		float q = 3.6f;
		float mix = 1.0f;
	};

	struct LowPassFilterParams
	{
		Param v = DefineParam(Type::kRate, "0%-100%");
		Param freq = DefineParam(Type::kFreq, "15000Hz");
		Param freqMax = DefineParam(Type::kFreq, "800Hz");
		Param q = DefineParam(Type::kFloat, "3.6");
		Param mix = DefineParam(Type::kRate, "0%>100%");

		const std::unordered_map<ParamID, Param*> dict = {
			{ ParamID::kV, &v },
			{ ParamID::kFreq, &freq },
			{ ParamID::kFreqMax, &freqMax },
			{ ParamID::kQ, &q },
			{ ParamID::kMix, &mix },
		};

		LowPassFilterDSPParams render(const Status& status, bool isOn)
		{
			return {
				.v = GetValue(v, status, isOn),
				.freq = GetValue(freq, status, isOn),
				.freqMax = GetValue(freqMax, status, isOn),
				.q = GetValue(q, status, isOn),
				.mix = GetValue(mix, status, isOn),
			};
		}

		LowPassFilterDSPParams renderByFX(const Status& status, std::optional<std::size_t> laneIdx)
		{
			const bool isOn = laneIdx.has_value();
			return render(status, isOn);
		}

		LowPassFilterDSPParams renderByLaser(const Status& status, bool isOn)
		{
			return render(status, isOn);
		}
	};
}

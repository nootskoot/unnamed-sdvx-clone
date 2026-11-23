#pragma once
#include <algorithm>
#include <unordered_map>
#include "ksmaudio/AudioEffect/AudioEffectParam.hpp"

namespace ksmaudio::AudioEffect
{
	struct PhaserDSPParams
	{
		float period = 4.0f; // 4s = 2 bars at 120 BPM
		std::size_t stage = 6U;
		float freq1 = 1500.0f;
		float freq2 = 20000.0f;
		float q = 0.707f;
		float feedback = 0.35f;
		float stereoWidth = 0.0f;
		float hiCutGain = -8.0f;
		float mix = 0.5f;
	};

	struct PhaserParams
	{
		Param period = DefineParam(Type::kLength, "1/2");
		Param stage = DefineParam(Type::kInt, "6");
		Param freq1 = DefineParam(Type::kFreq, "1500Hz");
		Param freq2 = DefineParam(Type::kFreq, "20000Hz");
		Param q = DefineParam(Type::kFloat, "0.707");
		Param feedback = DefineParam(Type::kRate, "35%");
		Param stereoWidth = DefineParam(Type::kRate, "75%");
		Param hiCutGain = DefineParam(Type::kDB, "-8.0dB");
		Param mix = DefineParam(Type::kRate, "0%>50%");

		const std::unordered_map<ParamID, Param*> dict = {
			{ ParamID::kPeriod, &period },
			{ ParamID::kStage, &stage },
			{ ParamID::kFreq1, &freq1 },
			{ ParamID::kFreq2, &freq2 },
			{ ParamID::kQ, &q },
			{ ParamID::kFeedback, &feedback },
			{ ParamID::kStereoWidth, &stereoWidth },
			{ ParamID::kHiCutGain, &hiCutGain },
			{ ParamID::kMix, &mix },
		};

		PhaserDSPParams render(const Status& status, bool isOn)
		{
			return {
				.period = GetValue(period, status, isOn),
				.stage = static_cast<std::size_t>(std::clamp(GetValueAsInt(stage, status, isOn), 0, 12)),
				.freq1 = GetValue(freq1, status, isOn),
				.freq2 = GetValue(freq2, status, isOn),
				.q = GetValue(q, status, isOn),
				.feedback = GetValue(feedback, status, isOn),
				.stereoWidth = GetValue(stereoWidth, status, isOn),
				.hiCutGain = GetValue(hiCutGain, status, isOn),
				.mix = GetValue(mix, status, isOn),
			};
		}

		PhaserDSPParams renderByFX(const Status& status, std::optional<std::size_t> laneIdx)
		{
			const bool isOn = laneIdx.has_value();
			return render(status, isOn);
		}

		PhaserDSPParams renderByLaser(const Status& status, bool isOn)
		{
			return render(status, isOn);
		}
	};
}

#pragma once
#include <algorithm>
#include <unordered_map>
#include "ksmaudio/AudioEffect/AudioEffectParam.hpp"

namespace ksmaudio::AudioEffect
{
	struct PitchShiftDSPParams
	{
		float pitch = 0.0f;
		std::size_t chunkSize = 700U;
		float overlap = 0.4f;
		float mix = 1.0f;
	};

	struct PitchShiftParams
	{
		Param pitch = DefineParam(Type::kPitch, "0>12");
		Param chunkSize = DefineParam(Type::kSample, "700samples");
		Param overlap = DefineParam(Type::kRate, "40%");
		Param mix = DefineParam(Type::kRate, "0%>100%");

		const std::unordered_map<ParamID, Param*> dict = {
			{ ParamID::kPitch, &pitch },
			{ ParamID::kChunkSize, &chunkSize },
			{ ParamID::kOverlap, &overlap },
			{ ParamID::kMix, &mix },
		};

		PitchShiftDSPParams render(const Status& status, bool isOn)
		{
			return {
				.pitch = GetValue(pitch, status, isOn),
				.chunkSize = static_cast<std::size_t>(std::clamp(GetValueAsInt(chunkSize, status, isOn), 1, 44100)),
				.overlap = std::clamp(GetValue(overlap, status, isOn), 0.0f, 0.5f),
				.mix = GetValue(mix, status, isOn),
			};
		}

		PitchShiftDSPParams renderByFX(const Status& status, std::optional<std::size_t> laneIdx)
		{
			const bool isOn = laneIdx.has_value();
			return render(status, isOn);
		}

		PitchShiftDSPParams renderByLaser(const Status& status, bool isOn)
		{
			return render(status, isOn);
		}
	};
}

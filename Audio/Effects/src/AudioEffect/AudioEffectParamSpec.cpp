#include "ksmaudio/AudioEffect/AudioEffectParamSpec.hpp"
#include <unordered_map>
#include <functional>

namespace ksmaudio::AudioEffect
{
	namespace
	{
		// string_viewキー用ハッシュ
		struct StringHash
		{
			using is_transparent = void;

			std::size_t operator()(std::string_view sv) const noexcept
			{
				return std::hash<std::string_view>{}(sv);
			}

			std::size_t operator()(const std::string& s) const noexcept
			{
				return std::hash<std::string>{}(s);
			}
		};

		// retrigger
		const std::vector<ParamSpec> s_retriggerParams = {
			{ ParamID::kUpdatePeriod, "update_period", Type::kLength, "1/2" },
			{ ParamID::kWaveLength, "wave_length", Type::kWaveLength, "0" },
			{ ParamID::kRate, "rate", Type::kRate, "70%" },
			{ ParamID::kUpdateTrigger, "update_trigger", Type::kSwitch, "off" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>100%" },
		};

		// gate
		const std::vector<ParamSpec> s_gateParams = {
			{ ParamID::kWaveLength, "wave_length", Type::kWaveLength, "0" },
			{ ParamID::kRate, "rate", Type::kRate, "50%" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>90%" },
		};

		// flanger
		const std::vector<ParamSpec> s_flangerParams = {
			{ ParamID::kPeriod, "period", Type::kLength, "2" },
			{ ParamID::kDelay, "delay", Type::kSample, "30samples" },
			{ ParamID::kDepth, "depth", Type::kSample, "45samples" },
			{ ParamID::kFeedback, "feedback", Type::kRate, "60%" },
			{ ParamID::kStereoWidth, "stereo_width", Type::kRate, "0%" },
			{ ParamID::kVol, "vol", Type::kRate, "75%" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>80%" },
		};

		// pitch_shift
		const std::vector<ParamSpec> s_pitchShiftParams = {
			{ ParamID::kPitch, "pitch", Type::kPitch, "0>12" },
			{ ParamID::kChunkSize, "chunk_size", Type::kSample, "700samples" },
			{ ParamID::kOverlap, "overlap", Type::kRate, "40%" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>100%" },
		};

		// bitcrusher
		const std::vector<ParamSpec> s_bitcrusherParams = {
			{ ParamID::kReduction, "reduction", Type::kSample, "0samples-30samples" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>100%" },
		};

		// phaser
		const std::vector<ParamSpec> s_phaserParams = {
			{ ParamID::kPeriod, "period", Type::kLength, "1/2" },
			{ ParamID::kStage, "stage", Type::kInt, "6" },
			{ ParamID::kFreq1, "freq_1", Type::kFreq, "1500Hz" },
			{ ParamID::kFreq2, "freq_2", Type::kFreq, "20000Hz" },
			{ ParamID::kQ, "q", Type::kFloat, "0.707" },
			{ ParamID::kFeedback, "feedback", Type::kRate, "35%" },
			{ ParamID::kStereoWidth, "stereo_width", Type::kRate, "75%" },
			{ ParamID::kHiCutGain, "hi_cut_gain", Type::kDB, "-8dB" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>50%" },
		};

		// wobble
		const std::vector<ParamSpec> s_wobbleParams = {
			{ ParamID::kWaveLength, "wave_length", Type::kWaveLength, "0" },
			{ ParamID::kFreq1, "freq_1", Type::kFreq, "500Hz" },
			{ ParamID::kFreq2, "freq_2", Type::kFreq, "20000Hz" },
			{ ParamID::kQ, "q", Type::kFloat, "1.414" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>80%" },
		};

		// tapestop
		const std::vector<ParamSpec> s_tapestopParams = {
			{ ParamID::kSpeed, "speed", Type::kRate, "50%" },
			{ ParamID::kTrigger, "trigger", Type::kSwitch, "off>on" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>100%" },
		};

		// echo
		const std::vector<ParamSpec> s_echoParams = {
			{ ParamID::kUpdatePeriod, "update_period", Type::kLength, "0" },
			{ ParamID::kWaveLength, "wave_length", Type::kWaveLength, "0" },
			{ ParamID::kUpdateTrigger, "update_trigger", Type::kSwitch, "off>on" },
			{ ParamID::kFeedbackLevel, "feedback_level", Type::kRate, "60%" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>100%" },
		};

		// sidechain
		const std::vector<ParamSpec> s_sidechainParams = {
			{ ParamID::kPeriod, "period", Type::kLength, "1/4" },
			{ ParamID::kHoldTime, "hold_time", Type::kLength, "50ms" },
			{ ParamID::kAttackTime, "attack_time", Type::kLength, "10ms" },
			{ ParamID::kReleaseTime, "release_time", Type::kLength, "1/8" },
			{ ParamID::kRatio, "ratio", Type::kFloat, "1>5" },
		};

		// switch_audio
		const std::vector<ParamSpec> s_switchAudioParams = {
			{ ParamID::kFilename, "filename", Type::kFilename, "" },
		};

		// peaking_filter
		const std::vector<ParamSpec> s_peakingFilterParams = {
			{ ParamID::kV, "v", Type::kRate, "0%-100%" },
			{ ParamID::kFreq, "freq", Type::kFreq, "50Hz" },
			{ ParamID::kFreqMax, "freq_max", Type::kFreq, "9000Hz" },
			{ ParamID::kBandwidth, "bandwidth", Type::kFloat, "1.2" },
			{ ParamID::kGain, "gain", Type::kRate, "50%" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>100%" },
		};

		// high_pass_filter
		const std::vector<ParamSpec> s_highPassFilterParams = {
			{ ParamID::kV, "v", Type::kRate, "0%-100%" },
			{ ParamID::kFreq, "freq", Type::kFreq, "100Hz" },
			{ ParamID::kFreqMax, "freq_max", Type::kFreq, "2200Hz" },
			{ ParamID::kQ, "q", Type::kFloat, "5.0" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>100%" },
		};

		// low_pass_filter
		const std::vector<ParamSpec> s_lowPassFilterParams = {
			{ ParamID::kV, "v", Type::kRate, "0%-100%" },
			{ ParamID::kFreq, "freq", Type::kFreq, "15000Hz" },
			{ ParamID::kFreqMax, "freq_max", Type::kFreq, "800Hz" },
			{ ParamID::kQ, "q", Type::kFloat, "3.6" },
			{ ParamID::kMix, "mix", Type::kRate, "0%>100%" },
		};

		const std::unordered_map<std::string, const std::vector<ParamSpec>*, StringHash, std::equal_to<>> s_effectParamSpecs = {
			{ "retrigger", &s_retriggerParams },
			{ "gate", &s_gateParams },
			{ "flanger", &s_flangerParams },
			{ "pitch_shift", &s_pitchShiftParams },
			{ "bitcrusher", &s_bitcrusherParams },
			{ "phaser", &s_phaserParams },
			{ "wobble", &s_wobbleParams },
			{ "tapestop", &s_tapestopParams },
			{ "echo", &s_echoParams },
			{ "sidechain", &s_sidechainParams },
			{ "switch_audio", &s_switchAudioParams },
			{ "peaking_filter", &s_peakingFilterParams },
			{ "high_pass_filter", &s_highPassFilterParams },
			{ "low_pass_filter", &s_lowPassFilterParams },
		};

		const std::vector<std::string_view> s_effectTypeNames = {
			"retrigger",
			"gate",
			"flanger",
			"pitch_shift",
			"bitcrusher",
			"phaser",
			"wobble",
			"tapestop",
			"echo",
			"sidechain",
			"switch_audio",
			"peaking_filter",
			"high_pass_filter",
			"low_pass_filter",
		};
	}

	const std::vector<ParamSpec>* GetParamSpecs(std::string_view effectType)
	{
		auto it = s_effectParamSpecs.find(effectType);
		return (it != s_effectParamSpecs.end()) ? it->second : nullptr;
	}

	const std::vector<std::string_view>& GetEffectTypeNames()
	{
		return s_effectTypeNames;
	}

	bool IsValidParam(std::string_view effectType, std::string_view paramName)
	{
		const auto* specs = GetParamSpecs(effectType);
		if (specs == nullptr)
		{
			return false;
		}

		for (const auto& spec : *specs)
		{
			if (spec.name == paramName)
			{
				return true;
			}
		}
		return false;
	}

	Type GetParamType(std::string_view effectType, std::string_view paramName)
	{
		const auto* spec = GetParamSpec(effectType, paramName);
		return (spec != nullptr) ? spec->type : Type::kUnspecified;
	}

	const ParamSpec* GetParamSpec(std::string_view effectType, std::string_view paramName)
	{
		const auto* specs = GetParamSpecs(effectType);
		if (specs == nullptr)
		{
			return nullptr;
		}

		for (const auto& spec : *specs)
		{
			if (spec.name == paramName)
			{
				return &spec;
			}
		}
		return nullptr;
	}
}

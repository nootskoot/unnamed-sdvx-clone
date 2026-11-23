#include "ksmaudio/AudioEffect/AudioEffectParam.hpp"
#include <string>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <iostream>
#include "ksmaudio/AudioEffect/detail/WaveLengthUtils.hpp"

namespace ksmaudio::AudioEffect
{
	namespace
	{
		// std::stofと同様にfloatをパース(std::stofと異なり、nan/infの場合もstd::invalid_argumentを投げる)
		float StofFinite(const std::string& str, std::size_t* pPos = nullptr)
		{
			const float value = std::stof(str, pPos);
			if (!std::isfinite(value))
			{
				throw std::invalid_argument("non-finite value");
			}
			return value;
		}

		// suffixの検証付きでfloatをパース
		bool ParseFloatWithSuffix(const std::string& str, const std::string& suffix, float* pOutValue, const std::string& typeName = "")
		{
			std::size_t pos = 0;
			try
			{
				*pOutValue = StofFinite(str, &pos);
			}
			catch (...)
			{
				if (!typeName.empty())
				{
					std::cerr << "[ksmaudio warning] " << typeName << " parse error: " << str << std::endl;
				}
				return false;
			}

			// 不正な文字列を受け付けないようsuffixの開始位置をチェック
			if (pos + suffix.length() != str.length())
			{
				if (!typeName.empty())
				{
					std::cerr << "[ksmaudio warning] " << typeName << " invalid suffix: " << str << std::endl;
				}
				return false;
			}

			if (str.substr(pos) != suffix)
			{
				if (!typeName.empty())
				{
					std::cerr << "[ksmaudio warning] " << typeName << " invalid suffix: " << str << std::endl;
				}
				return false;
			}

			return true;
		}
	}

	// HSP版: https://github.com/kshootmania/ksm-v1/blob/19bfb6acbec8abd304b2e7dae6009df8e8e1f66f/src/scene/play/play_utils.hsp#L405
	float StrToValue(Type type, const std::string& str, bool* pSuccess)
	{
		if (pSuccess != nullptr)
		{
			*pSuccess = true;
		}

		try
		{
			switch (type)
			{
			case Type::kUnspecified:
				return 0.0f;

			case Type::kLength:
			case Type::kWaveLength:
				// value
				//   > 0: measure
				//   < 0: sec
				if (str.ends_with("ms"))
				{
					float value = 0.0f;
					if (!ParseFloatWithSuffix(str, "ms", &value, "length"))
					{
						if (pSuccess != nullptr)
						{
							*pSuccess = false;
						}
						return 0.0f;
					}
					return -std::max(value, 0.0f) / 1000;
				}
				else if (str.ends_with('s') && !str.ends_with("es")) // Do not allow "XXXsamples"
				{
					float value = 0.0f;
					if (!ParseFloatWithSuffix(str, "s", &value, "length"))
					{
						if (pSuccess != nullptr)
						{
							*pSuccess = false;
						}
						return 0.0f;
					}
					return -std::max(value, 0.0f);
				}
				else if (str.starts_with("1/"))
				{
					const int d = std::stoi(str.substr(2U)); // 2 = strlen("1/")
					if (d > 0)
					{
						return 1.0f / d;
					}
					else
					{
						if (pSuccess != nullptr)
						{
							*pSuccess = false;
						}
						return 0.0f;
					}
				}
				{
					const float value = StofFinite(str);
					if (value < 0.0f)
					{
						std::cerr << "[ksmaudio warning] length value out of range (>= 0.0): " << value << std::endl;
					}
					return std::max(value, 0.0f);
				}

			case Type::kSample:
				if (str.ends_with("samples"))
				{
					float value = 0.0f;
					if (!ParseFloatWithSuffix(str, "samples", &value, "sample"))
					{
						if (pSuccess != nullptr)
						{
							*pSuccess = false;
						}
						return 0.0f;
					}
					if (value < 0.0f || value > 44100.0f)
					{
						std::cerr << "[ksmaudio warning] sample value out of range (0-44100): " << value << std::endl;
					}
					return std::clamp(value, 0.0f, 44100.0f);
				}
				else
				{
					if (pSuccess != nullptr)
					{
						*pSuccess = false;
					}
				}
				break;

			case Type::kSwitch:
				if (str == "on")
				{
					return 1.0f;
				}
				return 0.0f;

			case Type::kRate:
				if (str.ends_with('%'))
				{
					float value = 0.0f;
					if (!ParseFloatWithSuffix(str, "%", &value, "rate"))
					{
						if (pSuccess != nullptr)
						{
							*pSuccess = false;
						}
						return 0.0f;
					}
					value = value / 100;
					if (value < 0.0f || value > 1.0f)
					{
						std::cerr << "[ksmaudio warning] rate value out of range (0.0-1.0): " << value << std::endl;
					}
					return std::clamp(value, 0.0f, 1.0f);
				}
				else if (str.starts_with("1/"))
				{
					const int d = std::stoi(str.substr(2U)); // 2 = strlen("1/")
					if (d > 0)
					{
						return std::clamp(1.0f / d, 0.0f, 1.0f);
					}
					else
					{
						if (pSuccess != nullptr)
						{
							*pSuccess = false;
						}
						return 0.0f;
					}
				}
				{
					const float value = StofFinite(str);
					if (value < 0.0f || value > 1.0f)
					{
						std::cerr << "[ksmaudio warning] rate value out of range (0.0-1.0): " << value << std::endl;
					}
					return std::clamp(value, 0.0f, 1.0f);
				}

			case Type::kFreq:
				if (str.ends_with("kHz"))
				{
					float value = 0.0f;
					if (!ParseFloatWithSuffix(str, "kHz", &value, "freq"))
					{
						if (pSuccess != nullptr)
						{
							*pSuccess = false;
						}
						return 10.0f;
					}
					const float freq = std::max(value, 0.0f) * 1000;
					if (freq < 10.0f || freq > 20000.0f)
					{
						std::cerr << "[ksmaudio warning] freq value out of range (10-20000Hz): " << freq << "Hz" << std::endl;
					}
					return std::clamp(freq, 10.0f, 20000.0f);
				}
				else if (str.ends_with("Hz"))
				{
					float value = 0.0f;
					if (!ParseFloatWithSuffix(str, "Hz", &value, "freq"))
					{
						if (pSuccess != nullptr)
						{
							*pSuccess = false;
						}
						return 10.0f;
					}
					const float freq = std::max(value, 0.0f);
					if (freq < 10.0f || freq > 20000.0f)
					{
						std::cerr << "[ksmaudio warning] freq value out of range (10-20000Hz): " << freq << "Hz" << std::endl;
					}
					return std::clamp(freq, 10.0f, 20000.0f);
				}
				return 0.0f;

			case Type::kPitch:
				// param
				//   > 0: not quantized (real_value + 48)
				//   < 0: quantized (-real_value - 48)
				{
					const float value = StofFinite(str);
					if (value < -48.0f || value > 48.0f)
					{
						std::cerr << "[ksmaudio warning] pitch value out of range (-48.0-48.0): " << value << std::endl;
					}
					const float pitch = std::clamp(value, -48.0f, 48.0f);
					if (str.find('.') != std::string::npos)
					{
						return pitch + 48.0f;
					}
					else
					{
						return -(pitch + 48.0f);
					}
				}

			case Type::kInt:
				return static_cast<float>(std::stoi(str));

			case Type::kFloat:
				return StofFinite(str);

			case Type::kDB:
				if (str.ends_with("dB"))
				{
					float value = 0.0f;
					if (!ParseFloatWithSuffix(str, "dB", &value, "dB"))
					{
						if (pSuccess != nullptr)
						{
							*pSuccess = false;
						}
						return 0.0f;
					}
					return value;
				}
				return 0.0f;

			case Type::kFilename:
				return 0.0f;
			}
		}
		catch ([[maybe_unused]] const std::invalid_argument& e)
		{
			if (pSuccess != nullptr)
			{
				*pSuccess = false;
			}
		}
		catch ([[maybe_unused]] const std::out_of_range& e)
		{
			if (pSuccess != nullptr)
			{
				*pSuccess = false;
			}
		}

		if (pSuccess != nullptr)
		{
			*pSuccess = false;
		}
		return 0.0f;
	}

	ValueSet StrToValueSet(Type type, const std::string& str, bool* pSuccess)
	{
		const std::size_t pos1 = str.find('>');
		const std::size_t pos2FindStart = ((pos1 == std::string::npos) ? 0U : pos1 + 1U/*'>'*/) + 1U/*negative sign '-'*/;
		const std::size_t pos2 = str.find('-', pos2FindStart);

		const std::string offStr = (pos1 == std::string::npos) ? ((pos2 == std::string::npos) ? str : str.substr(0U, pos2)) : str.substr(0U, pos1);
		const std::string onMinStr = (pos1 == std::string::npos) ? offStr : ((pos2 == std::string::npos) ? str.substr(pos1 + 1U/*'>'*/) : str.substr(pos1 + 1U/*'>'*/, pos2 - pos1 - 1U));
		const std::string onMaxStr = (pos2 == std::string::npos) ? onMinStr : str.substr(pos2 + 1U/*'-'*/);

		bool offSuccess = true;
		bool onMinSuccess = true;
		bool onMaxSuccess = true;

		ValueSet valueSet = {
			.off = StrToValue(type, offStr, &offSuccess),
			.onMin = StrToValue(type, onMinStr, &onMinSuccess),
			.onMax = StrToValue(type, onMaxStr, &onMaxSuccess),
		};

		// いずれかのパースに失敗した場合
		if (!offSuccess || !onMinSuccess || !onMaxSuccess)
		{
			if (pSuccess != nullptr)
			{
				*pSuccess = false;
			}
			return {};
		}

		// For length parameters, the min and max values must have the same sign.
		// Otherwise, a value set of 0 is returned.
		if (type == Type::kLength && ((valueSet.onMin < 0.0f) != (valueSet.onMax < 0.0f)))
		{
			if (pSuccess != nullptr)
			{
				*pSuccess = false;
			}
			return {};
		}

		if (pSuccess != nullptr)
		{
			*pSuccess = true;
		}

		// For pitch parameters, the min and max values must have the same sign.
		// Otherwise, quantization is disabled (i.e., "0.0-12" is replaced by "0.0-12.0").
		if (type == Type::kPitch && ((valueSet.onMin < 0.0f) != (valueSet.onMax < 0.0f)))
		{
			return {
				.off = valueSet.off,
				.onMin = std::abs(valueSet.onMin),
				.onMax = std::abs(valueSet.onMax),
			};
		}

		return valueSet;
	}

	bool ValueAsBool(float value)
	{
		constexpr float kBoolThreshold = 0.999f;
		return value > kBoolThreshold;
	}

	float GetValue(const Param& param, const Status& status, bool isOn)
	{
		const float lerped = isOn ? std::lerp(param.valueSet.onMin, param.valueSet.onMax, status.v) : param.valueSet.off;
		const float effectivePlaybackSpeed = status.playbackSpeed != 0.0f ? status.playbackSpeed : 1.0f;

		if (param.type == Type::kLength)
		{
			if (lerped > 0.0f)
			{
				// Tempo-synced
				return lerped * 4 * 60 / status.bpm;
			}
			else
			{
				// Not tempo-synced
				return -lerped / effectivePlaybackSpeed;
			}
		}

		if (param.type == Type::kWaveLength)
		{
			if (lerped > 0.0f)
			{
				// Tempo-synced
				float waveLength;
				if (isOn)
				{
					waveLength = detail::WaveLengthUtils::Interpolate(param.valueSet.onMin, param.valueSet.onMax, status.v);
				}
				else
				{
					waveLength = param.valueSet.off;
				}
				return waveLength * 4 * 60 / status.bpm;
			}
			else
			{
				// Not tempo-synced
				return -lerped / effectivePlaybackSpeed;
			}
		}
		
		if (param.type == Type::kPitch)
		{
			if (lerped > 0.0f)
			{
				// Not quantized
				return lerped - 48.0f;
			}
			else
			{
				// Quantized
				return std::floor(-lerped - 48.0f);
			}
		}

		return lerped;
	}

	bool GetValueAsBool(const Param& param, const Status& status, bool isOn)
	{
		return ValueAsBool(GetValue(param, status, isOn));
	}

	int GetValueAsInt(const Param& param, const Status& status, bool isOn)
	{
		return static_cast<int>(GetValue(param, status, isOn));
	}

	Param DefineParam(Type type, const std::string& valueSetStr)
	{
		return {
			.type = type,
			.valueSet = StrToValueSet(type, valueSetStr),
		};
	}

	TapestopTriggerParam DefineTapestopTriggerParam(const std::string& valueSetStr)
	{
		return TapestopTriggerParam{ StrToValueSet(Type::kSwitch, valueSetStr) };
	}

	UpdateTriggerParam DefineUpdateTriggerParam(const std::string& valueSetStr)
	{
		return UpdateTriggerParam{ StrToValueSet(Type::kSwitch, valueSetStr) };
	}
}

#include "ksmaudio/AudioEffect/AudioEffectParamValidator.hpp"
#include "ksmaudio/AudioEffect/AudioEffectParamSpec.hpp"
#include <regex>
#include <cstdlib>
#include <cerrno>
#include <climits>

namespace ksmaudio::AudioEffect
{
	namespace
	{
		// doubleをパース(floatの範囲を超える値は失敗)
		bool ParseDouble(const std::string& str, double& outValue)
		{
			if (str.empty())
			{
				return false;
			}
			char* end = nullptr;
			errno = 0;
			outValue = std::strtod(str.c_str(), &end);
			if (errno != 0 || end != str.c_str() + str.size())
			{
				return false;
			}
			errno = 0;
			std::strtof(str.c_str(), nullptr);
			return errno == 0;
		}

		// intをパース
		bool ParseInt(const std::string& str, int& outValue)
		{
			if (str.empty())
			{
				return false;
			}
			char* end = nullptr;
			errno = 0;
			long value = std::strtol(str.c_str(), &end, 10);
			if (errno != 0 || end != str.c_str() + str.size())
			{
				return false;
			}
			if (value < INT_MIN || value > INT_MAX)
			{
				// longがintより大きい環境での桁あふれ
				return false;
			}
			outValue = static_cast<int>(value);
			return true;
		}

		// 単一値のバリデーション
		ValidationResult ValidateSingleValue(Type type, std::string_view value)
		{
			ValidationResult result;
			const std::string valueStr(value);

			switch (type)
			{
			case Type::kLength:
			case Type::kWaveLength:
			{
				// 形式: 1/[int], [float], [float]ms, [float]s
				static const std::regex reFrac(R"(^1/(\d+)$)");
				static const std::regex reMs(R"(^(-?\d+\.?\d*|\d*\.?\d+)ms$)");
				static const std::regex reSec(R"(^(-?\d+\.?\d*|\d*\.?\d+)s$)");
				static const std::regex reFloat(R"(^(-?\d+\.?\d*|\d*\.?\d+)$)");

				std::smatch matchFrac;
				if (std::regex_match(valueStr, matchFrac, reFrac))
				{
					int denom = 0;
					if (!ParseInt(matchFrac[1].str(), denom) || denom < 1)
					{
						result.isValid = false;
						result.errorMessage = "Denominator must be >= 1";
					}
					return result;
				}

				std::smatch matchTime;
				if (std::regex_match(valueStr, matchTime, reMs) || std::regex_match(valueStr, matchTime, reSec))
				{
					double v = 0.0;
					if (!ParseDouble(matchTime[1].str(), v) || v < 0.0)
					{
						result.isValid = false;
						result.errorMessage = "Value must be >= 0";
					}
					return result;
				}

				if (std::regex_match(valueStr, reFloat))
				{
					double v = 0.0;
					if (!ParseDouble(valueStr, v) || v < 0.0)
					{
						result.isValid = false;
						result.errorMessage = "Value must be >= 0";
					}
					return result;
				}

				result.isValid = false;
				result.errorMessage = "Valid formats: 1/[int], [float], [float]ms, [float]s";
				return result;
			}

			case Type::kSample:
			{
				// 形式: [int]samples (0〜44100)
				static const std::regex re(R"(^(\d+)samples$)");
				std::smatch match;
				if (!std::regex_match(valueStr, match, re))
				{
					result.isValid = false;
					result.errorMessage = "Valid format: [int]samples";
					return result;
				}

				int v = 0;
				if (!ParseInt(match[1].str(), v) || v < 0 || v > 44100)
				{
					result.isValid = false;
					result.errorMessage = "Value must be 0-44100";
				}
				return result;
			}

			case Type::kSwitch:
			{
				// 形式: on, off
				if (value != "on" && value != "off")
				{
					result.isValid = false;
					result.errorMessage = "Valid values: on, off";
				}
				return result;
			}

			case Type::kRate:
			{
				// 形式: 1/[int], [int]%, [float] (0.0〜1.0)
				static const std::regex reFrac(R"(^1/(\d+)$)");
				static const std::regex rePercent(R"(^(\d+)%$)");
				static const std::regex reFloat(R"(^(-?\d+\.?\d*|\d*\.?\d+)$)");

				std::smatch matchFrac;
				if (std::regex_match(valueStr, matchFrac, reFrac))
				{
					int denom = 0;
					if (!ParseInt(matchFrac[1].str(), denom) || denom < 1)
					{
						result.isValid = false;
						result.errorMessage = "Denominator must be >= 1";
					}
					return result;
				}

				std::smatch matchPercent;
				if (std::regex_match(valueStr, matchPercent, rePercent))
				{
					int v = 0;
					if (!ParseInt(matchPercent[1].str(), v) || v < 0 || v > 100)
					{
						result.isValid = false;
						result.errorMessage = "Value must be 0-100";
					}
					return result;
				}

				if (std::regex_match(valueStr, reFloat))
				{
					double v = 0.0;
					if (!ParseDouble(valueStr, v) || v < 0.0 || v > 1.0)
					{
						result.isValid = false;
						result.errorMessage = "Value must be 0.0-1.0";
					}
					return result;
				}

				result.isValid = false;
				result.errorMessage = "Valid formats: 1/[int], [int]%, [float] (0.0-1.0)";
				return result;
			}

			case Type::kFreq:
			{
				// 形式: [int]Hz, [float]kHz
				static const std::regex reHz(R"(^(\d+)Hz$)");
				static const std::regex reKHz(R"(^(-?\d+\.?\d*|\d*\.?\d+)kHz$)");

				std::smatch matchHz;
				if (std::regex_match(valueStr, matchHz, reHz))
				{
					int v = 0;
					if (!ParseInt(matchHz[1].str(), v) || v < 10 || v > 20000)
					{
						result.isValid = false;
						result.errorMessage = "Value must be 10-20000Hz";
					}
					return result;
				}

				std::smatch matchKHz;
				if (std::regex_match(valueStr, matchKHz, reKHz))
				{
					double v = 0.0;
					if (!ParseDouble(matchKHz[1].str(), v) || v < 0.01 || v > 20.0)
					{
						result.isValid = false;
						result.errorMessage = "Value must be 0.01-20.0kHz";
					}
					return result;
				}

				result.isValid = false;
				result.errorMessage = "Valid formats: [int]Hz, [float]kHz";
				return result;
			}

			case Type::kPitch:
			{
				// 形式: [float] (-48.0〜48.0)
				static const std::regex re(R"(^(-?\d+\.?\d*|\d*\.?\d+)$)");
				if (!std::regex_match(valueStr, re))
				{
					result.isValid = false;
					result.errorMessage = "Valid format: [float]";
					return result;
				}

				double v = 0.0;
				if (!ParseDouble(valueStr, v) || v < -48.0 || v > 48.0)
				{
					result.isValid = false;
					result.errorMessage = "Value must be -48.0 to 48.0";
				}
				return result;
			}

			case Type::kInt:
			{
				// 形式: [int]
				static const std::regex re(R"(^-?\d+$)");
				int v = 0;
				if (!std::regex_match(valueStr, re) || !ParseInt(valueStr, v))
				{
					result.isValid = false;
					result.errorMessage = "Valid format: [int]";
				}
				return result;
			}

			case Type::kFloat:
			{
				// 形式: [float]
				static const std::regex re(R"(^(-?\d+\.?\d*|\d*\.?\d+)$)");
				double v = 0.0;
				if (!std::regex_match(valueStr, re) || !ParseDouble(valueStr, v))
				{
					result.isValid = false;
					result.errorMessage = "Valid format: [float]";
				}
				return result;
			}

			case Type::kDB:
			{
				// 形式: [float]dB
				static const std::regex re(R"(^(-?\d+\.?\d*|\d*\.?\d+)dB$)");
				std::smatch match;
				double v = 0.0;
				if (!std::regex_match(valueStr, match, re) || !ParseDouble(match[1].str(), v))
				{
					result.isValid = false;
					result.errorMessage = "Valid format: [float]dB";
				}
				return result;
			}

			case Type::kFilename:
			{
				// 空でない文字列
				if (value.empty())
				{
					result.isValid = false;
					result.errorMessage = "Filename required";
				}
				return result;
			}

			case Type::kUnspecified:
				break;
			}

			return result;
		}
	}

	ValidationResult ValidateParamValue(Type type, std::string_view value)
	{
		// 空値は許可
		if (value.empty())
		{
			return {};
		}

		// Off>OnMin-OnMax形式をパース
		std::string_view off, onMin, onMax;

		const auto gtPos = value.find('>');
		const std::size_t searchStart = ((gtPos == std::string_view::npos) ? 0 : gtPos + 1/*'>'*/) + 1/*'-'*/;
		const auto dashPos = value.find('-', searchStart);

		if (gtPos != std::string_view::npos && dashPos != std::string_view::npos && dashPos > gtPos)
		{
			off = value.substr(0, gtPos);
			onMin = value.substr(gtPos + 1, dashPos - gtPos - 1);
			onMax = value.substr(dashPos + 1);
		}
		else if (gtPos != std::string_view::npos)
		{
			off = value.substr(0, gtPos);
			onMin = value.substr(gtPos + 1);
			onMax = onMin;
		}
		else if (dashPos != std::string_view::npos)
		{
			// OnMin-OnMax
			off = value.substr(0, dashPos);
			onMin = off;
			onMax = value.substr(dashPos + 1);
		}
		else
		{
			off = value;
			onMin = value;
			onMax = value;
		}

		// 空文字列チェック
		ValidationResult result;

		if (off.empty())
		{
			result.isValid = false;
			result.errorMessage = "Off value cannot be empty";
			return result;
		}

		if (onMin.empty())
		{
			result.isValid = false;
			result.errorMessage = "OnMin value cannot be empty";
			return result;
		}

		if (onMax.empty())
		{
			result.isValid = false;
			result.errorMessage = "OnMax value cannot be empty";
			return result;
		}

		// 各値をバリデーション
		result = ValidateSingleValue(type, off);
		if (!result.isValid)
		{
			result.errorMessage = "Off value: " + result.errorMessage;
			return result;
		}

		if (onMin != off)
		{
			result = ValidateSingleValue(type, onMin);
			if (!result.isValid)
			{
				result.errorMessage = "OnMin value: " + result.errorMessage;
				return result;
			}
		}

		if (onMax != onMin && onMax != off)
		{
			result = ValidateSingleValue(type, onMax);
			if (!result.isValid)
			{
				result.errorMessage = "OnMax value: " + result.errorMessage;
				return result;
			}
		}

		return result;
	}

	ValidationResult ValidateParamValue(
		std::string_view effectType,
		std::string_view paramName,
		std::string_view value)
	{
		const Type type = GetParamType(effectType, paramName);
		if (type == Type::kUnspecified)
		{
			ValidationResult result;
			result.isValid = false;
			result.errorMessage = "Unknown parameter";
			return result;
		}
		return ValidateParamValue(type, value);
	}

	std::string_view GetTypeName(Type type)
	{
		switch (type)
		{
		case Type::kLength:
		case Type::kWaveLength:
			return "length";
		case Type::kSample:
			return "sample";
		case Type::kSwitch:
			return "switch";
		case Type::kRate:
			return "rate";
		case Type::kFreq:
			return "freq";
		case Type::kPitch:
			return "pitch";
		case Type::kInt:
			return "int";
		case Type::kFloat:
			return "float";
		case Type::kDB:
			return "dB";
		case Type::kFilename:
			return "filename";
		case Type::kUnspecified:
			return "unspecified";
		}
		return "";
	}
}

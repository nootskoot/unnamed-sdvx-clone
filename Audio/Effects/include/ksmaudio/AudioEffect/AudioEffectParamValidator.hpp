#pragma once
#include <string>
#include <string_view>
#include "AudioEffectParam.hpp"

namespace ksmaudio::AudioEffect
{
	struct ValidationResult
	{
		bool isValid = true;
		std::string errorMessage;
	};

	// パラメータ値をバリデーション(Off>OnMin-OnMax形式対応)
	[[nodiscard]]
	ValidationResult ValidateParamValue(Type type, std::string_view value);

	// エフェクトタイプとパラメータ名を指定してバリデーション
	[[nodiscard]]
	ValidationResult ValidateParamValue(
		std::string_view effectType,
		std::string_view paramName,
		std::string_view value);

	// 型の表示名を取得
	[[nodiscard]]
	std::string_view GetTypeName(Type type);
}

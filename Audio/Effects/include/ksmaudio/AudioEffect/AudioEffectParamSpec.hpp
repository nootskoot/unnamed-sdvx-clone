#pragma once
#include <string_view>
#include <vector>
#include "AudioEffectParam.hpp"

namespace ksmaudio::AudioEffect
{
	struct ParamSpec
	{
		ParamID id;
		std::string_view name;
		Type type;
		std::string_view defaultValue;
	};

	// パラメータ定義を取得(見つからない場合はnullptr)
	[[nodiscard]]
	const std::vector<ParamSpec>* GetParamSpecs(std::string_view effectType);

	// 全エフェクトタイプ名のリストを取得
	[[nodiscard]]
	const std::vector<std::string_view>& GetEffectTypeNames();

	// パラメータがエフェクトタイプで有効かどうかを判定
	[[nodiscard]]
	bool IsValidParam(std::string_view effectType, std::string_view paramName);

	// パラメータの型を取得(見つからない場合はType::kUnspecified)
	[[nodiscard]]
	Type GetParamType(std::string_view effectType, std::string_view paramName);

	// パラメータ定義を取得(見つからない場合はnullptr)
	[[nodiscard]]
	const ParamSpec* GetParamSpec(std::string_view effectType, std::string_view paramName);
}

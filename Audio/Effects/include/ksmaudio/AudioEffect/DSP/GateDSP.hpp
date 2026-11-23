#pragma once
#include "ksmaudio/AudioEffect/AudioEffect.hpp"
#include "ksmaudio/AudioEffect/Params/GateParams.hpp"
#include "ksmaudio/AudioEffect/detail/DSPSimpleTriggerHandler.hpp"

namespace ksmaudio::AudioEffect
{
	class GateDSP
	{
	private:
		const DSPCommonInfo m_info;
		detail::DSPSimpleTriggerHandler m_triggerHandler;

	public:
		explicit GateDSP(const DSPCommonInfo& info);

		void process(float* pData, std::size_t dataSize, bool bypass, const GateDSPParams& params);

		void updateParams(const GateDSPParams& params);

		void reset();
	};
}

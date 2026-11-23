#pragma once
#include "ksmaudio/AudioEffect/AudioEffect.hpp"
#include "ksmaudio/AudioEffect/Params/RetriggerEchoParams.hpp"
#include "ksmaudio/AudioEffect/detail/LinearBuffer.hpp"

namespace ksmaudio::AudioEffect
{
	class RetriggerEchoDSP
	{
	private:
		const DSPCommonInfo m_info;
		detail::LinearBuffer<float> m_linearBuffer;
		std::ptrdiff_t m_framesUntilTrigger = -1;

	public:
		explicit RetriggerEchoDSP(const DSPCommonInfo& info);

		void process(float* pData, std::size_t dataSize, bool bypass, const RetriggerEchoDSPParams& params);

		void updateParams(const RetriggerEchoDSPParams& params);

		void reset();
	};
}

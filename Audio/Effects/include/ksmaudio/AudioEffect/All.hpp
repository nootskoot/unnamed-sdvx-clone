#pragma once
#include "AudioEffect.hpp"

#include "DSP/RetriggerEchoDSP.hpp"
#include "Params/RetriggerEchoParams.hpp"

#include "DSP/GateDSP.hpp"
#include "Params/GateParams.hpp"

#include "DSP/FlangerDSP.hpp"
#include "Params/FlangerParams.hpp"

#include "DSP/BitcrusherDSP.hpp"
#include "Params/BitcrusherParams.hpp"

#include "DSP/PhaserDSP.hpp"
#include "Params/PhaserParams.hpp"

#include "DSP/PitchShiftDSP.hpp"
#include "Params/PitchShiftParams.hpp"

#include "DSP/WobbleDSP.hpp"
#include "Params/WobbleParams.hpp"

#include "DSP/TapestopDSP.hpp"
#include "Params/TapestopParams.hpp"

#include "DSP/SidechainDSP.hpp"
#include "Params/SidechainParams.hpp"

#include "DSP/PeakingFilterDSP.hpp"
#include "Params/PeakingFilterParams.hpp"

#include "DSP/HighPassFilterDSP.hpp"
#include "Params/HighPassFilterParams.hpp"

#include "DSP/LowPassFilterDSP.hpp"
#include "Params/LowPassFilterParams.hpp"

namespace ksmaudio
{
	// priorityはHSP版と同一に設定
	// HSP版: https://github.com/kshootmania/ksm-v1/blob/ea05374a3ece796612b29d927cb3c6f5aabb266e/src/audio/fxdef.hsp
	// 別途定義されているコンプレッサーの優先度(kCompressorFXPriority)を下回らないよう注意

	using Retrigger = AudioEffect::BasicAudioEffectWithTrigger<AudioEffect::RetriggerParams, AudioEffect::RetriggerEchoDSP, AudioEffect::RetriggerEchoDSPParams, 25>;

	using Gate = AudioEffect::BasicAudioEffectWithTrigger<AudioEffect::GateParams, AudioEffect::GateDSP, AudioEffect::GateDSPParams, 17>;

	using Flanger = AudioEffect::BasicAudioEffect<AudioEffect::FlangerParams, AudioEffect::FlangerDSP, AudioEffect::FlangerDSPParams, 15>;

	using Bitcrusher = AudioEffect::BasicAudioEffect<AudioEffect::BitcrusherParams, AudioEffect::BitcrusherDSP, AudioEffect::BitcrusherDSPParams, 20>;

	using Phaser = AudioEffect::BasicAudioEffect<AudioEffect::PhaserParams, AudioEffect::PhaserDSP, AudioEffect::PhaserDSPParams, 16>;

	using PitchShift = AudioEffect::BasicAudioEffect<AudioEffect::PitchShiftParams, AudioEffect::PitchShiftDSP, AudioEffect::PitchShiftDSPParams, 20>;

	using Wobble = AudioEffect::BasicAudioEffectWithTrigger<AudioEffect::WobbleParams, AudioEffect::WobbleDSP, AudioEffect::WobbleDSPParams, 17>;

	using Tapestop = AudioEffect::BasicAudioEffect<AudioEffect::TapestopParams, AudioEffect::TapestopDSP, AudioEffect::TapestopDSPParams, 18>;

	using Echo = AudioEffect::BasicAudioEffectWithTrigger<AudioEffect::EchoParams, AudioEffect::RetriggerEchoDSP, AudioEffect::RetriggerEchoDSPParams, 26>;

	using Sidechain = AudioEffect::BasicAudioEffectWithTrigger<AudioEffect::SidechainParams, AudioEffect::SidechainDSP, AudioEffect::SidechainDSPParams, 14>;

	using PeakingFilter = AudioEffect::BasicAudioEffect<AudioEffect::PeakingFilterParams, AudioEffect::PeakingFilterDSP, AudioEffect::PeakingFilterDSPParams, 1>;

	using HighPassFilter = AudioEffect::BasicAudioEffect<AudioEffect::HighPassFilterParams, AudioEffect::HighPassFilterDSP, AudioEffect::HighPassFilterDSPParams, 2>;

	using LowPassFilter = AudioEffect::BasicAudioEffect<AudioEffect::LowPassFilterParams, AudioEffect::LowPassFilterDSP, AudioEffect::LowPassFilterDSPParams, 3>;
}

#include "stdafx.h"
#include "AudioPlayback.hpp"
#include <Beatmap/BeatmapPlayback.hpp>
#include <Audio/DSP.hpp>
#include <Audio/Audio.hpp>
#include <Beatmap/KSHFilter.hpp>

namespace {
bool ConfigureNativeFilter(DSP* dsp, const AudioEffect& effect, float input, uint32 sampleRate) {
    if(!effect.nativeFilter) return false;
    const float v=std::clamp(effect.filterPosition.Sample(input),0.f,1.f);
    const float first=effect.filterFreq.Sample(input), last=effect.filterFreqMax.Sample(input);
    float curve=v;
    if(effect.type==EffectType::PeakingFilter)curve=(KSHFilter::Peak(v,0.5f,sampleRate).frequency-50)/7850;
    else if(effect.type==EffectType::HighPassFilter)curve=(KSHFilter::HighFrequency(v)-100)/2100;
    else if(effect.type==EffectType::LowPassFilter)curve=(15000-KSHFilter::LowFrequency(v))/14200;
    const float frequency=std::clamp(first+(last-first)*std::clamp(curve,0.f,1.f),1.f,sampleRate*.49f);
    auto* filter=static_cast<BQFDSP*>(dsp);
    if(effect.type==EffectType::PeakingFilter) {
        const double w=6.283185307179586*frequency/sampleRate;
        const double bandwidth=std::clamp(effect.filterBandwidth.Sample(input),.1f,10.f);
        const float q=float(1/(2*std::sinh(std::log(2.0)*bandwidth*.5*w/std::sin(w))));
        filter->SetPeaking(q,frequency,KSHFilter::Peak(v,effect.filterGain.Sample(input),sampleRate).gain);
    } else if(effect.type==EffectType::HighPassFilter) filter->SetHighPass(effect.hpf.q.Sample(input),frequency);
    else filter->SetLowPass(effect.lpf.q.Sample(input),frequency);
    return true;
}
}

DSP *GameAudioEffect::CreateDSP(const TimingPoint &tp, float filterInput, uint32 sampleRate, float playbackSpeed)
{
	DSP *ret = nullptr;

	double noteDuration = tp.GetWholeNoteLength();
	if (playbackSpeed > 0) noteDuration /= playbackSpeed;

	const uint32 actualLength = duration.Sample(filterInput).Absolute(noteDuration);
	const uint32 maxLength = Math::Max(duration.Sample(0.f).Absolute(noteDuration), duration.Sample(1.f).Absolute(noteDuration));

	switch (type)
	{
	case EffectType::Bitcrush:
	{
		BitCrusherDSP *bcDSP = new BitCrusherDSP(sampleRate);
		bcDSP->SetPeriod((float)bitcrusher.reduction.Sample(filterInput));
		ret = bcDSP;
		break;
	}
	case EffectType::Echo:
	{
		EchoDSP *echoDSP = new EchoDSP(sampleRate);
		echoDSP->feedback = echo.feedback.Sample(filterInput);
		echoDSP->SetLength(actualLength);
		ret = echoDSP;
		break;
	}
	case EffectType::PeakingFilter:
	case EffectType::LowPassFilter:
	case EffectType::HighPassFilter:
	{
		// Initialize coefficients before attaching the DSP (also used by FX/named filters).
		BQFDSP *bqfDSP = new BQFDSP(sampleRate);
        if (ConfigureNativeFilter(bqfDSP, *this, filterInput, sampleRate)) {}
        else if (type == EffectType::PeakingFilter)
            bqfDSP->SetPeaking(peaking.q.Sample(filterInput), peaking.freq.Sample(filterInput), peaking.gain.Sample(filterInput));
        else if (type == EffectType::LowPassFilter)
            bqfDSP->SetLowPass(lpf.q.Sample(filterInput), lpf.freq.Sample(filterInput));
        else bqfDSP->SetHighPass(hpf.q.Sample(filterInput), hpf.freq.Sample(filterInput));
		ret = bqfDSP;
		break;
	}
	case EffectType::Gate:
	{
		GateDSP *gateDSP = new GateDSP(sampleRate);
		gateDSP->SetLength(actualLength);
		gateDSP->SetGating(gate.gate.Sample(filterInput));
		ret = gateDSP;
		break;
	}
	case EffectType::TapeStop:
	{
		TapeStopDSP *tapestopDSP = new TapeStopDSP(sampleRate);
		tapestopDSP->SetLength(actualLength);
		ret = tapestopDSP;
		break;
	}
	case EffectType::Retrigger:
	{
		RetriggerDSP *retriggerDSP = new RetriggerDSP(sampleRate);
		retriggerDSP->SetMaxLength(maxLength);
		retriggerDSP->SetLength(actualLength);
		retriggerDSP->SetGating(retrigger.gate.Sample(filterInput));
		retriggerDSP->SetResetDuration(retrigger.reset.Sample(filterInput).Absolute(noteDuration));
		ret = retriggerDSP;
		break;
	}
	case EffectType::Wobble:
	{
		WobbleDSP *wb = new WobbleDSP(sampleRate);
		wb->SetLength(actualLength);
		wb->q = wobble.q.Sample(filterInput);
		wb->fmax = wobble.max.Sample(filterInput);
		wb->fmin = wobble.min.Sample(filterInput);
		ret = wb;
		break;
	}
	case EffectType::Phaser:
	{
		PhaserDSP *phs = new PhaserDSP(sampleRate);
		phs->SetLength(actualLength);
		phs->SetStage(phaser.stage.Sample(filterInput));
		phs->fmin = phaser.min.Sample(filterInput);
		phs->fmax = phaser.max.Sample(filterInput);
		phs->q = phaser.q.Sample(filterInput);
		phs->feedback = phaser.feedback.Sample(filterInput);
		phs->stereoWidth = phaser.stereoWidth.Sample(filterInput);
		phs->hiCutGain = phaser.hiCutGain.Sample(filterInput);
		ret = phs;
		break;
	}
	case EffectType::Flanger:
	{
		FlangerDSP *fl = new FlangerDSP(sampleRate);
		fl->SetLength(actualLength);
		fl->SetDelayRange(abs(flanger.offset.Sample(filterInput)),
						  abs(flanger.depth.Sample(filterInput)));
		fl->SetFeedback(flanger.feedback.Sample(filterInput));
		fl->SetStereoWidth(flanger.stereoWidth.Sample(filterInput));
		fl->SetVolume(flanger.volume.Sample(filterInput));
		ret = fl;
		break;
	}
	case EffectType::SideChain:
	{
		SidechainDSP *sc = new SidechainDSP(sampleRate);
		sc->SetLength(actualLength);
		sc->SetAttackTime(sidechain.attackTime.Sample(filterInput).Absolute(noteDuration));
		sc->SetHoldTime(sidechain.holdTime.Sample(filterInput).Absolute(noteDuration));
		sc->SetReleaseTime(sidechain.releaseTime.Sample(filterInput).Absolute(noteDuration));
		sc->ratio = sidechain.ratio.Sample(filterInput);
		ret = sc;
		break;
	}
    case EffectType::Panning:
    {
        auto* pan = new PanDSP();
        pan->panning = panning.panning.Sample(filterInput);
        ret = pan;
        break;
    }
	case EffectType::PitchShift:
	{
		PitchShiftDSP *ps = new PitchShiftDSP(sampleRate);
		ps->amount = pitchshift.amount.Sample(filterInput);
        ps->chunkSize = pitchChunk.Sample(filterInput);
        ps->overlap = pitchOverlap.Sample(filterInput);
		ret = ps;
		break;
	}
	default:
		break;
	}

	if (!ret)
	{
		Logf("Failed to create game audio effect for type \"%s\"", Logger::Severity::Warning, Enum_EffectType::ToString(type));
		return nullptr;
	}

	ret->priority = AudioEffect::GetDefaultEffectPriority(type);
	ret->mix = mix.Sample(filterInput);
	return ret;
}
bool GameAudioEffect::UpdateDSP(DSP* dsp, const TimingPoint &tp, float filterInput, uint32 sampleRate, float playbackSpeed)
{
	DSP *ret = nullptr;
    if (!dsp) return false;

	double noteDuration = tp.GetWholeNoteLength();
	if (playbackSpeed > 0) noteDuration /= playbackSpeed;

	const uint32 actualLength = duration.Sample(filterInput).Absolute(noteDuration);
	const uint32 maxLength = Math::Max(duration.Sample(0.f).Absolute(noteDuration), duration.Sample(1.f).Absolute(noteDuration));

	switch (type)
	{
	case EffectType::Bitcrush:
	{
		BitCrusherDSP *bcDSP = static_cast<BitCrusherDSP*>(dsp);
		bcDSP->SetPeriod((float)bitcrusher.reduction.Sample(filterInput));
		ret = bcDSP;
		break;
	}
	case EffectType::Echo:
	{
		EchoDSP *echoDSP = static_cast<EchoDSP*>(dsp);
		echoDSP->feedback = echo.feedback.Sample(filterInput);
		echoDSP->SetLength(actualLength);
		ret = echoDSP;
		break;
	}
	case EffectType::PeakingFilter:
	case EffectType::LowPassFilter:
	case EffectType::HighPassFilter:
	{
		// Initialize coefficients before attaching the DSP (also used by FX/named filters).
		BQFDSP *bqfDSP = static_cast<BQFDSP*>(dsp);
        if (ConfigureNativeFilter(bqfDSP, *this, filterInput, sampleRate)) {}
        else if (type == EffectType::PeakingFilter)
            bqfDSP->SetPeaking(peaking.q.Sample(filterInput), peaking.freq.Sample(filterInput), peaking.gain.Sample(filterInput));
        else if (type == EffectType::LowPassFilter)
            bqfDSP->SetLowPass(lpf.q.Sample(filterInput), lpf.freq.Sample(filterInput));
        else bqfDSP->SetHighPass(hpf.q.Sample(filterInput), hpf.freq.Sample(filterInput));
		ret = bqfDSP;
		break;
	}
	case EffectType::Gate:
	{
		GateDSP *gateDSP = static_cast<GateDSP*>(dsp);
		gateDSP->SetLength(actualLength);
		gateDSP->SetGating(gate.gate.Sample(filterInput));
		ret = gateDSP;
		break;
	}
	case EffectType::TapeStop:
	{
		TapeStopDSP *tapestopDSP = static_cast<TapeStopDSP*>(dsp);
		tapestopDSP->SetLength(actualLength);
		ret = tapestopDSP;
		break;
	}
	case EffectType::Retrigger:
	{
		RetriggerDSP *retriggerDSP = static_cast<RetriggerDSP*>(dsp);
		retriggerDSP->SetMaxLength(maxLength);
		retriggerDSP->SetLength(actualLength);
		retriggerDSP->SetGating(retrigger.gate.Sample(filterInput));
		retriggerDSP->SetResetDuration(retrigger.reset.Sample(filterInput).Absolute(noteDuration));
		ret = retriggerDSP;
		break;
	}
	case EffectType::Wobble:
	{
		WobbleDSP *wb = static_cast<WobbleDSP*>(dsp);
		wb->SetLength(actualLength);
		wb->q = wobble.q.Sample(filterInput);
		wb->fmax = wobble.max.Sample(filterInput);
		wb->fmin = wobble.min.Sample(filterInput);
		ret = wb;
		break;
	}
	case EffectType::Phaser:
	{
		PhaserDSP *phs = static_cast<PhaserDSP*>(dsp);
		phs->SetLength(actualLength);
		phs->SetStage(phaser.stage.Sample(filterInput));
		phs->fmin = phaser.min.Sample(filterInput);
		phs->fmax = phaser.max.Sample(filterInput);
		phs->q = phaser.q.Sample(filterInput);
		phs->feedback = phaser.feedback.Sample(filterInput);
		phs->stereoWidth = phaser.stereoWidth.Sample(filterInput);
		phs->hiCutGain = phaser.hiCutGain.Sample(filterInput);
		ret = phs;
		break;
	}
	case EffectType::Flanger:
	{
		FlangerDSP *fl = static_cast<FlangerDSP*>(dsp);
		fl->SetLength(actualLength);
		fl->SetDelayRange(abs(flanger.offset.Sample(filterInput)),
						  abs(flanger.depth.Sample(filterInput)));
		fl->SetFeedback(flanger.feedback.Sample(filterInput));
		fl->SetStereoWidth(flanger.stereoWidth.Sample(filterInput));
		fl->SetVolume(flanger.volume.Sample(filterInput));
		ret = fl;
		break;
	}
	case EffectType::SideChain:
	{
		SidechainDSP *sc = static_cast<SidechainDSP*>(dsp);
		sc->SetLength(actualLength);
		sc->SetAttackTime(sidechain.attackTime.Sample(filterInput).Absolute(noteDuration));
		sc->SetHoldTime(sidechain.holdTime.Sample(filterInput).Absolute(noteDuration));
		sc->SetReleaseTime(sidechain.releaseTime.Sample(filterInput).Absolute(noteDuration));
		sc->ratio = sidechain.ratio.Sample(filterInput);
		ret = sc;
		break;
	}
    case EffectType::Panning:
    {
        auto* pan = static_cast<PanDSP*>(dsp);
        pan->panning = panning.panning.Sample(filterInput);
        ret = pan;
        break;
    }
	case EffectType::PitchShift:
	{
		PitchShiftDSP *ps = static_cast<PitchShiftDSP*>(dsp);
		ps->amount = pitchshift.amount.Sample(filterInput);
        ps->chunkSize = pitchChunk.Sample(filterInput);
        ps->overlap = pitchOverlap.Sample(filterInput);
		ret = ps;
		break;
	}
	default:
		break;
	}

	if (!ret)
	{
		Logf("Failed to create game audio effect for type \"%s\"", Logger::Severity::Warning, Enum_EffectType::ToString(type));
		return false;
	}

	ret->priority = AudioEffect::GetDefaultEffectPriority(type);
	ret->mix = mix.Sample(filterInput);
	return true;
}
void GameAudioEffect::SetParams(DSP *dsp, AudioPlayback &playback, HoldObjectState *object)
{
	const TimingPoint &tp = *playback.GetBeatmapPlayback().GetTimingPointAt(object->time);
	double noteDuration = tp.GetWholeNoteLength();
    if (playback.GetPlaybackSpeed() > 0) noteDuration /= playback.GetPlaybackSpeed();

	switch (type)
	{
	case EffectType::Bitcrush:
	{
		BitCrusherDSP *bcDSP = (BitCrusherDSP *)dsp;
		if (object->effectParams[0] >= 0) bcDSP->SetPeriod((float)object->effectParams[0]);
		break;
	}
	case EffectType::Gate:
	{
		GateDSP *gateDSP = (GateDSP *)dsp;
		if (object->effectParams[0] > 0) gateDSP->SetLength(noteDuration / object->effectParams[0]);
		break;
	}
	case EffectType::TapeStop:
	{
		TapeStopDSP *tapestopDSP = (TapeStopDSP *)dsp;
		if (object->effectParams[0] > 0) tapestopDSP->SetLength((1000 * ((double)16 / object->effectParams[0])));
		break;
	}
	case EffectType::Retrigger:
	{
		RetriggerDSP *retriggerDSP = (RetriggerDSP *)dsp;
		if (object->effectParams[0] > 0) retriggerDSP->SetLength(noteDuration / object->effectParams[0]);
		break;
	}
	case EffectType::Echo:
	{
		EchoDSP *echoDSP = (EchoDSP *)dsp;
		if (object->effectParams[0] > 0) echoDSP->SetLength(noteDuration / object->effectParams[0]);
		if (object->effectParams[1] >= 0) echoDSP->feedback = object->effectParams[1] / 100.0f;
		break;
	}
	case EffectType::Wobble:
	{
		WobbleDSP *wb = (WobbleDSP *)dsp;
		if (object->effectParams[0] > 0) wb->SetLength(noteDuration / object->effectParams[0]);
		break;
	}
	case EffectType::Phaser:
	{
		PhaserDSP *phs = (PhaserDSP *)dsp;
		break;
	}
	case EffectType::Flanger:
	{
		FlangerDSP *fl = (FlangerDSP *)dsp;
		break;
	}
	case EffectType::PitchShift:
	{
		PitchShiftDSP *ps = (PitchShiftDSP *)dsp;
		if (object->effectType < EffectType::UserDefined0 || object->effectParams[0] != -1)
            ps->amount = (float)object->effectParams[0];
		break;
	}
	default:
		break;
	}
}

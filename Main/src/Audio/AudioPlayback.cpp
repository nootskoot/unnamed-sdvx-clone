#include "stdafx.h"
#include "AudioPlayback.hpp"
#include <Beatmap/BeatmapPlayback.hpp>
#include <Beatmap/Beatmap.hpp>
#include <Shared/ChartSource.hpp>
#include <Audio/Audio.hpp>
#include <Audio/DSP.hpp>
#include <Beatmap/KSHFilter.hpp>
#include <cstdlib>  // opt-in audio comparison environment switches
#include <Shared/Profiling.hpp>
// USC_AUDIO_PROBE_PATCH_20261110: default game audio is unchanged.
// Set USC_AUDIO_PROBE=1 for structured event/gain logs.
// Diagnostic-only: this probe does not alter DSP mixing or FX state.
namespace {
    bool USC_AudioEnvironmentSwitch(const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] != '\0' && value[0] != '0';
    }
    bool USC_AudioProbe() {
        static const bool enabled = USC_AudioEnvironmentSwitch("USC_AUDIO_PROBE");
        return enabled;
    }
}

AudioPlayback::AudioPlayback()
{
}
AudioPlayback::~AudioPlayback()
{
	for (auto &it : m_namedLaserFilters) m_CleanupDSP(it.second.dsp);
	m_namedLaserFilters.clear();
	m_CleanupDSP(m_buttonDSPs[0]);
	m_CleanupDSP(m_buttonDSPs[1]);
	m_CleanupDSP(m_laserDSP);
}
bool AudioPlayback::Init(class BeatmapPlayback &playback, Ref<ChartSource> chartSource, bool preRender)
{
	// Cleanup exising DSP's
	for (auto &it : m_namedLaserFilters) m_CleanupDSP(it.second.dsp);
	m_namedLaserFilters.clear();
    m_fxOverrides.clear(); m_filterOverrides.clear(); m_inactiveOverrides.clear(); m_filterCaptureTimes.clear();
    m_automationCursor = 0;
    m_automationTimingTime = std::numeric_limits<MapTime>::min();
    m_lastCaptureTime = std::numeric_limits<MapTime>::min();
    m_audioProbeLastLaserTime = std::numeric_limits<MapTime>::min();
    m_effectEnabled[0] = m_effectEnabled[1] = false;
    m_lastEnabledFXLane = 0;
    m_effectMix[0] = m_effectMix[1] = 0.0f;
    m_automationTime = std::numeric_limits<MapTime>::min();
	m_currentHoldEffects[0] = nullptr;
	m_currentHoldEffects[1] = nullptr;
	m_CleanupDSP(m_buttonDSPs[0]);
	m_CleanupDSP(m_buttonDSPs[1]);
	m_CleanupDSP(m_laserDSP);

    m_fxtrack.reset(); m_legacyP.reset(); m_legacyFP.reset();
    m_singleF=false; m_fxtrackEnabled=true;
    m_legacyFXActive=m_legacyLaserActive=false;
	m_playback = &playback;
	m_beatmap = &playback.GetBeatmap();
    m_laserAudioSampler.Build(*m_beatmap);
	m_beatmapRootPath = "";
	m_chartSource = std::move(chartSource);
	assert(m_beatmap != nullptr);

	// Set default effect type
	SetLaserEffect(EffectType::PeakingFilter);

	const BeatmapSettings &mapSettings = m_beatmap->GetMapSettings();
	String audioName = mapSettings.audioNoFX;
	audioName.TrimBack(' ');
	Resource audio = m_chartSource->ResolvePath(audioName);
	if (!audio.IsValid())
	{
		Logf("Audio file for beatmap does not exist: \"%s\"", Logger::Severity::Error, audioName);
		return false;
	}
	m_music = g_audio->CreateStream(audio, true);
	if (!m_music)
	{
		Logf("Failed to load any audio for beatmap \"%s\"", Logger::Severity::Error, audioName);
		return false;
	}

	m_musicVolume = mapSettings.musicVolume;
	m_music->SetVolume(m_musicVolume);

	// Load FX track
	audioName = mapSettings.audioFX;
	audioName.TrimBack(' ');
	if (!audioName.empty())
	{
		audio = m_chartSource->ResolvePath(audioName);
		if (!audio.IsValid() || (audio.IsPath() && Path::IsDirectory(audio.GetPath())))
		{
			Logf("FX audio for beatmap does not exist: \"%s\" Using real-time effects instead.", Logger::Severity::Warning, audioName);
		}
		else
		{
			m_fxtrack = g_audio->CreateStream(audio, true);
			if (m_fxtrack)
			{
				// Initially mute normal track if fx is enabled
				m_music->SetVolume(0.0f);
			}
		}
	}

    if (m_fxtrack && !mapSettings.audioLaserFX.empty() && !mapSettings.audioCombinedFX.empty()) {
        auto loadStem=[&](String name) -> Ref<AudioStream> {
            name.TrimBack(' ');
            const auto resource=m_chartSource->ResolvePath(name);
            if (!resource.IsValid() || (resource.IsPath() && Path::IsDirectory(resource.GetPath()))) return {};
            auto stream=g_audio->CreateStream(resource,true);
            if (stream) stream->SetVolume(0.f);
            return stream;
        };
        m_legacyP=loadStem(mapSettings.audioLaserFX);
        m_legacyFP=loadStem(mapSettings.audioCombinedFX);
        if (!m_legacyP || !m_legacyFP) {
            Logf("Incomplete legacy P/FP stems; using the existing single FX-track fallback.",Logger::Severity::Warning);
            m_legacyP.reset();m_legacyFP.reset();
        } else SetFXTrackState(false,false);
    }
	if (m_fxtrack)
	{
		m_singleF = !m_legacyP || !m_legacyFP;
        SetFXTrackState(false,false);
        if (USC_AudioProbe())
            Logf("[AUDIO-PROBE] ROUTE external_fxtrack=1 pfp=%d single_f=%d prerender_requested=%d",
                Logger::Severity::Warning, int(bool(m_legacyP && m_legacyFP)),
                int(m_singleF), int(preRender));
        // Prevent loading switchables if fx track is in use.
		return true;
	}

	const bool hasNamedFilters = std::any_of(m_beatmap->GetObjectStates().begin(), m_beatmap->GetObjectStates().end(), [](const auto& o) {
        if (o->type != ObjectType::Event) return false;
        auto key = reinterpret_cast<const EventObjectState*>(o.get())->key;
        return key == EventKey::NamedLaserFilterMix || key == EventKey::NamedLaserFilterRefresh;
    });
    if (preRender && !hasNamedFilters && m_beatmap->GetAudioEffectChanges().empty())
	{
		m_fxtrack = AudioStream::Clone(g_audio, m_music);
		assert(m_fxtrack);
        if (USC_AudioProbe())
            Logf("[AUDIO-PROBE] ROUTE prerender_fx=1; real-time FX hold events bypassed",
                Logger::Severity::Warning);
		m_PreRenderDSPTrack();
		return true;
	}


	auto switchablePaths = m_beatmap->GetSwitchablePaths();

	// Load switchable audio tracks
	for (auto it = switchablePaths.begin(); it != switchablePaths.end(); ++it)
	{
		audioName = *it;
		audioName.TrimBack(' ');

		SwitchableAudio switchable;
		switchable.m_enabled = false;
		if (!audioName.empty())
		{
			audio = m_chartSource->ResolvePath(audioName);
			if (!audio.IsValid())
			{
				Logf("Audio for a SwitchAudio effect does not exist: \"%s\"", Logger::Severity::Warning, audioName);
			}
			else
			{
				switchable.m_audio = g_audio->CreateStream(audio, true);
				if (switchable.m_audio)
				{
					// Mute all switchable audio by default
					switchable.m_audio->SetVolume(0.0f);
				}
			}
		}
		m_switchables.Add(switchable);
	}


    if (USC_AudioProbe())
        Logf("[AUDIO-PROBE] ROUTE realtime_fx=1 prerender_requested=%d named_filters=%d automation=%d",
            Logger::Severity::Warning, int(preRender), int(hasNamedFilters),
            int(!m_beatmap->GetAudioEffectChanges().empty()));
	return true;
}
void AudioPlayback::Tick(float deltaTime)
{
    if (m_playback && m_beatmap) {
        const auto time = m_playback->GetLastTime();
        if (time < m_lastCaptureTime) {
            m_filterCaptureTimes.clear();
            std::map<EffectType, float> mixes;
            for (const auto& o : m_beatmap->GetObjectStates()) {
                if (o->time > time) break;
                if (o->type != ObjectType::Event) continue;
                const auto* e = reinterpret_cast<const EventObjectState*>(o.get());
                if (e->key == EventKey::NamedLaserFilterRefresh && e->data.uintVal)
                    m_filterCaptureTimes[EffectType(uint16(e->data.uintVal))] = e->time;
                if (e->key == EventKey::NamedLaserFilterMix)
                    mixes[EffectType(uint16(e->data.uintVal >> 16))] = (e->data.uintVal & 0xffff) / 1000.f;
            }
            m_CleanupDSP(m_laserDSP);
            for (auto& entry : m_namedLaserFilters) m_CleanupDSP(entry.second.dsp);
            m_namedLaserFilters.clear();
            for (const auto& entry : mixes) SetNamedLaserFilterMix(entry.first, entry.second);
            for (const auto& entry : m_filterCaptureTimes)
                if (!mixes.count(entry.first) && m_ResolveInactiveEffect(entry.first).type == EffectType::Retrigger)
                    SetNamedLaserFilterMix(entry.first, 0.f);
        }
        m_lastCaptureTime = time;
    }
    if (m_playback && m_beatmap && (!m_fxtrack || m_singleF)) m_UpdateAutomation(m_playback->GetLastTime());
}
AudioEffect AudioPlayback::m_ResolveEffect(EffectType type, bool filter) const
{
    const auto& overrides = filter ? m_filterOverrides : m_fxOverrides;
    const auto it = overrides.find(type);
    AudioEffect effect = it != overrides.end() ? it->second : (filter ? m_beatmap->GetFilter(type) : m_beatmap->GetEffect(type));
    if (m_beatmap->GetMapSettings().kshBehaviorVersion >= 0 && effect.type == EffectType::PitchShift && effect.pitchChunk.Sample() == 0)
        effect.pitchChunk = 700;
    return effect;
}
AudioEffect AudioPlayback::m_ResolveInactiveEffect(EffectType type) const {
    auto it=m_inactiveOverrides.find(type);
    if(it!=m_inactiveOverrides.end())return it->second;
    if(auto* effect=m_beatmap->GetInactiveFilter(type))return *effect;
    return m_ResolveEffect(type,true);
}
void AudioPlayback::m_ConfigureGateClock(DSP* dsp, const AudioEffect& effect) {
    if (dsp && effect.type == EffectType::Gate && m_beatmap->GetKSONScroll())
        static_cast<GateDSP*>(dsp)->SetMeasureTimes(m_beatmap->GetKSONBarTimes(), m_playback->audioOffset);
}
void AudioPlayback::m_UpdateAutomation(MapTime time)
{
    const auto& changes = m_beatmap->GetAudioEffectChanges();
    if (changes.empty()) return;
    bool changed = time < m_automationTime;
    const auto timingTime = m_playback->GetCurrentTimingPoint().time;
    const bool timingChanged = timingTime != m_automationTimingTime;
    m_automationTimingTime = timingTime;
    if (changed) { m_automationCursor = 0; m_fxOverrides.clear(); m_filterOverrides.clear(); m_inactiveOverrides.clear(); }
    m_automationTime = time;
    while (m_automationCursor < changes.size() && changes[m_automationCursor].time <= time) {
        const auto& change = changes[m_automationCursor++];
        (change.inactive ? m_inactiveOverrides : change.filter ? m_filterOverrides : m_fxOverrides)[change.type] = change.effect;
        changed = true;
    }
    if (!changed && !timingChanged) return;
    const auto& tp = m_playback->GetCurrentTimingPoint();
    const auto rate = m_GetDSPTrack()->GetAudioSampleRate();
    const auto speed = GetPlaybackSpeed();
    for (int lane = 0; lane < 2; ++lane) {
        auto* hold = m_currentHoldEffects[lane];
        if (!hold) continue;
        const bool enabled = m_effectEnabled[lane];
        m_buttonEffects[lane] = m_ResolveEffect(hold->effectType, false);
        m_buttonEffects[lane].UpdateDSP(m_buttonDSPs[lane], tp, 0.0f, rate, speed);
        if (m_buttonDSPs[lane]) {
            auto params = *hold;
            if (m_fxOverrides.count(hold->effectType)) {
                if (!(hold->effectParamMask & 1)) params.effectParams[0] = -1;
                if (!(hold->effectParamMask & 2)) params.effectParams[1] = -1;
                params.effectType = EffectType::UserDefined0; // -1 means no explicit override
            }
            m_buttonEffects[lane].SetParams(m_buttonDSPs[lane], *this, &params);
        }
        SetEffectEnabled(lane, enabled);
    }
    m_laserEffect = m_ResolveEffect(m_laserEffectType, true);
    m_laserEffect.UpdateDSP(m_laserDSP, tp, GetLaserFilterInput(), rate, speed);
    if (m_laserDSP) m_SetLaserEffectParameter(GetLaserFilterInput(), m_laserDSP);
    m_MirrorLaserDSP(m_laserDSP,m_laserEffect,GetLaserFilterInput(),true);
    for (auto& entry : m_namedLaserFilters) {
        auto& state = entry.second;
        state.effect = m_ResolveInactiveEffect(EffectType(entry.first));
        state.effect.UpdateDSP(state.dsp, tp, 0.0f, rate, speed);
        if (state.dsp) state.dsp->mix = state.mix;
        m_MirrorLaserDSP(state.dsp,state.effect,0.f,false);
    }
    m_UpdateIndependentFXMix();
}
void AudioPlayback::m_UpdateIndependentFXMix() {
    for (const auto& entry:m_namedLaserFilters) {
        if(!entry.second.dsp)continue;
        // Triggered KSH FX holds use the persistent capture DSP itself. Never
        // replace it with a fresh per-hold DSP that has no pre-hold recording.
        bool persistent = false;
        int activeLane = -1;
        for (int lane=0; lane<2; ++lane)
            if (m_buttonEffects[lane].persistentFXCapture && m_buttonEffects[lane].independentFXBus==entry.first) {
                persistent = true;
                if (m_currentHoldEffects[lane] && m_effectEnabled[lane] && (activeLane < 0 || uint32(lane)==m_lastEnabledFXLane)) activeLane=lane;
            }
        if (persistent) {
            auto* dsp = entry.second.dsp;
            if (activeLane >= 0) {
                auto effect=m_buttonEffects[activeLane];
                effect.UpdateDSP(dsp, m_playback->GetCurrentTimingPoint(), 0.f,
                    m_GetDSPTrack()->GetAudioSampleRate(), GetPlaybackSpeed());
                auto params=*m_currentHoldEffects[activeLane];
                if (m_fxOverrides.count(params.effectType)) {
                    if (!(params.effectParamMask & 1)) params.effectParams[0]=-1;
                    if (!(params.effectParamMask & 2)) params.effectParams[1]=-1;
                    params.effectType=EffectType::UserDefined0;
                }
                effect.SetParams(dsp,*this,&params);
                dsp->mix=m_effectMix[activeLane];
            } else {
                GameAudioEffect effect=m_ResolveInactiveEffect(EffectType(entry.first));
                effect.UpdateDSP(dsp,m_playback->GetCurrentTimingPoint(),0.f,
                    m_GetDSPTrack()->GetAudioSampleRate(),GetPlaybackSpeed());
                dsp->mix=entry.second.mix;
            }
            continue;
        }
        bool used=false, held=false;
        for(int lane=0;lane<2;++lane)if(m_buttonEffects[lane].independentFXBus==entry.first) {
            used=true;
            held|=m_currentHoldEffects[lane] && m_effectEnabled[lane];
        }
        if(used)entry.second.dsp->mix=held?0.f:entry.second.mix;
    }
}
void AudioPlayback::Play()
{
	m_music->Play();
	if (m_fxtrack)
		m_fxtrack->Play();
    if (m_legacyP) m_legacyP->Play();
    if (m_legacyFP) m_legacyFP->Play();
	for (auto it = m_switchables.begin(); it != m_switchables.end(); ++it)
		if (it->m_audio)
			it->m_audio->Play();

	m_paused = false;
}
void AudioPlayback::Advance(MapTime ms)
{
	SetPosition(GetPosition() + ms);
}
MapTime AudioPlayback::GetPosition() const
{
	return m_music->GetPosition();
}
void AudioPlayback::SetPosition(MapTime time)
{
	m_music->SetPosition(time);
	if (m_fxtrack)
		m_fxtrack->SetPosition(time);
    if (m_legacyP) m_legacyP->SetPosition(time);
    if (m_legacyFP) m_legacyFP->SetPosition(time);
	for (auto it = m_switchables.begin(); it != m_switchables.end(); ++it)
		if (it->m_audio)
			it->m_audio->SetPosition(time);
}
void AudioPlayback::TogglePause()
{
	if (m_paused)
		Play();
	else
		Pause();
}
void AudioPlayback::Pause()
{
	if (m_paused)
		return;

	m_music->Pause();
	if (m_fxtrack)
		m_fxtrack->Pause();
    if (m_legacyP) m_legacyP->Pause();
    if (m_legacyFP) m_legacyFP->Pause();
	for (auto it = m_switchables.begin(); it != m_switchables.end(); ++it)
		if (it->m_audio)
			it->m_audio->Pause();

	m_paused = true;
}
bool AudioPlayback::HasEnded() const
{
	return m_music->HasEnded();
}
void AudioPlayback::SetEffect(uint32 index, HoldObjectState *object, class BeatmapPlayback &playback)
{
	// Don't use effects when using an FX track
	if (m_fxtrack)
		return;

	assert(index <= 1);
	m_CleanupDSP(m_buttonDSPs[index]);
	m_currentHoldEffects[index] = object;

	// For Time based effects
	const TimingPoint *timingPoint = playback.GetTimingPointAt(object->time);
	// Duration of a single bar
	double barDelay = timingPoint->numerator * timingPoint->beatDuration;

	DSP *&dsp = m_buttonDSPs[index];

	m_buttonEffects[index] = m_ResolveEffect(object->effectType, false);
    if (USC_AudioProbe()) {
        Logf("[AUDIO-PROBE] FX_BEGIN t=%d lane=%u id=%u kind=%u dur=%d persistent=%d",
            Logger::Severity::Warning, int(object->time), unsigned(index),
            unsigned(uint16(object->effectType)), unsigned(uint16(m_buttonEffects[index].type)),
            int(object->duration), int(m_buttonEffects[index].persistentFXCapture));
    }

    if (m_buttonEffects[index].persistentFXCapture) {
        const auto bus=EffectType(m_buttonEffects[index].independentFXBus);
        if (!m_filterCaptureTimes.count(bus)) m_filterCaptureTimes[bus]=object->time;
        SetNamedLaserFilterMix(bus,0.f);
        m_UpdateIndependentFXMix();
        return;
    }

	// Do not create DSP for SwitchAudio effect
	if (m_buttonEffects[index].type == EffectType::SwitchAudio)
		return;

	Ref<AudioStream> audioTrack = m_GetDSPTrack();

	dsp = m_buttonEffects[index].CreateDSP(m_playback->GetCurrentTimingPoint(),
										   0.0f,
										   audioTrack->GetAudioSampleRate(),
										   GetPlaybackSpeed());
	if (!dsp)
		return;

	Logf("Set effect: %s", Logger::Severity::Debug, dsp->GetName());

	auto params = *object;
	if (m_fxOverrides.count(object->effectType)) {
		if (!(object->effectParamMask & 1)) params.effectParams[0] = -1;
		if (!(object->effectParamMask & 2)) params.effectParams[1] = -1;
		params.effectType = EffectType::UserDefined0;
	}
	m_buttonEffects[index].SetParams(dsp, *this, &params);

	// Initialize mix value to previous value
	dsp->mix = m_effectMix[index];
    const auto* captureDefinition=m_beatmap->GetInactiveFilter(object->effectType);
    const bool reservedFXBus=captureDefinition && captureDefinition->persistentFXCapture;
    dsp->startTime = !reservedFXBus && m_filterCaptureTimes.count(object->effectType)
        ? m_filterCaptureTimes.at(object->effectType) : object->time;
	dsp->chartOffset = playback.GetBeatmap().GetMapSettings().offset;
	dsp->lastTimingPoint = playback.GetCurrentTimingPoint().time;

	// Add the DSP to the track
    m_ConfigureGateClock(dsp, m_buttonEffects[index]);
	audioTrack->AddDSP(dsp);
}
void AudioPlayback::SetEffectEnabled(uint32 index, bool enabled)
{
		assert(index <= 1);
    const bool changed = enabled != m_effectEnabled[index];
    if (enabled && !m_effectEnabled[index]) m_lastEnabledFXLane=index;
	m_effectEnabled[index] = enabled;
	m_effectMix[index] = enabled ? m_buttonEffects[index].mix.Sample() : 0.0f;
    if (USC_AudioProbe() && changed) {
        Logf("[AUDIO-PROBE] FX_STATE t=%d lane=%u on=%d id=%u kind=%u mix=%.3f",
            Logger::Severity::Warning, m_playback ? int(m_playback->GetLastTime()) : -1,
            unsigned(index), int(enabled),
            m_currentHoldEffects[index] ? unsigned(uint16(m_currentHoldEffects[index]->effectType)) : 0u,
            unsigned(uint16(m_buttonEffects[index].type)), double(m_effectMix[index]));
    }
	if (m_buttonEffects[index].type == EffectType::SwitchAudio)
	{
		SetSwitchableTrackEnabled(m_buttonEffects[index].switchaudio.index.Sample(), enabled);
		return;
	}
	if (m_buttonDSPs[index])
	{
		m_buttonDSPs[index]->mix = m_effectMix[index];
	}
    // ksmaudio's FX bus keeps one instance per *named* effect ID, not one per lane.
    // This is only an experiment for the case where both lanes invoke the SAME ID.
    const bool twoFlangers = m_effectEnabled[0] && m_effectEnabled[1] &&
        m_currentHoldEffects[0] && m_currentHoldEffects[1] &&
        m_buttonEffects[0].type == EffectType::Flanger &&
        m_buttonEffects[1].type == EffectType::Flanger;
    const bool sameId = twoFlangers &&
        m_currentHoldEffects[0]->effectType == m_currentHoldEffects[1]->effectType;
    if (USC_AudioProbe() && changed && twoFlangers) {
        Logf("[AUDIO-PROBE] OVERLAP_FLANGER t=%d idL=%u idR=%u same_id=%d",
            Logger::Severity::Warning, m_playback ? int(m_playback->GetLastTime()) : -1,
            unsigned(uint16(m_currentHoldEffects[0]->effectType)),
            unsigned(uint16(m_currentHoldEffects[1]->effectType)), int(sameId));
    }
    m_UpdateIndependentFXMix();
}
void AudioPlayback::ClearEffect(uint32 index, HoldObjectState *object)
{
	assert(index <= 1);
	if (m_currentHoldEffects[index] == object)
	{
		m_CleanupDSP(m_buttonDSPs[index]);
		m_currentHoldEffects[index] = nullptr;
        if (USC_AudioProbe())
            Logf("[AUDIO-PROBE] FX_END lane=%u", Logger::Severity::Warning, unsigned(index));
        m_UpdateIndependentFXMix();
	}
}
void AudioPlayback::SetLaserEffect(EffectType type)
{
	if (type != m_laserEffectType)
	{
        auto old=m_namedLaserFilters.find(uint16(m_laserEffectType));
        if(old!=m_namedLaserFilters.end() && old->second.dsp) {
            old->second.effect=m_ResolveInactiveEffect(m_laserEffectType);
            old->second.effect.UpdateDSP(old->second.dsp,m_playback->GetCurrentTimingPoint(),0.f,m_GetDSPTrack()->GetAudioSampleRate(),GetPlaybackSpeed());
            old->second.dsp->mix=old->second.mix;
            m_MirrorLaserDSP(old->second.dsp,old->second.effect,0.f,false);
        }
		m_CleanupDSP(m_laserDSP);
		m_laserEffectType = type;
		m_laserEffect = m_ResolveEffect(type, true);
	}
}
bool AudioPlayback::m_SkipEffectIfInputIsZero()
{
    // Native parameters decide the endpoint response. A reversed v range can
    // be strongest at zero; constant/custom filters may also be audible there.
    if (m_beatmap && m_beatmap->GetKSONScroll()) return false;
	return m_laserEffect.type == EffectType::PeakingFilter || m_laserEffect.type == EffectType::HighPassFilter
		|| m_laserEffect.type == EffectType::LowPassFilter || m_laserEffect.type == EffectType::PitchShift
		|| m_laserEffect.type == EffectType::Bitcrush;
}
void AudioPlayback::SetLaserFilterInput(float input, bool active, unsigned eligibleLanes)
{
    if (USC_AudioProbe() && m_playback) {
        const MapTime now = m_playback->GetLastTime();
        if (m_audioProbeLastLaserTime == std::numeric_limits<MapTime>::min() ||
            now < m_audioProbeLastLaserTime || now - m_audioProbeLastLaserTime >= 1000) {
            m_audioProbeLastLaserTime = now;
            Logf("[AUDIO-PROBE] LASER t=%d id=%u kind=%u input=%.3f active=%d eligible=%u gain=%.3f fxstem=%d",
                Logger::Severity::Warning, int(now), unsigned(uint16(m_laserEffectType)),
                unsigned(uint16(m_laserEffect.type)), double(input), int(active), eligibleLanes,
                double(m_laserEffectMix), int(bool(m_fxtrack)));
        }
    }
    // P/FP already contain laser effects. Applying a live DSP would double them.
    if (m_legacyP && m_legacyFP) { m_laserInput=input; return; }
    if(m_laserEffect.type==EffectType::PeakingFilter && m_beatmap && m_playback &&
       m_beatmap->GetMapSettings().peakingFilterDelay>0 && (!m_fxtrack || m_singleF)) {
        active=m_laserAudioSampler.Sample(double(m_playback->GetLastTime())-
            m_beatmap->GetMapSettings().peakingFilterDelay,eligibleLanes,input);
    }
    auto independent=m_namedLaserFilters.find(uint16(m_laserEffectType));
    if(independent!=m_namedLaserFilters.end() && independent->second.dsp && (independent->second.mix>0 || independent->second.effect.type==EffectType::Retrigger)) {
        m_CleanupDSP(m_laserDSP);
        GameAudioEffect effect=active?m_ResolveEffect(m_laserEffectType,true):m_ResolveInactiveEffect(m_laserEffectType);
        effect.UpdateDSP(independent->second.dsp,m_playback->GetCurrentTimingPoint(),active?input:0.f,m_GetDSPTrack()->GetAudioSampleRate(),GetPlaybackSpeed());
        independent->second.dsp->mix=active?effect.mix.Sample(input):independent->second.mix;
        m_MirrorLaserDSP(independent->second.dsp,effect,active?input:0.f,false);
        m_laserInput=input; return;
    }
	if (m_laserEffect.type != EffectType::None && (active && (input != 0.0f || !m_SkipEffectIfInputIsZero())))
	{
		if (m_laserEffect.type == EffectType::SwitchAudio)
		{
			m_laserSwitchable = m_laserEffect.switchaudio.index.Sample();
			SetSwitchableTrackEnabled(m_laserSwitchable, true);
			return;
		}

		// SwitchAudio transition into other filters
		if (m_laserSwitchable >= 0)
		{
			SetSwitchableTrackEnabled(m_laserSwitchable, false);
			m_laserSwitchable = -1;
		}

		// Create DSP
		if (!m_laserDSP)
		{
			// Don't use Bitcrush effects over FX track
			if (m_fxtrack && !m_singleF && m_laserEffectType == EffectType::Bitcrush)
				return;

			Ref<AudioStream> audioTrack = m_GetDSPTrack();

			m_laserDSP = m_laserEffect.CreateDSP(m_playback->GetCurrentTimingPoint(),
												 GetLaserFilterInput(),
												 audioTrack->GetAudioSampleRate(),
												 GetPlaybackSpeed());
			if (!m_laserDSP)
			{
				Logf("Failed to create laser DSP with type %d", Logger::Severity::Warning, m_laserEffect.type);
				return;
			}

			m_laserDSP->startTime = m_filterCaptureTimes.count(m_laserEffectType) ? m_filterCaptureTimes.at(m_laserEffectType) : m_playback->GetLastTime();
            m_laserDSP->lastTimingPoint = m_playback->GetCurrentTimingPoint().time;
            m_laserDSP->chartOffset = m_beatmap->GetMapSettings().offset;
            m_ConfigureGateClock(m_laserDSP, m_laserEffect);
			audioTrack->AddDSP(m_laserDSP);
		}

		// Set params
		m_SetLaserEffectParameter(input, m_laserDSP);
		m_laserDSP->mix = Math::Clamp(m_laserDSP->mix, 0.f, 1.f);
        m_MirrorLaserDSP(m_laserDSP,m_laserEffect,input,true);
		m_laserInput = input;
	}
	else
	{
		if (m_laserSwitchable >= 0)
			SetSwitchableTrackEnabled(m_laserSwitchable, false);

		m_laserSwitchable = -1;
		m_CleanupDSP(m_laserDSP);
		m_laserInput = 0.0f;
	}
}
float AudioPlayback::GetLaserFilterInput() const
{
	return m_laserInput;
}
void AudioPlayback::SetLaserEffectMix(float mix)
{
    if (USC_AudioProbe() && (mix != m_laserEffectMix))
        Logf("[AUDIO-PROBE] LASER_GAIN t=%d old=%.3f new=%.3f",
            Logger::Severity::Warning, m_playback ? int(m_playback->GetLastTime()) : -1,
            double(m_laserEffectMix), double(mix));
	m_laserEffectMix = mix;
}
float AudioPlayback::GetLaserEffectMix() const
{
	return m_laserEffectMix;
}
// Independent named filters are chained alongside the ordinary laser DSP.
// Explicit mix controls audibility. Triggered retriggers retain their capture at zero mix.
void AudioPlayback::SetNamedLaserFilterMix(EffectType type, float mix)
{
	// Pre-rendered or external FX tracks must not be modified in this path.
	if ((m_fxtrack && !m_singleF) || !m_music || !m_beatmap || !m_playback) return;
	mix = Math::Clamp(mix, 0.0f, 1.0f);
	const uint16 key = (uint16)type;
	auto &state = m_namedLaserFilters[key];
    if (USC_AudioProbe() && (mix != state.mix))
        Logf("[AUDIO-PROBE] NAMED_FILTER t=%d id=%u old=%.3f new=%.3f",
            Logger::Severity::Warning, int(m_playback->GetLastTime()), unsigned(key),
            double(state.mix), double(mix));
	state.mix = mix;
    const auto inactiveEffect = m_ResolveInactiveEffect(type);
    if (m_singleF && inactiveEffect.persistentFXCapture) return;
    const bool retainCapture = inactiveEffect.type == EffectType::Retrigger && m_filterCaptureTimes.count(type);
	if (mix <= 0.0f && !retainCapture)
	{
		m_CleanupDSP(state.dsp);
		return;
	}
	if (state.dsp) { state.dsp->mix = mix; m_MirrorLaserDSP(state.dsp,state.effect,0.f,false); m_UpdateIndependentFXMix(); return; }
	state.effect = inactiveEffect;
    if (state.effect.type == EffectType::None || state.effect.type == EffectType::SwitchAudio) return;
	Ref<AudioStream> track = m_GetDSPTrack();
	state.dsp = state.effect.CreateDSP(m_playback->GetCurrentTimingPoint(),
		1.0f, track->GetAudioSampleRate(), GetPlaybackSpeed());
	if (!state.dsp) return;
	state.dsp->startTime = m_filterCaptureTimes.count(type) ? m_filterCaptureTimes.at(type) : m_playback->GetLastTime();
	state.dsp->chartOffset = m_beatmap->GetMapSettings().offset;
	state.dsp->lastTimingPoint = m_playback->GetCurrentTimingPoint().time;
	state.dsp->mix = mix;
	m_ConfigureGateClock(state.dsp, state.effect);
	track->AddDSP(state.dsp);
    m_MirrorLaserDSP(state.dsp,state.effect,1.f,false);
    m_UpdateIndependentFXMix();
}
void AudioPlayback::RefreshNamedLaserFilter(EffectType type)
{
    if (!m_playback) return;
    m_filterCaptureTimes[type] = m_playback->GetLastTime();
    // Use the authored timestamp, not the frame on which its callback arrived.
    for (const auto& o : m_beatmap->GetObjectStates()) {
        if (o->time > m_playback->GetLastTime()) break;
        if (o->type != ObjectType::Event) continue;
        const auto* e = reinterpret_cast<const EventObjectState*>(o.get());
        if (e->key == EventKey::NamedLaserFilterRefresh && e->data.uintVal == uint32(uint16(type)))
            m_filterCaptureTimes[type] = e->time;
    }
    // KSH FX/filter IDs are separate namespaces. A reserved capture bus can
    // numerically equal an unrelated ordinary FX ID; do not refresh that DSP.
    const auto* captureDefinition=m_beatmap->GetInactiveFilter(type);
    const bool persistentFXBus=captureDefinition && captureDefinition->persistentFXCapture;
    for (int lane=0;lane<2;++lane)
        if(!persistentFXBus && m_currentHoldEffects[lane] && m_currentHoldEffects[lane]->effectType==type && m_buttonDSPs[lane] && m_buttonEffects[lane].type==EffectType::Retrigger)
            m_buttonDSPs[lane]->startTime=m_filterCaptureTimes[type];
    if (type == m_laserEffectType && m_laserEffect.type == EffectType::Retrigger && m_laserDSP)
        m_laserDSP->startTime = m_filterCaptureTimes[type];
    // A trigger can precede the laser invocation by many measures. Start
    // recording now even when the named filter is inaudible.
    const uint16 key = (uint16)type;
    if (m_ResolveInactiveEffect(type).type == EffectType::Retrigger) {
        const auto existing = m_namedLaserFilters.find(key);
        SetNamedLaserFilterMix(type, existing == m_namedLaserFilters.end() ? 0.f : existing->second.mix);
    }
	auto it = m_namedLaserFilters.find(key);
	if (it == m_namedLaserFilters.end() || !it->second.dsp) return;
	if (it->second.effect.type != EffectType::Retrigger) return;
	it->second.dsp->startTime = m_filterCaptureTimes[type];
    m_MirrorLaserDSP(it->second.dsp,it->second.effect,0.f,false);
}
Ref<AudioStream> AudioPlayback::m_GetDSPTrack()
{
	if (m_fxtrack)
		return m_fxtrack;
	return m_music;
}
void AudioPlayback::SetFXTrackState(bool fxActive, bool laserActive)
{
    m_legacyFXActive=fxActive;m_legacyLaserActive=laserActive;
    if (!m_legacyP || !m_legacyFP) { SetFXTrackEnabled(m_singleF ? fxActive : fxActive || laserActive); return; }
    m_music->SetVolume(!fxActive && !laserActive ? m_musicVolume : 0.f);
    m_fxtrack->SetVolume(fxActive && !laserActive ? m_musicVolume : 0.f);
    m_legacyP->SetVolume(!fxActive && laserActive ? m_musicVolume : 0.f);
    m_legacyFP->SetVolume(fxActive && laserActive ? m_musicVolume : 0.f);
}
void AudioPlayback::SetFXTrackEnabled(bool enabled)
{
    if (!m_fxtrack) return;
    // Reapply gains even when the state is unchanged (e.g. SetVolume).
    m_fxtrack->SetVolume(enabled ? m_musicVolume : 0.f);
    m_music->SetVolume(enabled ? 0.f : m_musicVolume);
    m_fxtrackEnabled = enabled;
}
void AudioPlayback::SetSwitchableTrackEnabled(size_t index, bool enabled)
{
	if (m_fxtrack)
		return;

	assert(index < m_switchables.size());

	int32 disableTrack = -1;
	int32 enableTrack = -1;

	if (!enabled)
	{
		disableTrack = index;
		m_enabledSwitchables.Remove(index);
		enableTrack = m_enabledSwitchables.size() ? m_enabledSwitchables.back() : -2;
	}
	else
	{
		disableTrack = m_enabledSwitchables.size() ? m_enabledSwitchables.back() : -2;
		m_enabledSwitchables.AddUnique(index);
		enableTrack = m_enabledSwitchables.size() ? m_enabledSwitchables.back() : -2;
	}

	if (disableTrack != -1)
	{
		if (disableTrack == -2)
			m_music->SetVolume(0.0f);
		else if (m_switchables[disableTrack].m_audio)
			m_switchables[disableTrack].m_audio->SetVolume(0.0f);
	}

	if (enableTrack != -1)
	{
		if (enableTrack == -2)
			m_music->SetVolume(m_musicVolume);
		else if (m_switchables[enableTrack].m_audio)
			m_switchables[enableTrack].m_audio->SetVolume(m_musicVolume);
	}
}

void AudioPlayback::ResetSwitchableTracks()
{
	for (size_t i = 0; i < m_switchables.size(); ++i)
	{
		if (m_switchables[i].m_audio)
			m_switchables[i].m_audio->SetVolume(0.0f);
	}
	m_music->SetVolume(m_musicVolume);
}

BeatmapPlayback &AudioPlayback::GetBeatmapPlayback()
{
	return *m_playback;
}
const Beatmap &AudioPlayback::GetBeatmap() const
{
	return *m_beatmap;
}
const String &AudioPlayback::GetBeatmapRootPath() const
{
	return m_beatmapRootPath;
}
void AudioPlayback::SetPlaybackSpeed(float speed)
{
	m_music->PlaybackSpeed = speed;
	if (m_fxtrack)
		m_fxtrack->PlaybackSpeed = speed;
    if (m_legacyP) m_legacyP->PlaybackSpeed=speed;
    if (m_legacyFP) m_legacyFP->PlaybackSpeed=speed;
	for (auto it = m_switchables.begin(); it != m_switchables.end(); ++it)
		if (it->m_audio)
			it->m_audio->PlaybackSpeed = speed;
}
float AudioPlayback::GetPlaybackSpeed() const
{
	return m_music->PlaybackSpeed;
}
void AudioPlayback::SetVolume(float volume)
{
    if (m_fxtrack) {
        m_musicVolume=volume;
        SetFXTrackState(m_legacyFXActive,m_legacyLaserActive);
        return;
    }
	m_music->SetVolume(volume);
	if (m_fxtrack)
		m_fxtrack->SetVolume(volume);
	for (auto it = m_switchables.begin(); it != m_switchables.end(); ++it)
		if (it->m_audio)
			it->m_audio->SetVolume(volume);
}
void AudioPlayback::m_CleanupDSP(DSP *&ptr)
{
	if (ptr)
	{
		auto mirror=m_legacyDryDSPs.find(ptr);
        if(mirror!=m_legacyDryDSPs.end()) {
            m_music->RemoveDSP(mirror->second);
            delete mirror->second;
            m_legacyDryDSPs.erase(mirror);
        }
        m_GetDSPTrack()->RemoveDSP(ptr);
		delete ptr;
		ptr = nullptr;
	}
}
// F-only stems contain button effects, not laser effects. Run independent
// laser DSP state on both synchronized streams, including the muted stream.
void AudioPlayback::m_MirrorLaserDSP(DSP* original, GameAudioEffect effect, float input, bool ordinary)
{
    if (!m_singleF || !original) return;
    auto& mirror=m_legacyDryDSPs[original];
    if (!mirror) {
        mirror=effect.CreateDSP(m_playback->GetCurrentTimingPoint(),input,m_music->GetAudioSampleRate(),GetPlaybackSpeed());
        if (!mirror) { m_legacyDryDSPs.erase(original); return; }
        m_ConfigureGateClock(mirror,effect);
        m_music->AddDSP(mirror);
    }
    mirror->startTime=original->startTime;
    mirror->chartOffset=original->chartOffset;
    mirror->lastTimingPoint=original->lastTimingPoint;
    if (ordinary) m_SetLaserEffectParameter(input,mirror);
    else effect.UpdateDSP(mirror,m_playback->GetCurrentTimingPoint(),input,m_music->GetAudioSampleRate(),GetPlaybackSpeed());
    mirror->mix=original->mix;
}
void AudioPlayback::m_SetLaserEffectParameter(float input, DSP* dsp)
{
	assert(input >= 0.0f && input <= 1.0f);
    if (m_laserEffectType >= EffectType::UserDefined0) {
        GameAudioEffect effect=m_laserEffect;
        if(effect.nativeLegacyGain) {
            effect.filterGain=m_laserEffectMix;
            if(effect.type==EffectType::HighPassFilter)effect.hpf.q=2+6*m_laserEffectMix;
            if(effect.type==EffectType::LowPassFilter)effect.lpf.q=2+3.2f*m_laserEffectMix;
        }
        effect.UpdateDSP(dsp, m_playback->GetCurrentTimingPoint(), input,
            m_GetDSPTrack()->GetAudioSampleRate(), GetPlaybackSpeed());
        return;
    }

    if (m_beatmap->GetMapSettings().kshBehaviorVersion >= 0) {
        const float gain=std::max(0.f,m_laserEffectMix);
        const float rate=m_GetDSPTrack()->GetAudioSampleRate();
        if (m_laserEffectType == EffectType::PeakingFilter) {
            auto p=KSHFilter::Peak(input,gain,rate);
            static_cast<BQFDSP*>(dsp)->SetPeaking(p.q,p.frequency,p.gain);
            dsp->mix=1; return;
        }
        if (m_laserEffectType == EffectType::HighPassFilter) {
            static_cast<BQFDSP*>(dsp)->SetHighPass(2+6*gain,KSHFilter::HighFrequency(input));
            dsp->mix=1; return;
        }
        if (m_laserEffectType == EffectType::LowPassFilter) {
            static_cast<BQFDSP*>(dsp)->SetLowPass(2+3.2f*gain,KSHFilter::LowFrequency(input));
            dsp->mix=1; return;
        }
    }
	// Mix float biquad filters, these are applied manualy by changing the filter parameters (gain,q,freq,etc.)
	float mix = m_laserEffectMix;
	double noteDuration = m_playback->GetCurrentTimingPoint().GetWholeNoteLength();
    if (GetPlaybackSpeed() > 0) noteDuration /= GetPlaybackSpeed();
	uint32 actualLength = m_laserEffect.duration.Sample(input).Absolute(noteDuration);

	if (input < 0.1f)
		mix *= input / 0.1f;

	switch (m_laserEffect.type)
	{
	case EffectType::Bitcrush:
	{
		dsp->mix = m_laserEffect.mix.Sample(input);
		auto *bcDSP = (BitCrusherDSP *)dsp;
		bcDSP->SetPeriod((float)m_laserEffect.bitcrusher.reduction.Sample(input));
		break;
	}
	case EffectType::Echo:
	{
		dsp->mix = m_laserEffect.mix.Sample(input);
		auto *echoDSP = (EchoDSP *)dsp;
		echoDSP->feedback = m_laserEffect.echo.feedback.Sample(input);
		break;
	}
	case EffectType::PeakingFilter:
	{
		dsp->mix = m_laserEffectMix;
		if (input > 0.8f)
			mix *= 1.0f - (input - 0.8f) / 0.2f;

		auto *bqfDSP = (BQFDSP *)dsp;
		bqfDSP->SetPeaking(m_laserEffect.peaking.q.Sample(input), m_laserEffect.peaking.freq.Sample(input), m_laserEffect.peaking.gain.Sample(input) * mix);
		break;
	}
	case EffectType::LowPassFilter:
	{
		dsp->mix = m_laserEffectMix;
		auto *bqfDSP = (BQFDSP *)dsp;
		bqfDSP->SetLowPass(m_laserEffect.lpf.q.Sample(input) * mix + 0.1f, m_laserEffect.lpf.freq.Sample(input));
		break;
	}
	case EffectType::HighPassFilter:
	{
		dsp->mix = m_laserEffectMix;
		auto *bqfDSP = (BQFDSP *)dsp;
		bqfDSP->SetHighPass(m_laserEffect.hpf.q.Sample(input) * mix + 0.1f, m_laserEffect.hpf.freq.Sample(input));
		break;
	}
	case EffectType::PitchShift:
	{
		dsp->mix = m_laserEffect.mix.Sample(input);
		auto *ps = (PitchShiftDSP *)dsp;
		ps->amount = m_laserEffect.pitchshift.amount.Sample(input);
		break;
	}
	case EffectType::Gate:
	{
		dsp->mix = m_laserEffect.mix.Sample(input);
		break;
	}
	case EffectType::Retrigger:
	{
		dsp->mix = m_laserEffect.mix.Sample(input);
		auto *rt = (RetriggerDSP *)dsp;
		rt->SetLength(m_laserEffect.duration.Sample(input).AbsolutePrecise(noteDuration));
		break;
	}
	default:
		break;
	}
}

void AudioPlayback::m_PreRenderDSPTrack()
{
	ProfilerScope $("Pre-rendering FX effects");
	Vector<DSP *> DSPs;
	for (const auto& chartObj : m_playback->GetBeatmap().GetObjectStates())
	{
		if (chartObj->type == ObjectType::Hold)
		{
			HoldObjectState *holdObj = (HoldObjectState *) chartObj.get();
			if (holdObj->effectType != EffectType::None)
			{
				//Add DSP
				GameAudioEffect effect = m_beatmap->GetEffect(holdObj->effectType);
				DSP *dsp = effect.CreateDSP(*m_playback->GetTimingPointAt(chartObj->time),
											1.0f,
											m_fxtrack->GetSampleRate(),
											1.0f);
				if (dsp != nullptr)
				{
					effect.SetParams(dsp, *this, holdObj);
					dsp->startTime = chartObj->time;
					dsp->endTime = dsp->startTime + holdObj->duration;
					dsp->SetAudioBase(m_fxtrack.get());
					dsp->priority = GameAudioEffect::GetDefaultEffectPriority(effect.type);
					Logf("Added %s at %dms", Logger::Severity::Debug, dsp->GetName(), dsp->startTime);
					DSPs.Add(dsp);
				}
			}
		}
	}
	DSPs.Sort(DSP::Sorter);
	m_fxtrack->PreRenderDSPs(DSPs);
}

GameAudioEffect::GameAudioEffect(const AudioEffect &other)
{
	*((AudioEffect *)this) = other;
}

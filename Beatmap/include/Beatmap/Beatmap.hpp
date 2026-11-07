#pragma once
#include "BeatmapObjects.hpp"
#include "AudioEffects.hpp"
#include "EffectTimeline.hpp"
#include "KSONScroll.hpp"
#include <unordered_map>
#include <Shared/Resource.hpp>

/* Global settings stored in a beatmap */
struct BeatmapSettings
{
	static bool StaticSerialize(BinaryStream& stream, BeatmapSettings*& settings);

	// Basic song meta data
	String title;
	String artist;
	String effector;
	String illustrator;
	String tags;
	// Reported BPM range by the map
	String bpm;
	// Offset in ms for the map to start
	MapTime offset;
	// Both audio tracks specified for the map / if any is set
	String audioNoFX;
	String audioFX;
	// Path to the jacket image
	String jacketPath;
	// Path to the background and foreground shader files
	String backgroundPath;
	String foregroundPath;

	// Level, as indicated by map creator
	uint8 level;

	// Difficulty, as indicated by map creator
	uint8 difficulty;

	// Total, total gauge gained when played perfectly
	uint16 total = 0;

	// Preview offset
	MapTime previewOffset;
	// Preview duration
	MapTime previewDuration;

	// Initial audio settings
    // Runtime import metadata; deliberately not added to the legacy DB serializer.
    int kshBehaviorVersion = -1; // -1: native KSON/default behavior
    bool kshLegacyKeep = false;
    bool slamAutoVolume = false;
	float slamVolume = 1.0f;
	float laserEffectMix = 1.0f;
	float musicVolume = 1.0f;

	// BPM Override for mmod calculation
	float speedBpm = -1.0f;

	EffectType laserEffectType = EffectType::PeakingFilter;
};

/*
	Generic beatmap format, Can either load it's own format or KShoot maps
*/
class Beatmap : public Unique
{
public:
	// Vector interacts badly with unique_ptr, so std::vector was used instead.
	using Objects = std::vector<std::unique_ptr<ObjectState>>;
	using ObjectsIterator = Objects::const_iterator;

	using TimingPoints = Vector<TimingPoint>;
	using TimingPointsIterator = TimingPoints::const_iterator;

	using LaneTogglePoints = Vector<LaneHideTogglePoint>;
	using LaneTogglePointsIterator = LaneTogglePoints::const_iterator;

public:
    struct AudioEffectChange {
        MapTime time;
        EffectType type;
        bool filter;
        AudioEffect effect;
    };
    const Vector<AudioEffectChange>& GetAudioEffectChanges() const { return m_audioEffectChanges; }
	bool Load(BinaryStream& input, bool metadataOnly = false);
	bool Load(const Resource& resource, bool metadataOnly = false);

	/// Returns the settings of the map, contains metadata + song/image paths.
	const BeatmapSettings& GetMapSettings() const;

	const Vector<LaneHideTogglePoint>& GetLaneTogglePoints() const { return m_laneTogglePoints; }

	const Vector<String>& GetSamplePaths() const { return m_samplePaths; }
	const Vector<String>& GetSwitchablePaths() const { return m_switchablePaths; }

	/// Retrieves audio effect settings for a given button id
	[[nodiscard]]
	AudioEffect GetEffect(EffectType type) const;
	/// Retrieves audio effect settings for a given filter effect id
	[[nodiscard]]
	AudioEffect GetFilter(EffectType type) const;

	/// Get the timing of the first (non-event) object
	[[nodiscard]]
	MapTime GetFirstObjectTime(MapTime lowerBound) const;
	/// Get the timing of the last (non-event) object
	[[nodiscard]]
	MapTime GetLastObjectTime() const;
	/// Get the timing of the last object, including the event objects
	[[nodiscard]]
	MapTime GetLastObjectTimeIncludingEvents() const;

	/// Measure -> Time
	[[nodiscard]]
	MapTime GetMapTimeFromMeasureInd(int measure) const;
	/// Time -> Measure
	[[nodiscard]]
	int GetMeasureIndFromMapTime(MapTime time) const;

	/// Computes the most frequently occuring BPM (to be used for MMod)
	[[nodiscard]]
	double GetModeBPM() const;
	void GetBPMInfo(double& startBPM, double& minBPM, double& maxBPM, double& modeBPM) const;

	void Shuffle(int seed, bool random, bool mirror);
	void ApplyShuffle(const std::array<int, 6>& swaps, bool flipLaser);

	/// # of (4th-note) beats between the start and the end
	[[nodiscard]]
	float GetBeatCount(MapTime start, MapTime end, TimingPointsIterator hint) const;
	[[nodiscard]]
	float GetBeatCountWithScrollSpeedApplied(MapTime start, MapTime end, TimingPointsIterator hint) const;

	[[nodiscard]]
	inline float GetBeatCount(MapTime start, MapTime end) const
	{
		return GetBeatCount(start, end, GetTimingPoint(start));
	}

	[[nodiscard]]
	inline float GetBeatCountWithScrollSpeedApplied(MapTime start, MapTime end) const
	{
		return GetBeatCountWithScrollSpeedApplied(start, end, GetTimingPoint(start));
	}

	[[nodiscard]]
	const Objects& GetObjectStates() const noexcept { return m_objectStates; }

	[[nodiscard]]
	ObjectsIterator GetFirstObjectState() const noexcept { return m_objectStates.begin(); }
	[[nodiscard]]
	ObjectsIterator GetEndObjectState() const noexcept { return m_objectStates.end(); }

	[[nodiscard]]
	bool HasObjectState() const noexcept { return !m_objectStates.empty(); }

	[[nodiscard]]
	const TimingPoints& GetTimingPoints() const noexcept { return m_timingPoints; }

	[[nodiscard]]
	TimingPointsIterator GetFirstTimingPoint() const noexcept { return m_timingPoints.begin(); }
	TimingPointsIterator GetEndTimingPoint() const noexcept { return m_timingPoints.end(); }

	/// Returns the latest timing point for given mapTime
	inline TimingPointsIterator GetTimingPoint(MapTime mapTime) const
	{
		return GetTimingPoint(mapTime, 0, m_timingPoints.size());
	}

	/// GetTimingPoint but a hint is given
	TimingPointsIterator GetTimingPoint(MapTime mapTime, TimingPointsIterator hint, bool forwardOnly = false) const;

	/// GetTimingPoint but begin and end ranges are specified
	inline TimingPointsIterator GetTimingPoint(MapTime mapTime, TimingPointsIterator beginIt, TimingPointsIterator endIt) const
	{
		return GetTimingPoint(mapTime, static_cast<size_t>(std::distance(m_timingPoints.begin(), beginIt)), static_cast<size_t>(std::distance(m_timingPoints.begin(), endIt)));
	}

	TimingPointsIterator GetTimingPoint(MapTime mapTime, size_t begin, size_t end) const;

	LaneTogglePointsIterator GetFirstLaneTogglePoint() const { return m_laneTogglePoints.begin(); }
	LaneTogglePointsIterator GetEndLaneTogglePoint() const { return m_laneTogglePoints.end(); }

	float GetGraphValueAt(EffectTimeline::GraphType type, MapTime mapTime) const;
	bool CheckIfManualTiltInstant(MapTime bound, MapTime mapTime) const;

	float GetCenterSplitValueAt(MapTime mapTime) const;
	float GetScrollSpeedAt(MapTime mapTime) const;
	// KSON_STAGE8_EXTREME_HOLD: read-only renderer inspection of authored scroll nodes.
	const LineGraph& GetScrollSpeedGraph() const noexcept
	{
		return m_effects.GetGraph(EffectTimeline::GraphType::SCROLL_SPEED);
	}
	// Negative scroll speeds make future object distance non-monotonic.
	// Used only to select the safe renderer visibility path; never clamps scroll.
	bool HasReverseScroll() const
	{
		const auto& graph = m_effects.GetGraph(EffectTimeline::GraphType::SCROLL_SPEED);
		for (const auto& point : graph)
		{
			if (point.second.value.first < 0.0 || point.second.value.second < 0.0)
				return true;
		}
		return false;
	}

    using VisualPath = std::vector<std::pair<double,double>>; // exact ms, absolute scroll beats
    const VisualPath& GetVisualPath(const ObjectState* obj) const
    {
        static const VisualPath empty;
        const auto it = m_visualPaths.find(obj);
        return it == m_visualPaths.end() ? empty : it->second;
    }
    const std::vector<double>& GetKSONBars() const { return m_ksonBars; }
    const KSONScroll* GetKSONScroll() const { return m_ksonScroll.get(); }
    std::pair<double, double> GetVisualTimes(const ObjectState* obj) const
    {
        const auto it = m_visualTimes.find(obj);
        if (it != m_visualTimes.end()) return it->second;
        double end = obj->time;
        const MultiObjectState* state = *obj;
        if (obj->type == ObjectType::Hold) end += state->hold.duration;
        if (obj->type == ObjectType::Laser) end += state->laser.duration;
        return {double(obj->time), end};
    }

private:
    Vector<AudioEffectChange> m_audioEffectChanges;
    std::unordered_map<const ObjectState*, VisualPath> m_visualPaths;
    std::vector<double> m_ksonBars;
    std::unique_ptr<KSONScroll> m_ksonScroll;
    std::unordered_map<const ObjectState*, std::pair<double,double>> m_visualTimes;
	bool m_ProcessKShootMap(BinaryStream& input, bool metadataOnly);
	bool m_ProcessKSON(const Resource& resource, bool metadataOnly);

	Map<EffectType, AudioEffect> m_customAudioEffects;
	Map<EffectType, AudioEffect> m_customAudioFilters;

	Objects m_objectStates;
	TimingPoints m_timingPoints;

	EffectTimeline m_effects;

	LineGraph m_centerSplit;
	Vector<LaneHideTogglePoint> m_laneTogglePoints;
	Map<String, Map<MapTime, String>> m_positionalOptions;

	Vector<String> m_samplePaths;
	Vector<String> m_switchablePaths;
	BeatmapSettings m_settings;
};
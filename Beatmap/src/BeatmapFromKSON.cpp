// KSON format_version=1 experimental native importer for USC.
// Native pulse-space scrolling with millisecond gameplay judgements.
// Native audio events and parameter snapshots feed USC DSPs.
#include "stdafx.h"
#include "Beatmap.hpp"
#include "json.hpp"
#include "KShootMap.hpp"

AudioEffect ParseCustomEffect(const KShootEffectDefinition&, Vector<String>&);
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using J = nlohmann::json;
constexpr double PULSES_PER_BEAT = 240.0;
struct TempoStep { uint32 pulse; double bpm; double ms; };

uint32 GetPulse(const J& j) {
    if (!j.is_number_integer()) throw std::runtime_error("KSON pulse must be an integer");
    const auto i = j.get<int64_t>();
    if (i < 0 || i > std::numeric_limits<uint32>::max())
        throw std::runtime_error("KSON pulse outside uint32 range");
    return uint32(i);
}

// Invert the quadratic curve's X(t), then evaluate its Y(t).
// With a=b=0, y=x (the KSON default straight interpolation).
double KsonCurveAt(double a, double b, double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    if (a == 0.0 && b == 0.0) return x;
    const double discr = std::max(0.0, 4.0 * a * a + 4.0 * (1.0 - 2.0 * a) * x);
    const double denom = 2.0 * a + std::sqrt(discr);
    const double t = denom > 1e-12 ? (2.0 * x / denom) : std::sqrt(x);
    return 2.0 * b * t * (1.0 - t) + t * t;
}

std::pair<double, double> GraphPair(const J& value) {
    if (value.is_number()) {
        const double v = value.get<double>();
        return {v, v};
    }
    if (!value.is_array() || value.size() != 2)
        throw std::runtime_error("Invalid KSON graph value");
    return {value[0].get<double>(), value[1].get<double>()};
}

const J& Member(const J& root, const char* key) {
    static const J empty = J::object();
    if (!root.is_object() || !root.contains(key) || !root[key].is_object())
        return empty;
    return root[key];
}

const J& Items(const J& root, const char* key) {
    static const J empty = J::array();
    if (!root.is_object() || !root.contains(key) || !root[key].is_array())
        return empty;
    return root[key];
}

MapTime RoundTime(double ms) {
    if (!std::isfinite(ms) || ms < -2147483647.0 || ms > 2147483647.0)
        throw std::runtime_error("KSON exceeds USC's millisecond time range");
    return MapTime(std::llround(ms));
}

// The specified numerator/denominator determines pulses in ONE measure.
uint32 MeasurePulses(int numerator, int denominator) {
    if (numerator <= 0 || numerator > 255 || denominator <= 0 || denominator > 255)
        throw std::runtime_error("Invalid KSON time signature");
    const uint32 scaled = uint32(960 * numerator);
    if (scaled % uint32(denominator))
        throw std::runtime_error("Time signature is not representable in 240 PPQ");
    return scaled / uint32(denominator);
}
} // namespace

bool Beatmap::m_ProcessKSON(const Resource& resource, bool metadataOnly) {
    try {
        std::string source;
        if (resource.IsPath()) {
            File file;
            if (!file.OpenRead(resource.GetPath())) return false;
            char chunk[16384];
            size_t bytes = 0;
            while ((bytes = file.Read(chunk, sizeof(chunk))) != 0)
                {
                if (source.size() + bytes > 32 * 1024 * 1024)
                    throw std::runtime_error("KSON file exceeds 32 MiB safety limit");
                source.append(chunk, bytes);
            }
        } else if (resource.IsValid() && resource.GetBytes()) {
            const auto& bytes = *resource.GetBytes();
            if (bytes.size() > 32 * 1024 * 1024)
                throw std::runtime_error("KSON file exceeds 32 MiB safety limit");
            source.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        } else {
            return false;
        }
        // One chart only; no temporary .ksh stubs or conversion files.
        if (source.size() > 32 * 1024 * 1024)
            throw std::runtime_error("KSON file exceeds 32 MiB safety limit");
        const J root = J::parse(source);
        if (!root.is_object() || root.value("format_version", -1) != 1)
            throw std::runtime_error("Only KSON format_version=1 is supported");
        const J& meta = Member(root, "meta");
        const J& beat = Member(root, "beat");
        const J& audio = Member(root, "audio");
        const J& bgm = Member(audio, "bgm");
        const J& preview = Member(bgm, "preview");
        m_settings.peakingFilterDelay = std::max(0, std::min(160,
            Member(Member(audio,"audio_effect"),"laser").value("peaking_filter_delay",0)));
        const J& stems = Member(bgm,"legacy");
        if(stems.contains("fp_filenames") && stems["fp_filenames"].is_array() && stems["fp_filenames"].size()>1)
            m_settings.peakingFilterDelay=0;

        // Metadata is also needed when USC scans song entries.
        m_settings.title = meta.value("title", std::string());
        m_settings.artist = meta.value("artist", std::string());
        m_settings.effector = meta.value("chart_author", std::string());
        m_settings.illustrator = meta.value("jacket_author", std::string());
        m_settings.jacketPath = meta.value("jacket_filename", std::string());
        m_settings.bpm = meta.value("disp_bpm", std::string());
        m_settings.level = uint8(std::max(1, std::min(20, meta.value("level", 1))));
        m_settings.difficulty = uint8(0);
        if (meta.contains("difficulty") && meta["difficulty"].is_number_integer())
            m_settings.difficulty = uint8(std::max(0, std::min(3, meta["difficulty"].get<int>())));
        else if (meta.contains("difficulty") && meta["difficulty"].is_string())
            m_settings.difficulty = uint8(3); // KSM v2 maps all custom difficulty names to index 3.
        const J cameraVersion = Member(root,"compat").value("ksh_version",J());
        m_settings.ksonCameraVersion = -1;
        if (cameraVersion.is_string()) {
            const auto v=cameraVersion.get<std::string>();
            if (v.size()>=3 && std::all_of(v.begin(),v.begin()+3,[](char c){return c>='0' && c<='9';})) {
                const int parsed=std::stoi(v.substr(0,3));
                if (parsed<200) m_settings.ksonCameraVersion=parsed;
            }
        }
        m_settings.audioNoFX = bgm.value("filename", std::string());
        m_settings.audioFX = "";
        m_settings.audioLaserFX = "";
        m_settings.audioCombinedFX = "";
        if (stems.contains("fp_filenames") && stems["fp_filenames"].is_array()) {
            const auto& files=stems["fp_filenames"];
            // F-only uses live laser processing; complete F/P/FP bypasses it.
            if (!files.empty() && files[0].is_string())
                m_settings.audioFX=files[0].get<std::string>();
            if (files.size()>=3 && files[0].is_string() && files[1].is_string() && files[2].is_string()) {
                m_settings.audioFX=files[0].get<std::string>();
                m_settings.audioLaserFX=files[1].get<std::string>();
                m_settings.audioCombinedFX=files[2].get<std::string>();
            }
        }
        m_settings.laserEffectMix = .5f;
        m_settings.slamVolume = .5f;
        m_settings.offset = bgm.value("offset", 0);
        m_settings.musicVolume = float(bgm.value("vol", 1.0));
        m_settings.previewOffset = preview.value("offset", 0);
        m_settings.previewDuration = preview.value("duration", 15000);
        m_settings.total = uint16(std::min(65535, Member(root, "gauge").value("total", 0)));
        if (meta.contains("std_bpm") && meta["std_bpm"].is_number())
            m_settings.speedBpm = float(meta["std_bpm"].get<double>());
        const J& bgLegacy = Member(Member(root, "bg"), "legacy");
        if (Member(bgLegacy, "layer").contains("filename"))
            m_settings.foregroundPath = Member(bgLegacy, "layer").value("filename", std::string());
        if (Items(bgLegacy, "bg").size() && Items(bgLegacy, "bg")[0].is_object())
            m_settings.backgroundPath = Items(bgLegacy, "bg")[0].value("filename", std::string());

        // Time signatures are specified by MEASURE index, not pulse.
        // Calculate each signature change's absolute pulse first.
        std::map<uint32, std::pair<int, int>> signatureAtPulse;
        std::map<uint32, std::pair<int, int>> signatureAtMeasure;
        signatureAtMeasure.emplace(0, std::make_pair(4, 4));
        for (const J& event : Items(beat, "time_sig")) {
            if (!event.is_array() || event.size() != 2 || !event[1].is_array() || event[1].size() != 2)
                throw std::runtime_error("Invalid KSON time_sig event");
            const uint32 idx = GetPulse(event[0]);
            const int num = event[1][0].get<int>();
            const int den = event[1][1].get<int>();
            MeasurePulses(num, den);
            signatureAtMeasure[idx] = {num, den};
        }
        uint64_t pulse = 0;
        uint32 measure = 0;
        int curNum = 4, curDen = 4;
        for (auto it = signatureAtMeasure.begin(); it != signatureAtMeasure.end(); ++it) {
            if (it->first > 2000000)
                throw std::runtime_error("Time signature index is unreasonably large");
            pulse += uint64_t(it->first - measure) * MeasurePulses(curNum, curDen);
            if (pulse > std::numeric_limits<uint32>::max())
                throw std::runtime_error("KSON measure pulse overflow");
            measure = it->first;
            curNum = it->second.first;
            curDen = it->second.second;
            signatureAtPulse[uint32(pulse)] = it->second;
        }

        std::map<uint32, double> bpmAtPulse;
        for (const J& e : Items(beat, "bpm")) {
            if (!e.is_array() || e.size() != 2) throw std::runtime_error("Invalid KSON BPM event");
            const double bpm = e[1].get<double>();
            if (!std::isfinite(bpm) || bpm <= 0.0) throw std::runtime_error("Invalid KSON BPM");
            bpmAtPulse[GetPulse(e[0])] = bpm;
        }
        if (bpmAtPulse.find(0) == bpmAtPulse.end())
            throw std::runtime_error("KSON is missing initial BPM at pulse 0");

        // Piecewise integration with DOUBLE precision; round to ms only at output.
        std::vector<TempoStep> tempos;
        for (const auto& b : bpmAtPulse) {
            double ms = 0.0;
            if (!tempos.empty()) {
                const auto& prev = tempos.back();
                ms = prev.ms + double(b.first - prev.pulse) * (60000.0 / (prev.bpm * PULSES_PER_BEAT));
            }
            tempos.push_back({b.first, b.second, ms});
        }
        auto ExactTimeAt = [&](uint32 y) -> double {
            const auto it = std::upper_bound(tempos.begin(), tempos.end(), y,
                [](uint32 p, const TempoStep& s) { return p < s.pulse; });
            const TempoStep& v = *(it == tempos.begin() ? tempos.begin() : it - 1);
            return double(m_settings.offset) + v.ms +
                double(y - v.pulse) * 60000.0 / (v.bpm * PULSES_PER_BEAT);
        };
        auto TimeAt = [&](uint32 y) { return RoundTime(ExactTimeAt(y)); };
        std::set<uint32> timingPulses;
        for (const auto& t : bpmAtPulse) timingPulses.insert(t.first);
        for (const auto& t : signatureAtPulse) timingPulses.insert(t.first);
        double bpmNow = bpmAtPulse.at(0);
        int numNow = 4, denNow = 4;
        for (uint32 y : timingPulses) {
            auto bpm = bpmAtPulse.find(y);
            if (bpm != bpmAtPulse.end()) bpmNow = bpm->second;
            auto sig = signatureAtPulse.find(y);
            if (sig != signatureAtPulse.end()) { numNow = sig->second.first; denNow = sig->second.second; }
            TimingPoint point;
            point.time = TimeAt(y);
            point.beatDuration = 60000.0 / bpmNow;
            point.numerator = uint8(numNow);
            point.denominator = uint8(denNow);
            m_timingPoints.Add(std::move(point));
        }
        if (metadataOnly) return true;
        m_ksonScroll = std::make_unique<KSONScroll>();
        m_visualTimes.clear();
        m_visualPaths.clear();
        for (const auto& t : tempos)
            m_ksonScroll->tempos.push_back({double(t.pulse), t.bpm, t.ms + m_settings.offset});

        // KSON camera: use USC's existing graph consumers. The values use
        // the same /100 scaling as the KSH zoom and center_split settings.
        // Camera graph control points are evaluated by LineGraph.
        const J& camera = Member(root, "camera");
        auto ValidCurve = [](const std::pair<double,double>& c) {
            return std::isfinite(c.first) && std::isfinite(c.second) &&
                c.first>=0 && c.first<=1 && c.second>=0 && c.second<=1;
        };
        const J& cameraBody = Member(Member(camera, "cam"), "body");
        auto ReadCameraGraph = [&](const char* key, LineGraph& graph, double scale) {
            for (const J& event : Items(cameraBody, key)) {
                if (!event.is_array() || event.size() < 2) continue;
                try {
                    const uint32 y = GetPulse(event[0]);
                    const auto values = GraphPair(event[1]);
                    if (!std::isfinite(values.first) || !std::isfinite(values.second)) continue;
                    LineGraph::Point point(values.first * scale, values.second * scale);
                    if (event.size() >= 3 && event[2].is_array() && event[2].size() == 2)
                        point.curve = {event[2][0].get<double>(), event[2][1].get<double>()};
                    if(!ValidCurve(point.curve)) continue;
                    graph.Insert(TimeAt(y), point);
                } catch (const std::exception&) {
                    // Camera is nonessential to chart loading: ignore malformed points.
                }
            }
        };
        ReadCameraGraph("zoom_top", m_effects.GetGraph(EffectTimeline::GraphType::ZOOM_TOP), 0.01);
        ReadCameraGraph("zoom_bottom", m_effects.GetGraph(EffectTimeline::GraphType::ZOOM_BOTTOM), 0.01);
        ReadCameraGraph("zoom_side", m_effects.GetGraph(EffectTimeline::GraphType::SHIFT_X), 0.01);
        ReadCameraGraph("center_split", m_centerSplit, 0.01);
        // KSON rotation_deg is in DEGREES, unlike KSH manual tilt.
        // Keep a separate graph so it composes with (not replaces) tilt.
        ReadCameraGraph("rotation_deg", m_effects.GetGraph(EffectTimeline::GraphType::KSON_ROTATION_DEG), 1.0);

        // KSON tilt: dispatch the same TrackRollBehaviour events used for KSH.
        // Manual tilt values are mapped using the KSH manual rotation conversion.
        // KSON rotation_deg is a separate transform and is NOT folded into this
        // graph; the current USC renderer needs a distinct rotation layer for it.
        bool manualTiltActive = false;
        for (const J& event : Items(camera, "tilt")) {
            if (!event.is_array() || event.size() != 2) continue;
            try {
                const MapTime when = TimeAt(GetPulse(event[0]));
                const J& value = event[1];
                bool manual = false;
                LineGraph::Point rotation(0.0);
                bool curved = false;
                J curve;
                std::string autoMode;
                bool manualEndpoint=false;
                if (value.is_number()) {
                    const double v = value.get<double>();
                    rotation = LineGraph::Point(v * (-10.0 / 360.0));
                    manual = true;
                } else if (value.is_string()) {
                    autoMode = value.get<std::string>();
                } else if (value.is_array() && value.size() == 2) {
                    if (value[0].is_number() && value[1].is_number()) {
                        rotation = LineGraph::Point(value[0].get<double>() * (-10.0 / 360.0),
                                                    value[1].get<double>() * (-10.0 / 360.0));
                        manual = true;
                    } else if (value[0].is_number() && value[1].is_array() && value[1].size() == 2) {
                        rotation = LineGraph::Point(value[0].get<double>() * (-10.0 / 360.0));
                        curve = value[1]; curved = true; manual = true;
                    } else if (value[0].is_array() && value[0].size() == 2 &&
                               value[1].is_array() && value[1].size() == 2) {
                        rotation = LineGraph::Point(value[0][0].get<double>() * (-10.0 / 360.0),
                                                    value[0][1].get<double>() * (-10.0 / 360.0));
                        curve = value[1]; curved = true; manual = true;
                    } else if (value[0].is_number() && value[1].is_string()) {
                        rotation=LineGraph::Point(value[0].get<double>()*(-10.0/360.0));
                        if(!std::isfinite(rotation.value.first)) continue;
                        manualEndpoint=true;
                        autoMode = value[1].get<std::string>();
                    } else continue;
                } else continue;

                TrackRollBehaviour behaviour = TrackRollBehaviour::Zero;
                if (manual) {
                    if (!std::isfinite(rotation.value.first) || !std::isfinite(rotation.value.second)) continue;
                    if (curved)
                        rotation.curve = {curve[0].get<double>(), curve[1].get<double>()};
                    if(!ValidCurve(rotation.curve)) continue;
                    m_effects.GetGraph(EffectTimeline::GraphType::ROTATION_Z).Insert(when, rotation);
                    behaviour = TrackRollBehaviour::Manual;
                    manualTiltActive = true;
                } else {

                    bool keep = false;
                    if (autoMode.compare(0, 5, "keep_") == 0) {
                        keep = true; autoMode = autoMode.substr(5);
                    }
                    if (autoMode == "normal") behaviour = TrackRollBehaviour::Normal;
                    else if (autoMode == "bigger") behaviour = TrackRollBehaviour::Bigger;
                    else if (autoMode == "biggest") behaviour = TrackRollBehaviour::Biggest;
                    else if (autoMode == "zero") behaviour = TrackRollBehaviour::Zero;
                    else continue;
                    if (keep) behaviour = behaviour | TrackRollBehaviour::Keep;
                    if(manualEndpoint)
                        m_effects.GetGraph(EffectTimeline::GraphType::ROTATION_Z).Insert(when,rotation);
                    else if(manualTiltActive)
                        m_effects.GetGraph(EffectTimeline::GraphType::ROTATION_Z).Extend(when);
                    manualTiltActive=false;
                }
                auto* evt = new EventObjectState();
                evt->time = when;
                evt->interTickIndex = 0;
                evt->key = EventKey::TrackRollBehaviour;
                evt->data.rollVal = behaviour;
                m_objectStates.emplace_back(std::unique_ptr<ObjectState>(*evt));
            } catch (const std::exception&) {
                // Ignore unsupported camera encodings; keep the notes playable.
            }
        }

        LaneHideTogglePoint firstLane;
        firstLane.time = 0;
        firstLane.duration = 1;
        m_laneTogglePoints.Add(std::move(firstLane));

        uint32 lastPulse = 0;
        const J& note = Member(root, "note");
        // Audio invocation is defined in chart pulses, not rounded milliseconds.
        // Keep lane-local indexes while creating notes so dense event lists do
        // not repeatedly scan every note (including subdivided laser curves).
        struct FXHoldSpan { uint32 end; HoldObjectState* object; };
        std::map<uint32, FXHoldSpan> fxHolds[2];
        std::multimap<uint32, ButtonObjectState*> fxChips[2];
        std::map<uint32, std::vector<LaserObjectState*>> slamsByPulse;
        // BT, FX: an integer is a chip; [pulse,length] is chip/hold.
        auto ReadButtons = [&](const char* key, int laneCount, int baseIndex) {
            const J& lanes = Items(note, key);
            for (size_t i = 0; i < lanes.size() && int(i) < laneCount; ++i) {
                if (!lanes[i].is_array()) continue;
                for (const J& value : lanes[i]) {
                    uint32 y, len = 0;
                    if (value.is_number_integer()) y = GetPulse(value);
                    else if (value.is_array() && value.size() == 2) {
                        y = GetPulse(value[0]);
                        len = GetPulse(value[1]);
                    } else throw std::runtime_error("Invalid KSON button note");
                    if (uint64_t(y) + len > std::numeric_limits<uint32>::max())
                        throw std::runtime_error("KSON button hold tick overflow");
                    lastPulse = std::max(lastPulse, y + len);
                    if (len) {
                        auto* obj = new HoldObjectState();
                        obj->time = TimeAt(y);
                        obj->duration = std::max(MapTime(1), MapTime(TimeAt(y + len) - obj->time));
                        obj->index = uint8(baseIndex + i);
                        if (baseIndex == 4) fxHolds[i].emplace(y, FXHoldSpan{y + len, obj});
                        m_visualTimes[*obj] = {ExactTimeAt(y), ExactTimeAt(y + len)};
                        m_objectStates.emplace_back(std::unique_ptr<ObjectState>(*obj));
                    } else {
                        auto* obj = new ButtonObjectState();
                        obj->time = TimeAt(y);
                        obj->index = uint8(baseIndex + i);
                        if (baseIndex == 4) fxChips[i].emplace(y, obj);
                        m_visualTimes[*obj] = {ExactTimeAt(y), ExactTimeAt(y + len)};
                        m_objectStates.emplace_back(std::unique_ptr<ObjectState>(*obj));
                    }
                }
            }
        };
        ReadButtons("bt", 4, 0);
        ReadButtons("fx", 2, 4);

        // A KSON graph value may be [v,vf]: this denotes an INSTANT change,
        // not a position range. Curved interpolation is deferred to stage 2.
        // KSON camera slam patterns: attach native spin metadata to laser slams.
        // Native spin lengths remain in pulses, including across BPM changes.
        using SpinSpec = std::pair<SpinStruct::SpinType, uint32>;
        std::map<std::pair<uint32, int>, SpinSpec> spinAtPulse;
        const J& cameraSlamEvents = Member(
            Member(Member(Member(root, "camera"), "cam"), "pattern"), "laser");
        const J& patterns = Member(cameraSlamEvents, "slam_event");
        auto ReadSpinPattern = [&](const char* name, SpinStruct::SpinType type) {
            for (const J& entry : Items(patterns, name)) {
                if (!entry.is_array() || entry.size() != 3) continue;
                try {
                    const uint32 pulse = GetPulse(entry[0]);
                    const int direction = entry[1].get<int>();
                    const uint32 length = GetPulse(entry[2]);
                    if ((direction != -1 && direction != 1) || length == 0)
                        continue;
                    spinAtPulse[{pulse, direction}] = {type, length};
                } catch (const std::exception&) {
                    // Bad optional camera data must not reject a playable chart.
                }
            }
        };
        ReadSpinPattern("spin", SpinStruct::SpinType::Full);
        ReadSpinPattern("half_spin", SpinStruct::SpinType::Quarter);

        std::map<std::pair<uint32,int>, KSONSwing> swingAtPulse;
        for(const auto& entry : Items(patterns,"swing")) {
            if(!entry.is_array() || (entry.size()!=3 && entry.size()!=4)) continue;
            try {
                const uint32 pulse=GetPulse(entry[0]);
                KSONSwing swing; swing.direction=entry[1].get<int>(); swing.length=GetPulse(entry[2]);
                if(!swing.length || (swing.direction!=-1 && swing.direction!=1)) continue;
                if(entry.size()==4) {
                    if(!entry[3].is_object()) continue;
                    swing.scale=entry[3].value("scale",250.0);
                    if(entry[3].contains("repeat")) swing.repeat=GetPulse(entry[3]["repeat"]);
                    if(entry[3].contains("decay_order")) swing.decay=GetPulse(entry[3]["decay_order"]);
                }
                if(!std::isfinite(swing.scale) || swing.decay>2) continue;
                swingAtPulse[{pulse,swing.direction}]=swing;
            } catch(const std::exception&) { /* Ignore invalid optional camera data. */ }
        }

        const J& laserLanes = Items(note, "laser");
        for (size_t lane = 0; lane < laserLanes.size() && lane < 2; ++lane) {
            if (!laserLanes[lane].is_array()) continue;
            for (const J& section : laserLanes[lane]) {
                if (!section.is_array() || section.size() < 2 || !section[1].is_array())
                    throw std::runtime_error("Invalid KSON laser section");
                const uint32 sectionY = GetPulse(section[0]);
                const bool extended = section.size() >= 3 && section[2].get<int>() == 2;
                const J& points = section[1];
                LaserObjectState* previous = nullptr;
                auto addSegment = [&](uint32 y0, uint32 y1, double p0, double p1, bool instantaneous) {
                    auto* obj = new LaserObjectState();
                    obj->index = uint8(lane);
                    obj->time = TimeAt(y0);
                    obj->tick = y0;
                    m_visualTimes[*obj] = {ExactTimeAt(y0), ExactTimeAt(y1)};
                    obj->duration = instantaneous ? 0 : std::max(MapTime(0), MapTime(TimeAt(y1) - obj->time));
                    obj->points[0] = float(p0);
                    obj->points[1] = float(p1);
                    if (extended) obj->flags |= LaserObjectState::flag_Extended;
                    if (instantaneous) obj->flags |= LaserObjectState::flag_Instant;
                    // A KSON camera pattern applies only to an actual same-pulse,
                    // same-direction laser slam; other laser events are unaffected.
                    if (instantaneous) {
                        slamsByPulse[y0].push_back(obj);
                        const int direction = p1 > p0 ? 1 : (p1 < p0 ? -1 : 0);
                        const auto swing = swingAtPulse.find({y0,direction});
                        if(swing!=swingAtPulse.end()) obj->nativeSwing=swing->second;
                        const auto spin = spinAtPulse.find({y0, direction});
                        if (spin != spinAtPulse.end()) {
                            obj->spin.type = spin->second.first;
                            obj->nativeSpinLength = spin->second.second;
                            obj->spin.duration = 1; // Native camera path uses exact pulses.
                            obj->spin.direction = static_cast<float>(direction);
                        }
                    }
                    if (previous) { obj->prev = previous; previous->next = obj; }
                    previous = obj;
                    m_objectStates.emplace_back(std::unique_ptr<ObjectState>(*obj));
                };
                uint32 prevY = 0;
                double prevAfter = 0.0;
                // KSON curve belongs to the point at the START of a segment.
                std::pair<double, double> prevCurve = {0.0, 0.0};
                bool first = true;
                for (const J& pt : points) {
                    if (!pt.is_array() || pt.size() < 2) throw std::runtime_error("Invalid laser point");
                    const uint32 rel = GetPulse(pt[0]);
                    if (uint64_t(sectionY) + rel > std::numeric_limits<uint32>::max())
                        throw std::runtime_error("KSON laser tick overflow");
                    const uint32 y = sectionY + rel;
                    const auto values = GraphPair(pt[1]);
                    if ((!first && y <= prevY) || !std::isfinite(values.first) ||
                        !std::isfinite(values.second))
                        throw std::runtime_error("Invalid KSON laser point ordering or position");
                    lastPulse = std::max(lastPulse, y);
                    if (!first && y > prevY) {
                        // USC interpolates each object in milliseconds, whereas KSON
                        // defines its graph in pulses. Always split at tempo changes,
                        // including straight graphs, before approximating the curve.
                        auto valueAt = [&](uint32 pulse) {
                            const double x = double(pulse - prevY) / double(y - prevY);
                            return prevAfter + (values.first - prevAfter) *
                                KsonCurveAt(prevCurve.first, prevCurve.second, x);
                        };
                        unsigned splitsLeft = 255;
                        auto subdivide = [&](auto&& self, uint32 lo, uint32 hi,
                                             double vlo, double vhi, unsigned depth) -> void {
                            const double x0 = double(lo - prevY) / double(y - prevY);
                            const double x1 = double(hi - prevY) / double(y - prevY);
                            const double a = prevCurve.first, b = prevCurve.second;
                            double error = 0.0;
                            if (values.first != prevAfter && a != b) {
                                // A quadratic graph has curvature of constant sign.
                                // Its greatest chord error is where dy/dx equals the
                                // chord slope; check that extremum, not just midpoint.
                                const double f0 = KsonCurveAt(a, b, x0);
                                const double f1 = KsonCurveAt(a, b, x1);
                                const double slope = (f1 - f0) / (x1 - x0);
                                const double denom = (1.0 - 2.0*b) - slope*(1.0 - 2.0*a);
                                if (std::abs(denom) > 1e-14) {
                                    const double t = (slope*a - b) / denom;
                                    const double x = 2.0*a*t*(1.0-t) + t*t;
                                    if (t > 0.0 && t < 1.0 && x > x0 && x < x1) {
                                        const double v = 2.0*b*t*(1.0-t) + t*t;
                                        error = std::abs((v - (f0 + slope*(x-x0))) *
                                            (values.first - prevAfter));
                                    }
                                }
                            }
                            const uint32 mid = lo + (hi - lo)/2;
                            // Bound mesh growth and respect USC's millisecond clock.
                            // Extended lasers span twice the width, so halve tolerance.
                            const double tolerance = extended ? 0.0005 : 0.001;
                            if (error > tolerance && depth < 24 && splitsLeft > 0 && mid > lo && mid < hi &&
                                TimeAt(mid) > TimeAt(lo) && TimeAt(mid) < TimeAt(hi)) {
                                --splitsLeft;
                                const double vmid = valueAt(mid);
                                self(self, lo, mid, vlo, vmid, depth+1);
                                self(self, mid, hi, vmid, vhi, depth+1);
                            } else {
                                addSegment(lo, hi, vlo, vhi, false);
                            }
                        };
                        uint32 start = prevY;
                        double startValue = prevAfter;
                        auto tempo = bpmAtPulse.upper_bound(prevY);
                        while (tempo != bpmAtPulse.end() && tempo->first < y) {
                            const uint32 end = tempo->first;
                            const double endValue = valueAt(end);
                            splitsLeft = 255;
                            subdivide(subdivide, start, end, startValue, endValue, 0);
                            start = end;
                            startValue = endValue;
                            ++tempo;
                        }
                        splitsLeft = 255;
                        subdivide(subdivide, start, y, startValue, values.first, 0);
                    }
                    if (values.first != values.second) addSegment(y, y, values.first, values.second, true);
                    prevCurve = {0.0, 0.0};
                    if (pt.size() >= 3 && pt[2].is_array() && pt[2].size() == 2) {
                        const double a = pt[2][0].get<double>(), b = pt[2][1].get<double>();
                        if (std::isfinite(a) && std::isfinite(b) &&
                            a >= 0.0 && a <= 1.0 && b >= 0.0 && b <= 1.0)
                            prevCurve = {a, b};
                    }
                    prevAfter = values.second;
                    prevY = y;
                    first = false;
                }
            }
        }

        // KSON stops change SCROLL SPEED, not elapsed music time.
        LineGraph& scroll = m_effects.GetGraph(EffectTimeline::GraphType::SCROLL_SPEED);
        for (const J& point : Items(beat, "scroll_speed")) {
            if (!point.is_array() || point.size() < 2) continue;
            const uint32 y = GetPulse(point[0]);
            const auto v = GraphPair(point[1]);
            LineGraph::Point p(v.first, v.second);
            if (point.size() >= 3 && point[2].is_array() && point[2].size() == 2)
                p.curve = {point[2][0].get<double>(), point[2][1].get<double>()};
            if (!std::isfinite(v.first) || !std::isfinite(v.second) ||
                !std::isfinite(p.curve.first) || !std::isfinite(p.curve.second) ||
                p.curve.first < 0 || p.curve.first > 1 || p.curve.second < 0 || p.curve.second > 1)
                throw std::runtime_error("Invalid KSON scroll graph");
            m_ksonScroll->graph[y] = {v.first, v.second, p.curve.first, p.curve.second};
            scroll.Insert(TimeAt(y), p);
            lastPulse = std::max(lastPulse, y);
        }
        for (const J& stop : Items(beat, "stop")) {
            if (!stop.is_array() || stop.size() != 2) continue;
            const uint32 y = GetPulse(stop[0]), len = GetPulse(stop[1]);
            if (uint64_t(y) + len > std::numeric_limits<uint32>::max())
                throw std::runtime_error("KSON stop tick overflow");
            lastPulse = std::max(lastPulse, uint32(y + len));
            m_ksonScroll->stops[y] = len;
            if (len && TimeAt(y + len) > TimeAt(y))
                scroll.RangeSet(TimeAt(y), TimeAt(y + len), 0.0);
        }

        // Adapt supported native audio definitions to USC's existing DSP parser.
        // Definitions are per event so overrides never mutate another hold.
        const std::map<std::string, std::string> effectNames = {
            {"retrigger","Retrigger"},{"gate","Gate"},{"flanger","Flanger"},
            {"phaser","Phaser"},{"tapestop","TapeStop"},{"bitcrusher","BitCrusher"},
            {"wobble","Wobble"},{"sidechain","SideChain"},{"echo","Echo"},
            {"panning","Panning"},{"pitch_shift","PitchShift"},
            {"low_pass_filter","LPF"},{"high_pass_filter","HPF"},
            {"peaking_filter","PEAK"},{"switch_audio","SwitchAudio"}};
        const std::map<std::string, std::string> paramNames = {
            {"wave_length","waveLength"},{"update_period","updatePeriod"},
            {"freq_1","loFreq"},{"freq_2","hiFreq"},{"q","Q"},
            {"vol","volume"},{"stereo_width","stereoWidth"},{"hi_cut_gain","hiCutGain"},
            {"attack_time","attackTime"},{"hold_time","holdTime"},
            {"release_time","releaseTime"},{"filename","fileName"},
            {"feedback_level","feedbackLevel"},{"chunk_size","chunkSize"},{"overlap","overWrap"}};
        const J& effects = Member(audio,"audio_effect");
        const J& compat = Member(root,"compat");
        const J version = compat.value("ksh_version", J());
        bool legacyCustomDefaults = false;
        if (version.is_string()) {
            const std::string v = version.get<std::string>();
            // Historical suffixes (e.g. 120b) follow a three-digit version.
            if (v.size() >= 3 && v[0] >= '0' && v[0] <= '9' &&
                v[1] >= '0' && v[1] <= '9' && v[2] >= '0' && v[2] <= '9')
                legacyCustomDefaults = std::stoi(v.substr(0,3)) < 200;
        }
        uint32 nextEffect = uint32(EffectType::UserDefined0);
        auto MakeEffect = [&](const J& category, const std::string& name, const J& overrides, bool off = false) {
            if(name.empty())return AudioEffect::GetDefault(EffectType::None);
            bool customDefinition=false;
            J definition = {{"type",name},{"v",J::object()}};
            for (const auto& def : Items(category,"def"))
                if (def.is_array() && def.size()==2 && def[0]==name) {definition=def[1];customDefinition=true;}
            const auto type = effectNames.find(definition.value("type", std::string()));
            if (type==effectNames.end()) {
                Logf("[KSON] Unsupported audio effect: %s",Logger::Severity::Warning,name.c_str());
                return AudioEffect::GetDefault(EffectType::None);
            }
            KShootEffectDefinition legacy;
            legacy.typeName=name;
            legacy.parameters["type"]=type->second;
            J parameters = Member(definition,"v");
            if (!parameters.is_object()) parameters = J::object();
            // Native KSON defaults differ from historical USC/KSH defaults.
            const std::string base = definition.value("type",std::string());
            if ((base=="retrigger" || base=="gate" || base=="echo" || base=="wobble") && !parameters.contains("wave_length")) parameters["wave_length"]="0";
            if (base=="wobble") {
                if(!parameters.contains("freq_1")) parameters["freq_1"]="500Hz";
                if(!parameters.contains("freq_2")) parameters["freq_2"]="20000Hz";
            }
            if(!parameters.contains("mix")) parameters["mix"]=base=="gate"?"0%>90%":base=="phaser"?"0%>50%":(base=="flanger"||base=="wobble")?"0%>80%":"0%>100%";
            if(base=="bitcrusher"&&!parameters.contains("reduction"))parameters["reduction"]="0samples-30samples";
            if(base=="tapestop"&&!parameters.contains("speed"))parameters["speed"]="50%";
            // KSON's Off>On defaults must be explicit. The KSH/USC DSP
            // defaults only describe the active state, not the inactive bus.
            if(base=="pitch_shift"&&!parameters.contains("pitch"))parameters["pitch"]="0>12";
            if(base=="sidechain"&&!parameters.contains("ratio"))parameters["ratio"]="1.0>5.0";
            if(base=="sidechain"&&!parameters.contains("release_time"))parameters["release_time"]=(customDefinition && legacyCustomDefaults)?"1/16":"1/8";
            if (overrides.is_object()) for(auto it=overrides.begin();it!=overrides.end();++it) parameters[it.key()]=it.value();
            for(auto it=parameters.begin();it!=parameters.end();++it) {
                const auto key=paramNames.find(it.key());
                const std::string mapped=key==paramNames.end()?it.key():key->second;
                std::string value=it.value().is_string()?it.value().get<std::string>():it.value().dump();
                if(off) {
                    const auto arrow=value.find('>');
                    if(arrow!=std::string::npos) value=value.substr(0,arrow);
                    else {const auto dash=value.find('-',1);if(dash!=std::string::npos)value=value.substr(0,dash);}
                }
                legacy.parameters[mapped]=value;
            }
            AudioEffect result=ParseCustomEffect(legacy,m_switchablePaths);
            // Parse native filter controls independently of KSH's Hz/Q fields.
            auto range=[&](const std::string& key, float fallback, float maximum) {
                if(!parameters.contains(key)) return EffectParam<float>(fallback,maximum);
                std::string value=parameters[key].is_string()?parameters[key].get<std::string>():parameters[key].dump();
                const auto arrow=value.find('>');
                if(arrow!=std::string::npos)value=off?value.substr(0,arrow):value.substr(arrow+1);
                const auto dash=value.find('-',1);
                auto number=[](const std::string& text){float v=std::stof(text);if(text.find('%')!=std::string::npos)v/=100;else if(text.find("kHz")!=std::string::npos)v*=1000;return v;};
                const float first=number(value);
                const float last=dash==std::string::npos||off?first:number(value.substr(dash+1));
                if(!std::isfinite(first)||!std::isfinite(last))throw std::runtime_error("Non-finite KSON effect parameter");
                return EffectParam<float>(first,last);
            };
            if(base=="peaking_filter"||base=="high_pass_filter"||base=="low_pass_filter") {
                result.nativeFilter=true;
                result.nativeLegacyGain=!customDefinition;
                result.filterPosition=range("v",0,off?0:1);
                const float first=base=="peaking_filter"?50:base=="high_pass_filter"?100:15000;
                const float last=base=="peaking_filter"?7900:base=="high_pass_filter"?2200:800;
                result.filterFreq=range("freq",first,first);
                result.filterFreqMax=range("freq_max",last,last);
                result.filterGain=range("gain",.5,.5);
                result.filterBandwidth=range("bandwidth",1.2,1.2);
                if(base=="high_pass_filter")result.hpf.q=range("q",5,5);
                if(base=="low_pass_filter")result.lpf.q=range("q",3.6,3.6);
            }
            if(base=="tapestop") {
                auto speed=range("speed",.5,.5);
                result.duration=EffectParam<EffectDuration>(EffectDuration(int32(160/std::max(.001f,speed.Sample(0)))),EffectDuration(int32(160/std::max(.001f,speed.Sample(1)))));
            }
            return result;
        };
        auto AddEvent = [&](uint32 pulse, EventKey key) {
            auto* event = new EventObjectState();
            event->time=TimeAt(pulse); event->key=key; event->interTickIndex=0;
            m_objectStates.emplace_back(std::unique_ptr<ObjectState>(*event));
            lastPulse=std::max(lastPulse,pulse);
            return event;
        };
        struct Binding { std::string name; EffectType id; J overrides; };
        std::vector<Binding> fxBindings;
        const J& fxEffects=Member(effects,"fx");
        struct FXCall {std::string name; size_t lane; J event;};
        std::vector<FXCall> calls;
        for(auto named=Member(fxEffects,"long_event").begin();named!=Member(fxEffects,"long_event").end();++named)
            if(named.value().is_array())for(size_t lane=0;lane<named.value().size()&&lane<2;++lane)
                for(const auto& event:named.value()[lane])calls.push_back({named.key(),lane,event});
        std::stable_sort(calls.begin(),calls.end(),[](const auto& a,const auto& b){return GetPulse(a.event.is_array()?a.event[0]:a.event)<GetPulse(b.event.is_array()?b.event[0]:b.event);});
        for(const auto& call:calls) {
            const auto& event=call.event;const auto lane=call.lane;
                    const uint32 y=GetPulse(event.is_array()?event[0]:event);
                    const J overrides=event.is_array() && event.size()>1?event[1]:J::object();
                    const auto effect=MakeEffect(fxEffects,call.name,overrides);
                    if(effect.type==EffectType::None && !call.name.empty()) continue;
                    HoldObjectState* hold=nullptr;
                    const double exact=ExactTimeAt(y);
                    auto span=fxHolds[lane].upper_bound(y);
                    if(span!=fxHolds[lane].begin()) {
                        --span;
                        if(y<span->second.end) hold=span->second.object;
                    }
                    if(!hold) {
                        Logf("[KSON] FX event without a matching hold at pulse %u",Logger::Severity::Warning,y);
                        continue;
                    }
                    const auto times=GetVisualTimes(*hold);
                    if(y>span->first) {
                        // Preserve the hold chain when an effect starts mid-note.
                        const MapTime split=TimeAt(y), end=hold->time+hold->duration;
                        if(split>=end) {
                            Logf("[KSON] Sub-millisecond FX split cannot be represented at pulse %u",Logger::Severity::Warning,y);
                            continue;
                        }
                        if (split > hold->time) {
                            auto* continuation=new HoldObjectState(*hold);
                            continuation->time=split; continuation->duration=end-split;
                            continuation->prev=hold;
                            if(hold->next) hold->next->prev=continuation;
                            hold->next=continuation; hold->duration=split-hold->time;
                            m_visualTimes[*hold]={times.first,exact};
                            m_visualTimes[*continuation]={exact,times.second};
                            m_objectStates.emplace_back(std::unique_ptr<ObjectState>(*continuation));
                            const uint32 endPulse=span->second.end;
                            span->second.end=y;
                            fxHolds[lane].emplace(y, FXHoldSpan{endPulse, continuation});
                            hold=continuation;
                        }
                        // Same-ms calls share one gameplay interval. Keep the last
                        // authored effect rather than discarding every later call.
                    }
                    if(call.name.empty()) {
                        hold->effectType=EffectType::None;
                        hold->effectParams[0]=hold->effectParams[1]=-1;
                        hold->effectParamMask=0;
                        continue;
                    }
                    hold->effectType=EffectType(nextEffect++);
                    m_customAudioEffects[hold->effectType]=effect;
                    // The definition already carries native durations and fractional values.
                    hold->effectParams[0]=hold->effectParams[1]=-1;
                    hold->effectParamMask=0;
                    fxBindings.push_back({call.name,hold->effectType,overrides});
        }
        std::map<std::string,EffectType> laserIds;
        const J& laserEffects=Member(effects,"laser");
        bool initialLaser=false;
        for(auto named=Member(laserEffects,"pulse_event").begin();named!=Member(laserEffects,"pulse_event").end();++named) {
            const auto effect=MakeEffect(laserEffects,named.key(),J::object());
            if(effect.type==EffectType::None) continue;
            const auto id=EffectType(nextEffect++);
            m_customAudioFilters[id]=effect;
            laserIds[named.key()]=id;
            for(const auto& event:named.value()) {
                if(!event.is_number_integer()) continue;
                const uint32 y=GetPulse(event);
                if(y==0) initialLaser=true;
                AddEvent(y,EventKey::LaserEffectType)->data.effectVal=id;
            }
        }
        if(!initialLaser) {
            auto id=EffectType(nextEffect++);
            m_customAudioFilters[id]=MakeEffect(laserEffects,"peaking_filter",J::object());
            laserIds["peaking_filter"]=id;
            AddEvent(0,EventKey::LaserEffectType)->data.effectVal=id;
        }
        for(const auto& e:Items(Member(laserEffects,"legacy"),"filter_gain"))
            if(e.is_array() && e.size()==2) AddEvent(GetPulse(e[0]),EventKey::LaserEffectMix)->data.floatVal=e[1].get<float>();
        const J& keySound=Member(audio,"key_sound");
        const J& chipEvents=Member(Member(keySound,"fx"),"chip_event");
        for(auto sample=chipEvents.begin();sample!=chipEvents.end();++sample) {
            if(!sample.value().is_array() || m_samplePaths.size()>=255) continue;
            const uint8 index=uint8(m_samplePaths.size());
            m_samplePaths.Add(sample.key());
            for(size_t lane=0;lane<sample.value().size() && lane<2;++lane)
                for(const auto& event:sample.value()[lane]) {
                    const uint32 y=GetPulse(event.is_array()?event[0]:event);
                    const float volume=event.is_array() && event.size()>1?event[1].value("vol",1.0f):1.0f;
                    const auto matches=fxChips[lane].equal_range(y);
                    for(auto match=matches.first;match!=matches.second;++match) {
                        auto* button=match->second;
                        button->hasSample=true; button->sampleIndex=index; button->sampleVolume=volume;
                    }
                }
        }
        const auto& laserSounds=Member(keySound,"laser");
        m_settings.slamAutoVolume=Member(laserSounds,"legacy").value("vol_auto",false);
        const auto& slams=Member(laserSounds,"slam_event");
        for(auto named=slams.begin();named!=slams.end();++named) {
            uint8 preset=0;uint16 sample=65535;
            const auto name=named.key();
            if(name=="slam_mute"||name=="mute")preset=3;
            else if(name=="slam_up"||name=="up")preset=4;
            else if(name=="slam_down"||name=="down")preset=1;
            else if(name=="slam_swing"||name=="swing")preset=2;
            else if(m_samplePaths.size()<255){sample=uint16(m_samplePaths.size());m_samplePaths.Add(name);}
            for(const auto& event:named.value()) {
                const auto match=slamsByPulse.find(GetPulse(event));
                if(match==slamsByPulse.end()) continue;
                for(auto* laser:match->second) {
                    laser->slamSound=preset;laser->slamSample=sample;
                }
            }
        }
        for(const auto& e:Items(Member(keySound,"laser"),"vol"))
            if(e.is_array() && e.size()==2) AddEvent(GetPulse(e[0]),EventKey::SlamVolume)->data.floatVal=e[1].get<float>();
        // Parameter-only effects are real buses: a constant mix is audible even without notes.
        auto automation=[&](const J& category, bool laser) {
            J changes=Member(category,"param_change");
            if(!changes.is_object())changes=J::object();
            for(const auto& def:Items(category,"def"))if(def.is_array()&&def.size()==2&&!changes.contains(def[0].get<std::string>()))changes[def[0].get<std::string>()]=J::object();
            for(auto named=changes.begin();named!=changes.end();++named) {
                const std::string name=named.key();
                EffectType bus;
                if(laser && laserIds.count(name))bus=laserIds[name];
                else {bus=EffectType(nextEffect++);m_customAudioFilters[bus]=MakeEffect(category,name,J::object());if(laser)laserIds[name]=bus;}
                m_inactiveAudioFilters[bus]=MakeEffect(category,name,J::object(),true);
                if(!laser)for(const auto& binding:fxBindings)if(binding.name==name)m_customAudioEffects[binding.id].independentFXBus=uint16(bus);
                std::map<uint32,J> byPulse;
                byPulse[0]=J::object();
                for(auto property=named.value().begin();property!=named.value().end();++property)
                    for(const auto& point:property.value())if(point.is_array()&&point.size()==2) {
                        const auto y=GetPulse(point[0]);
                        if(!byPulse[y].is_object())byPulse[y]=J::object();
                        byPulse[y][property.key()]=point[1];
                    }
                J cumulative=J::object();
                for(const auto& point:byPulse) {
                    for(auto property=point.second.begin();property!=point.second.end();++property)cumulative[property.key()]=property.value();
                    m_audioEffectChanges.Add({TimeAt(point.first),bus,true,MakeEffect(category,name,cumulative)});
                    m_audioEffectChanges.Add({TimeAt(point.first),bus,true,MakeEffect(category,name,cumulative,true),true});
                    if(!laser)for(const auto& binding:fxBindings)if(binding.name==name) {
                        J values=cumulative;
                        for(auto v=binding.overrides.begin();v!=binding.overrides.end();++v)values[v.key()]=v.value();
                        auto effect=MakeEffect(category,name,values);
                        effect.independentFXBus=uint16(bus);
                        m_audioEffectChanges.Add({TimeAt(point.first),binding.id,false,effect});
                    }
                    if(point.second.contains("update_trigger") && point.second["update_trigger"]=="on") {
                        AddEvent(point.first,EventKey::NamedLaserFilterRefresh)->data.uintVal=uint16(bus);
                        if(!laser)for(const auto& binding:fxBindings)if(binding.name==name)
                            AddEvent(point.first,EventKey::NamedLaserFilterRefresh)->data.uintVal=uint16(binding.id);
                    }
                    if(point.first==0 || point.second.contains("mix")) {
                        const auto inactive=MakeEffect(category,name,cumulative,true);
                        const float mix=std::clamp(inactive.mix.Sample(),0.f,1.f);
                        AddEvent(point.first,EventKey::NamedLaserFilterMix)->data.uintVal=(uint32(uint16(bus))<<16)|uint32(std::round(mix*1000));
                    }
                }
            }
        };
        automation(fxEffects,false);
        automation(laserEffects,true);
        std::stable_sort(m_audioEffectChanges.begin(),m_audioEffectChanges.end(),[](const auto& a,const auto& b){return a.time<b.time;});

        m_ksonScroll->Build();
        m_ksonBars.clear();
        m_ksonBarTimes.clear();
        for (auto sig=signatureAtPulse.begin(); sig!=signatureAtPulse.end(); ++sig) {
            const uint64_t stop=std::next(sig)==signatureAtPulse.end()?uint64_t(lastPulse)+1:std::next(sig)->first;
            const uint32 step=MeasurePulses(sig->second.first,sig->second.second);
            for(uint64_t y=sig->first;y<stop;y+=step) {
                if(m_ksonBars.size()>=1000000) throw std::runtime_error("Too many KSON measure lines");
                m_ksonBars.push_back(m_ksonScroll->PositionAtPulse(double(y)));
                m_ksonBarTimes.push_back(ExactTimeAt(uint32(y)));
            }
        }
        for (const auto& obj : m_objectStates) {
            if (obj->type != ObjectType::Hold && obj->type != ObjectType::Laser) continue;
            const auto times = GetVisualTimes(obj.get());
            auto& path = m_visualPaths[obj.get()];
            for (double t : m_ksonScroll->Times(times.first, times.second,
                    obj->type == ObjectType::Laser ? 8.0 : 0.0))
                path.emplace_back(t, m_ksonScroll->Position(t));
        }

        auto* end = new EventObjectState();
        end->time = RoundTime(double(TimeAt(lastPulse)) + 2000.0);
        end->key = EventKey::ChartEnd;
        end->interTickIndex = 0;
        m_objectStates.emplace_back(std::unique_ptr<ObjectState>(*end));
        ObjectState::SortArray(m_objectStates);
        return true;
    } catch (const std::exception& e) {
        Logf("[KSON] Parse failed: %s", Logger::Severity::Warning, e.what());
        return false;
    }
}

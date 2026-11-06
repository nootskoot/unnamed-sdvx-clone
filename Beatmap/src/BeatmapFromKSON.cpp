// KSON format_version=1 experimental native importer for USC.
// Stage 1: metadata, BPM, time signatures, stops, graph scroll speeds,
// BT/FX chips and holds, laser sections/slams. No custom audio or camera yet.
#include "stdafx.h"
#include "Beatmap.hpp"
#include "json.hpp"
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
                source.append(chunk, bytes);
        } else if (resource.IsValid() && resource.GetBytes()) {
            const auto& bytes = *resource.GetBytes();
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
        m_settings.audioNoFX = bgm.value("filename", std::string());
        m_settings.audioFX = "";
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
        auto TimeAt = [&](uint32 y) -> MapTime {
            const auto it = std::upper_bound(tempos.begin(), tempos.end(), y,
                [](uint32 p, const TempoStep& s) { return p < s.pulse; });
            const TempoStep& v = *(it == tempos.begin() ? tempos.begin() : it - 1);
            return RoundTime(double(m_settings.offset) + v.ms +
                double(y - v.pulse) * 60000.0 / (v.bpm * PULSES_PER_BEAT));
        };
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

        // KSON camera: use USC's existing graph consumers. The values use
        // the same /100 scaling as the KSH zoom and center_split settings.
        // Graph control points are preserved, although USC's LineGraph::ValueAt
        // currently interpolates linearly (curved rendering needs separate work).
        const J& camera = Member(root, "camera");
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
                        // The instantaneous manual->auto transition can't be
                        // accurately represented as one USC event: use its final auto state.
                        autoMode = value[1].get<std::string>();
                    } else continue;
                } else continue;

                TrackRollBehaviour behaviour = TrackRollBehaviour::Zero;
                if (manual) {
                    if (!std::isfinite(rotation.value.first) || !std::isfinite(rotation.value.second)) continue;
                    if (curved)
                        rotation.curve = {curve[0].get<double>(), curve[1].get<double>()};
                    m_effects.GetGraph(EffectTimeline::GraphType::ROTATION_Z).Insert(when, rotation);
                    behaviour = TrackRollBehaviour::Manual;
                    manualTiltActive = true;
                } else {
                    if (manualTiltActive)
                        m_effects.GetGraph(EffectTimeline::GraphType::ROTATION_Z).Extend(when);
                    manualTiltActive = false;
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
                        m_objectStates.emplace_back(std::unique_ptr<ObjectState>(*obj));
                    } else {
                        auto* obj = new ButtonObjectState();
                        obj->time = TimeAt(y);
                        obj->index = uint8(baseIndex + i);
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
        // KSON durations are 240 pulses/beat; USC SpinStruct uses 192/whole note.
        // Camera::Tick completes Full/Quarter spins at progress 2, so SetSpin needs half of the requested KSON duration.
        // KSON pulse-to-legacy unit ratio is 5:1; divide by 10 overall.
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
                    const uint32 uscDuration = static_cast<uint32>(
                        std::max(1.0, std::round(static_cast<double>(length) / 10.0)));
                    spinAtPulse[{pulse, direction}] = {type, uscDuration};
                } catch (const std::exception&) {
                    // Bad optional camera data must not reject a playable chart.
                }
            }
        };
        ReadSpinPattern("spin", SpinStruct::SpinType::Full);
        ReadSpinPattern("half_spin", SpinStruct::SpinType::Quarter);

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
                    obj->duration = instantaneous ? 0 : std::max(MapTime(0), MapTime(TimeAt(y1) - obj->time));
                    obj->points[0] = float(p0);
                    obj->points[1] = float(p1);
                    if (extended) obj->flags |= LaserObjectState::flag_Extended;
                    if (instantaneous) obj->flags |= LaserObjectState::flag_Instant;
                    // A KSON camera pattern applies only to an actual same-pulse,
                    // same-direction laser slam; other laser events are unaffected.
                    if (instantaneous) {
                        const int direction = p1 > p0 ? 1 : (p1 < p0 ? -1 : 0);
                        const auto spin = spinAtPulse.find({y0, direction});
                        if (spin != spinAtPulse.end()) {
                            obj->spin.type = spin->second.first;
                            obj->spin.duration = spin->second.second;
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
                    lastPulse = std::max(lastPulse, y);
                    if (!first && y > prevY) {
                        const MapTime elapsed = TimeAt(y) - TimeAt(prevY);
                        const bool curved = prevCurve.first != 0.0 || prevCurve.second != 0.0;
                        if (!curved || values.first == prevAfter || elapsed <= 16) {
                            addSegment(prevY, y, prevAfter, values.first, false);
                        } else {
                            // Approximate a KSON quadratic curve with USC's existing
                            // straight laser meshes, while preserving the exact end ticks.
                            // Avoid sub-millisecond segments at high chart BPM.
                            const uint32 count = std::min<uint32>(
                                12, std::min<uint32>(y - prevY, uint32(elapsed / 8)));
                            uint32 subStart = prevY;
                            double subValue = prevAfter;
                            for (uint32 i = 1; i <= count; ++i) {
                                const uint32 subEnd = prevY + uint32(
                                    (uint64_t(y - prevY) * i) / count);
                                const double fraction = double(subEnd - prevY) / double(y - prevY);
                                const double eased = KsonCurveAt(prevCurve.first, prevCurve.second, fraction);
                                const double nextValue = prevAfter + (values.first - prevAfter) * eased;
                                if (subEnd > subStart)
                                    addSegment(subStart, subEnd, subValue, nextValue, false);
                                subStart = subEnd;
                                subValue = nextValue;
                            }
                        }
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
            scroll.Insert(TimeAt(y), p);
            lastPulse = std::max(lastPulse, y);
        }
        for (const J& stop : Items(beat, "stop")) {
            if (!stop.is_array() || stop.size() != 2) continue;
            const uint32 y = GetPulse(stop[0]), len = GetPulse(stop[1]);
            if (uint64_t(y) + len > std::numeric_limits<uint32>::max())
                throw std::runtime_error("KSON stop tick overflow");
            lastPulse = std::max(lastPulse, uint32(y + len));
            if (len && TimeAt(y + len) > TimeAt(y))
                scroll.RangeSet(TimeAt(y), TimeAt(y + len), 0.0);
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

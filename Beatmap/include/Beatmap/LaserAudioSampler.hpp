#pragma once
#include "Beatmap.hpp"
#include <algorithm>

// Immutable chart-position index. Judgment gating remains a current-time input.
class LaserAudioSampler
{
    struct Segment { double start, end; float a, b; };
    std::vector<Segment> m_lanes[2];
public:
    void Build(const Beatmap& map)
    {
        for(auto& lane:m_lanes) lane.clear();
        for(const auto& object:map.GetObjectStates()) {
            if(object->type!=ObjectType::Laser) continue;
            const auto* laser=reinterpret_cast<const LaserObjectState*>(object.get());
            const auto time=map.GetVisualTimes(object.get());
            if(laser->index>1 || time.second<=time.first) continue;
            float a=laser->points[0],b=laser->points[1];
            if(laser->flags & LaserObjectState::flag_Extended) { a=(a+.5f)*.5f; b=(b+.5f)*.5f; }
            m_lanes[laser->index].push_back({time.first,time.second,a,b});
        }
        for(auto& lane:m_lanes) std::stable_sort(lane.begin(),lane.end(),[](const Segment&a,const Segment&b){return a.start<b.start;});
    }
    bool Sample(double time, unsigned eligibleLanes, float& value) const
    {
        value=0; bool active=false;
        for(unsigned i=0;i<2;++i) {
            if(!(eligibleLanes & (1u<<i))) continue;
            const auto& lane=m_lanes[i];
            auto it=std::upper_bound(lane.begin(),lane.end(),time,[](double t,const Segment&s){return t<s.start;});
            if(it==lane.begin()) continue;
            --it;
            if(time>=it->end) continue;
            float v=it->a+(it->b-it->a)*float((time-it->start)/(it->end-it->start));
            if(i==1) v=1-v;
            value=std::max(value,v); active=true;
        }
        return active;
    }
};

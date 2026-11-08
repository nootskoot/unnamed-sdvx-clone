#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

// Native KSON visual coordinates. Gameplay judgements remain in MapTime;
// authored scroll boundaries and visual note anchors retain fractional ms.
class KSONScroll
{
public:
    struct Tempo { double pulse, bpm, ms; };
    struct Node { double before = 1, after = 1, a = 0, b = 0; };
    std::vector<Tempo> tempos;
    std::map<double, Node> graph;
    std::map<double, double> stops; // start pulse -> length

    double Time(double pulse) const
    {
        auto it = std::upper_bound(tempos.begin(), tempos.end(), pulse,
            [](double p, const Tempo& t) { return p < t.pulse; });
        const auto& t = *(it == tempos.begin() ? it : std::prev(it));
        return t.ms + (pulse - t.pulse) * 60000.0 / (240.0 * t.bpm);
    }
    double Pulse(double ms) const
    {
        auto it = std::upper_bound(tempos.begin(), tempos.end(), ms,
            [](double m, const Tempo& t) { return m < t.ms; });
        const auto& t = *(it == tempos.begin() ? it : std::prev(it));
        return t.pulse + (ms - t.ms) * (240.0 * t.bpm) / 60000.0;
    }
    static double CurveTime(double a, double x)
    {
        x = std::clamp(x, 0.0, 1.0);
        const double d = 2*a + std::sqrt(std::max(0.0, 4*a*a + 4*(1-2*a)*x));
        return d > 1e-12 ? 2*x/d : std::sqrt(x);
    }
    static double Curve(double a, double b, double x)
    {
        const double t = CurveTime(a, x);
        return 2*b*t*(1-t) + t*t;
    }
    static double Area(double a, double b, double x)
    {
        const double t = CurveTime(a, x), t2 = t*t;
        return 2*a*b*t2 + (4*b*(1-2*a)+2*a*(1-2*b))*t2*t/3
            + (1-2*a)*(1-2*b)*t2*t2/2;
    }
    void Build()
    {
        if (graph.empty()) graph.emplace(0, Node{});
        // Merge overlapping stops rather than restoring speed within a stop.
        std::map<double, double> merged;
        for (const auto& s : stops) {
            if (s.second <= 0) continue;
            if (!merged.empty() && s.first <= merged.rbegin()->second)
                merged.rbegin()->second = std::max(merged.rbegin()->second, s.first+s.second);
            else merged[s.first] = s.first+s.second;
        }
        std::vector<double> cuts;
        for (const auto& n : graph) cuts.push_back(n.first);
        for (const auto& t : tempos) cuts.push_back(t.pulse);
        for (const auto& s : merged) { cuts.push_back(s.first); cuts.push_back(s.second); }
        // Speed is monotonic within each valid quadratic graph interval.
        // Its zero is the only possible interior extremum of scroll position.
        for (auto it = graph.begin(); std::next(it) != graph.end(); ++it) {
            const auto next = std::next(it);
            const double v0 = it->second.after, v1 = next->second.before;
            if (v0*v1 < 0) {
                double lo=0, hi=1;
                for (int i=0; i<60; ++i) {
                    const double mid=(lo+hi)/2;
                    const double v=v0+(v1-v0)*Curve(it->second.a,it->second.b,mid);
                    if ((v>0)==(v0>0)) lo=mid; else hi=mid;
                }
                cuts.push_back(it->first+(next->first-it->first)*(lo+hi)/2);
            }
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        pieces.clear();
        double prefix=0;
        for (size_t i=0; i<cuts.size(); ++i) {
            const double p=cuts[i];
            Piece s{}; s.begin=p; s.prefix=prefix;
            auto next=graph.upper_bound(p);
            if (next==graph.begin()) s.v0=s.v1=next->second.before;
            else if (next==graph.end()) s.v0=s.v1=std::prev(next)->second.after;
            else {
                const auto prev=std::prev(next);
                s.origin=prev->first; s.span=next->first-prev->first;
                s.v0=prev->second.after; s.v1=next->second.before;
                s.a=prev->second.a; s.b=prev->second.b;
            }
            auto stop=merged.upper_bound(p);
            if (stop!=merged.begin() && p<std::prev(stop)->second) s.v0=s.v1=0;
            pieces.push_back(s);
            if (i+1<cuts.size()) prefix+=Integral(s,cuts[i+1])/240.0;
        }
    }
    double Speed(double ms) const
    {
        const double p=Pulse(ms);
        auto it=std::upper_bound(pieces.begin(),pieces.end(),p,
            [](double v,const Piece& s){return v<s.begin;});
        if(it==pieces.begin()) return graph.begin()->second.before;
        --it;
        return it->v0+(it->v1-it->v0)*Curve(it->a,it->b,(p-it->origin)/it->span);
    }
    double Position(double ms) const { return PositionAtPulse(Pulse(ms)); }
    double PositionAtPulse(double p) const
    {
        auto it=std::upper_bound(pieces.begin(),pieces.end(),p,
            [](double v,const Piece& s){return v<s.begin;});
        if (it==pieces.begin()) return (p-pieces.front().begin)*graph.begin()->second.before/240.0;
        --it;
        return it->prefix+Integral(*it,p)/240.0;
    }
    // All graph/tempo/stop boundaries and position extrema inside a note.
    std::vector<double> Times(double begin, double end, double stepMs=0) const
    {
        std::vector<double> out{begin};
        const double p0=Pulse(begin), p1=Pulse(end);
        auto it=std::upper_bound(pieces.begin(),pieces.end(),p0,
            [](double p,const Piece& s){return p<s.begin;});
        for (;it!=pieces.end() && it->begin<p1;++it) out.push_back(Time(it->begin));
        out.push_back(end);
        if (stepMs>0) {
            const auto cuts=out; out.clear(); out.push_back(begin);
            for(size_t i=1;i<cuts.size();++i) {
                const int n=int(std::clamp(std::ceil((cuts[i]-cuts[i-1])/stepMs),1.0,4096.0));
                for(int j=1;j<=n;++j) out.push_back(cuts[i-1]+(cuts[i]-cuts[i-1])*j/n);
            }
        }
        return out;
    }
    std::pair<double,double> Bounds(double begin,double end) const
    {
        double lo=Position(begin), hi=lo;
        for(double t:Times(begin,end)) { const double p=Position(t); lo=std::min(lo,p); hi=std::max(hi,p); }
        return {lo,hi};
    }
private:
    struct Piece { double begin=0,prefix=0,origin=0,span=1,v0=1,v1=1,a=0,b=0; };
    std::vector<Piece> pieces;
    static double Integral(const Piece& s,double end)
    {
        if(s.v0==s.v1) return (end-s.begin)*s.v0;
        const double x0=(s.begin-s.origin)/s.span,x1=(end-s.origin)/s.span;
        return (end-s.begin)*s.v0+(s.v1-s.v0)*s.span*(Area(s.a,s.b,x1)-Area(s.a,s.b,x0));
    }
};

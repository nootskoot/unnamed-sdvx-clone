/// From https://github.com/m4saka/ksh

#include "stdafx.h"
#include "LineGraph.hpp"
#include <algorithm>
#include <cmath>

// Quadratic KSON graph interpolation: one control point (a,b).
// For graph points without an authored curve, {0,0} reduces to y=x.
namespace {
double KsonGraphCurveTime(double a, double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const double discr = std::max(0.0, 4.0 * a * a + 4.0 * (1.0 - 2.0 * a) * x);
    const double den = 2.0 * a + std::sqrt(discr);
    return den > 1e-12 ? 2.0 * x / den : std::sqrt(x);
}
double KsonGraphCurveAreaTo(double a, double b, double x) {
    const double t = KsonGraphCurveTime(a, x);
    const double t2 = t * t;
    return 2.0 * a * b * t2 +
        (4.0 * b * (1.0 - 2.0 * a) + 2.0 * a * (1.0 - 2.0 * b)) * t2 * t / 3.0 +
        (1.0 - 2.0 * a) * (1.0 - 2.0 * b) * t2 * t2 / 2.0;
}
double KsonGraphCurveValue(double a, double b, double x) {
    const double t = KsonGraphCurveTime(a, x);
    return 2.0 * b * t * (1.0 - t) + t * t;
}
} // namespace


void LineGraph::Insert(MapTime mapTime, double point)
{
    auto it = m_points.find(mapTime);
    if (it == m_points.end())
    {
        m_points.emplace_hint(it, mapTime, Point{point});
    }
    else
    {
        it->second.value.second = point;
    }
}

void LineGraph::Insert(MapTime mapTime, const LineGraph::Point& point)
{
    auto it = m_points.find(mapTime);
    if (it == m_points.end())
    {
        m_points.emplace_hint(it, mapTime, point);
    }
    else
    {
        it->second.value.second = point.value.second;
    }
}

void LineGraph::Insert(MapTime mapTime, const std::string& point)
{
    const std::size_t semicolonIdx = point.find(';');
    if (semicolonIdx == std::string::npos)
    {
        try
        {
            Insert(mapTime, std::stod(point));
        }
        catch (const std::invalid_argument&) {}
        catch (const std::out_of_range&) {}
    }
    else
    {
        Insert(mapTime, LineGraph::Point{std::stod(point.substr(semicolonIdx + 1)), std::stod(point.substr(semicolonIdx + 1))});
    }
}

void LineGraph::RangeSet(MapTime begin, MapTime end, double value)
{
    if (begin >= end) return;

    const double beginValue = ValueAt(begin);
    const double endValue = ValueAt(end);

    const auto beginIt = m_points.lower_bound(begin);
    const auto endIt = m_points.upper_bound(end);

    for (auto it = beginIt; it != endIt; it = m_points.erase(it));

    Insert(begin, LineGraph::Point{beginValue, value});
    Insert(end, LineGraph::Point{value, endValue});
}

void LineGraph::RangeAdd(MapTime begin, MapTime end, double delta)
{
    if (begin >= end) return;

    const double beginValue = ValueAt(begin);
    const double endValue = ValueAt(end);

    const auto beginIt = m_points.upper_bound(begin);
    const auto endIt = m_points.lower_bound(end);

    for (auto it = beginIt; it != endIt; ++it)
    {
        it->second.value.first += delta;
        it->second.value.second += delta;
    }

    Insert(begin, LineGraph::Point{beginValue, beginValue + delta});

    if (endIt != m_points.end() && endIt->first == end)
    {
        endIt->second.value.first += delta;
    } else
    {
        Insert(end, LineGraph::Point{endValue + delta, endValue});
    }
}

double LineGraph::Extend(MapTime time)
{
    if (m_points.empty())
    {
        Insert(time, m_default);
        return m_default;
    }

    auto it = m_points.upper_bound(time);

    if (it == m_points.begin())
    {
        return it->second.value.first;
    }

    it = std::prev(it);

    if (it->first == time)
    {
        return it->second.value.first;
    }

    const double value = it->second.value.second;
    Insert(time, value);

    return value;
}

double LineGraph::Integrate(MapTime begin, MapTime end) const
{
    int sign = 1;

    if (begin == end)
    {
        return 0.0;
    }

    if (m_points.size() == 0)
    {
        return (end - begin) * m_default;
    }

    if (end < begin)
    {
        std::swap(begin, end);
        sign = -1;
    }

    // Integration range is after the last point
    auto beginIt = m_points.upper_bound(begin);
    if (beginIt == m_points.end())
    {
        return sign * m_points.rbegin()->second.value.second * (end - begin);
    }

    if (end <= beginIt->first)
    {
        if (beginIt == m_points.begin())
        {
            // Integration range is before the first point
            return sign * beginIt->second.value.first * (end - begin);
        }
        else
        {
            // Integration range contained in a single segment
            return sign * Integrate(std::prev(beginIt), begin, end);
        }
    }

    double result = 0.0;

    // Ensure that the beginning of the integration range is beginIt
    if (beginIt == m_points.begin())
    {
        result = beginIt->second.value.first * (beginIt->first - begin);
    }
    else if (begin != beginIt->first)
    {
        auto beginPrev = std::prev(beginIt);
        result = Integrate(beginPrev, begin, beginIt->first);
    }

    auto endIt = m_points.upper_bound(end);
    if (endIt == m_points.begin())
    {
        // This means that end < m_points.begin()->first
        // But then, since begin < end, begin < m_points.begin()->first so beginIt == m_points.begin()
        // Therefore end < beginIt->first and this case is already handled
        // But let's check for this case just to be sure
        assert(false);
        return sign * endIt->second.value.first * (end - begin);
    }

    endIt = std::prev(endIt);

    // Ensure that the end of the integration range is endIt
    if (endIt->first != end)
    {
        result += Integrate(endIt, endIt->first, end);
    }

    // Integrate the remaining part
    for (; beginIt != endIt; ++beginIt)
    {
        result += Integrate(beginIt);
    }

    return sign * result;
}

double LineGraph::Integrate(PointsIterator curr, MapTime begin, MapTime end) const
{
    int sign = 1;

    if (begin == end)
    {
        return 0.0;
    }

    if (end < begin)
    {
        std::swap(begin, end);
        sign = -1;
    }

    if (m_points.empty())
    {
        return sign * (end - begin) * m_default;
    }

    if (curr == m_points.end())
    {
        return sign * m_points.rbegin()->second.value.second * (end - begin);
    }

    assert(curr->first <= begin);

    auto next = std::next(curr);
    if (next == m_points.end())
    {
        return sign * curr->second.value.second * (end - begin);
    }

    assert(end <= next->first);

        const double span = static_cast<double>(next->first - curr->first);
        const double x0 = (begin - curr->first) / span;
        const double x1 = (end - curr->first) / span;
        const double first = curr->second.value.second;
        const double last = next->second.value.first;
        const auto& curve = curr->second.curve;
        // Analytic integral of y(t) * dx/dt, preserving signed scroll speed.
        const double area = KsonGraphCurveAreaTo(curve.first, curve.second, x1) -
                            KsonGraphCurveAreaTo(curve.first, curve.second, x0);
        return sign * span * (first * (x1 - x0) + (last - first) * area);
}

double LineGraph::Integrate(PointsIterator curr) const
{
    if (m_points.empty() || curr == m_points.end())
    {
        return 0.0;
    }

    auto next = std::next(curr);
    if (next == m_points.end())
    {
        assert(false);
        return 0.0;
    }

        const double first = curr->second.value.second;
        const double last = next->second.value.first;
        const auto& curve = curr->second.curve;
        return static_cast<double>(next->first - curr->first) *
            (first + (last - first) * KsonGraphCurveAreaTo(curve.first, curve.second, 1.0));
}

double LineGraph::ValueAt(MapTime mapTime) const
{
    if (m_points.empty())
    {
        return m_default;
    }

    const auto secondItr = m_points.upper_bound(mapTime);
    if (secondItr == m_points.begin())
    {
        // Before the first plot
        return secondItr->second.value.first;
    }

    const auto firstItr = std::prev(secondItr);
    const double firstValue = (*firstItr).second.value.second;

    if (secondItr == m_points.end())
    {
        // After the last plot
        return firstValue;
    }

    const double secondValue = secondItr->second.value.first;
    const MapTime firstTime = firstItr->first;
    const MapTime secondTime = secondItr->first;

    // Erratic case
    if (firstTime == secondTime)
    {
        return secondValue;
    }

        const double x = (mapTime - firstTime) / static_cast<double>(secondTime - firstTime);
        const auto& curve = firstItr->second.curve;
        return Math::Lerp(firstValue, secondValue, KsonGraphCurveValue(curve.first, curve.second, x));
}
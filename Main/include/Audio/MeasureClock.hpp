#pragma once

/*
	Measure lines for the gate clock.

	GateDSP can follow the chart instead of free-running from the moment its object
	was created, but it needs a sorted list of measure-line times in map
	milliseconds to do it. KSON charts already ship one (Beatmap::GetKSONBarTimes).
	KSH v1 charts do not: their measure lines have to be rebuilt from the timing
	map, because a KSH timing point carries the bar duration (beat duration and time
	signature) and nothing else.

	The result is what a KSH v1 gate needs to land its open window on the chart's
	grid, exactly as a KSON gate already does, instead of on whichever audio frame
	happened to construct the DSP object.

	Times are map times (the same units as TimingPoint::time), which are positions in
	the music file. They are compared against the stream position directly; the
	player's calibration offset is not applied.
*/
#include <cmath>
#include <cstddef>
#include <vector>

namespace MeasureClock
{
	// Same ceiling the KSON parser uses for its bar list, so a broken chart cannot
	// make this allocate without bound.
	static constexpr std::size_t kMaxMeasureLines = 1000000U;

	// A bar shorter than this is not a bar; skip such a timing point rather than
	// emitting a line every few microseconds.
	static constexpr double kMinBarDurationMs = 0.5;

	/*
		Builds the measure-line times covered by a timing map.

		TimingPointsT needs: size(), operator[](std::size_t) yielding something with
		`MapTime time` and `double GetBarDuration()`.

		`endTimeMs` should be the last object's map time; one extra bar is emitted
		after it so a gate that outlives the last note still has an anchor.
	*/
	template <typename TimingPointsT>
	std::vector<double> BuildMeasureTimes(const TimingPointsT &timingPoints, double endTimeMs,
		std::size_t maxLines = kMaxMeasureLines)
	{
		std::vector<double> times;
		const std::size_t count = timingPoints.size();
		if (!count || maxLines == 0)
			return times;

		times.reserve(count * 8);

		// Last emitted line, used to keep the list strictly increasing when a new
		// timing point starts off the previous bar grid.
		bool havePrevious = false;
		double previous = 0.0;

		for (std::size_t i = 0; i < count; ++i)
		{
			const auto &point = timingPoints[i];
			const double bar = point.GetBarDuration();
			if (!std::isfinite(bar) || bar < kMinBarDurationMs)
				continue;

			const double start = static_cast<double>(point.time);
			double stop;
			if (i + 1 < count)
			{
				stop = static_cast<double>(timingPoints[i + 1].time);
			}
			else
			{
				// Final section: run past the last object so the gate keeps an anchor.
				stop = std::max(start, endTimeMs) + bar;
			}

			for (double t = start; t < stop; t += bar)
			{
				if (havePrevious && t <= previous)
					continue;
				if (times.size() >= maxLines)
					return times;
				times.push_back(t);
				previous = t;
				havePrevious = true;
			}
		}

		return times;
	}
}

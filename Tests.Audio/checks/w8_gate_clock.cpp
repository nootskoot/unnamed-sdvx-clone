// w8 verification: build KSH v1 measure lines from a timing map and check that a
// GateDSP following them lands its open window on the chart grid, instead of on the
// frame that happened to construct it.
//
// Compiled against the clone's real MeasureClock.hpp and the real GateDSP out of
// Audio/src/DSP.cpp. Nothing here reimplements the gate.
#include "stdafx.h"
#include "AudioBase.hpp"
#include "DSP.hpp"
#include "Audio/MeasureClock.hpp"

#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>

static int g_failures = 0;
static void Check(bool ok, const char *what, const std::string &detail = {})
{
	std::printf("%s %s%s%s\n", ok ? "  ok  " : "  FAIL", what,
		detail.empty() ? "" : " :: ", detail.c_str());
	if (!ok) ++g_failures;
}

// Minimal chart timing point, same shape as BeatmapObjects.hpp's TimingPoint for the
// two members the builder reads.
struct TestTimingPoint
{
	int32 time = 0;
	double beatDuration = 300.0; // 200 BPM
	uint8 numerator = 4;
	uint8 denominator = 4;
	double GetBarDuration() const { return beatDuration * 4.0 * (double(numerator) / double(denominator)); }
};

struct TestTimingPoints
{
	std::vector<TestTimingPoint> points;
	std::size_t size() const { return points.size(); }
	const TestTimingPoint &operator[](std::size_t i) const { return points[i]; }
};

class TestAudioBase : public AudioBase
{
public:
	uint32 rate = 44100;
	void Process(float *, uint32) override {}
	int32 GetPosition() const override { return int32(blockStartSample * 1000ULL / rate); }
	uint32 GetSampleRate() const override { return rate; }
	uint64 GetSamplePos() const override { return blockStartSample; }
	float *GetPCM() override { return nullptr; }
	uint64 GetPCMCount() const override { return 0; }
	void PreRenderDSPs(Vector<DSP *> &) override {}
};

struct Rises
{
	std::vector<double> riseMs;
	std::vector<double> fallMs;
};

// Runs a GateDSP over [startMs, endMs), feeding DC 1.0, and records where its output
// rises (open window start) and falls (open window end).
static Rises RunGate(GateDSP &gate, TestAudioBase &base, double sampleMs, double startMs, double endMs)
{
	Rises out;
	const uint32 block = 384;
	const uint32 rate = base.rate;
	const uint64 firstFrame = uint64(startMs * rate / 1000.0);
	const uint64 lastFrame = uint64(endMs * rate / 1000.0);

	std::vector<float> buffer(block * 2);
	bool previous = false;

	for (uint64 frame = firstFrame; frame < lastFrame; frame += block)
	{
		base.blockStartSample = frame;
		std::fill(buffer.begin(), buffer.end(), 1.0f);
		gate.Process(buffer.data(), block);
		for (uint32 i = 0; i < block; ++i)
		{
			const double t = double(frame + i) * 1000.0 / rate;
			// Before the DSP's start time the gate is not applied at all; that is not a
			// window edge, so skip it.
			if (t < sampleMs) continue;
			const bool open = buffer[i * 2] > 0.5f;
			if (open && !previous) out.riseMs.push_back(t);
			if (!open && previous) out.fallMs.push_back(t);
			previous = open;
		}
	}
	return out;
}

int main()
{
	// Chart shape taken from the attached Cherish charts: 200 BPM, 4/4, o=69.
	const double bpm = 200.0;
	const double beatMs = 60000.0 / bpm;
	const double barMs = beatMs * 4.0; // 1200 ms
	const double offsetMs = 69.0;
	const int measures = 80;

	TestTimingPoints timing;
	{
		TestTimingPoint p;
		p.time = int32(offsetMs);
		p.beatDuration = beatMs;
		p.numerator = 4;
		p.denominator = 4;
		timing.points.push_back(p);
	}

	std::printf("\n[1] BuildMeasureTimes on a 200 BPM 4/4 KSH v1 timing map\n");
	{
		const auto times = MeasureClock::BuildMeasureTimes(timing, offsetMs + measures * barMs);
		Check(!times.empty(), "produces measure lines");
		Check(times.size() >= std::size_t(measures), "covers the chart",
			"lines=" + std::to_string(times.size()));
		Check(std::abs(times.front() - offsetMs) < 1e-9, "first line is the map offset",
			"first=" + std::to_string(times.front()));
		double worstSpacing = 0.0;
		bool mono = true;
		for (std::size_t i = 1; i < times.size(); ++i)
		{
			if (times[i] <= times[i - 1]) mono = false;
			worstSpacing = std::max(worstSpacing, std::abs((times[i] - times[i - 1]) - barMs));
		}
		Check(mono, "strictly increasing");
		Check(worstSpacing < 1e-6, "spacing equals the bar duration",
			"worst error ms=" + std::to_string(worstSpacing));
	}

	std::printf("\n[2] BPM and signature changes\n");
	{
		TestTimingPoints changed = timing;
		TestTimingPoint fast;
		fast.time = int32(offsetMs + 40 * barMs);
		fast.beatDuration = 60000.0 / 150.0; // 180 -> 150 BPM
		fast.numerator = 3;
		fast.denominator = 4;
		changed.points.push_back(fast);
		const auto times = MeasureClock::BuildMeasureTimes(changed, offsetMs + measures * barMs);
		bool mono = true;
		for (std::size_t i = 1; i < times.size(); ++i)
			if (times[i] <= times[i - 1]) mono = false;
		Check(mono, "still strictly increasing across the change");
		// New bar = 400 ms/beat * 4 * 3/4 = 1200 ms -> 1200 ms. Pick a different signature
		// to make the change visible: 3/4 at 150 BPM = 400*3 = 1200 ms, same as before, so
		// use 5/4 to get 2000 ms.
		TestTimingPoints wide = changed;
		wide.points[1].numerator = 5;
		const auto wideTimes = MeasureClock::BuildMeasureTimes(wide, offsetMs + measures * barMs);
		double spacingAfter = 0.0;
		for (std::size_t i = 1; i < wideTimes.size(); ++i)
			if (wideTimes[i] > offsetMs + 41 * barMs) { spacingAfter = wideTimes[i] - wideTimes[i - 1]; break; }
		Check(std::abs(spacingAfter - 2000.0) < 1e-6, "5/4 at 150 BPM gives 2000 ms bars",
			"spacing=" + std::to_string(spacingAfter));
	}

	std::printf("\n[3] Degenerate timing maps\n");
	{
		TestTimingPoints bad = timing;
		TestTimingPoint zero;
		zero.time = int32(offsetMs + barMs);
		zero.beatDuration = 0.0;
		bad.points.insert(bad.points.begin() + 1, zero);
		const auto times = MeasureClock::BuildMeasureTimes(bad, offsetMs + 4 * barMs);
		Check(!times.empty(), "a zero-length bar is skipped, not looped forever",
			"lines=" + std::to_string(times.size()));

		const auto empty = MeasureClock::BuildMeasureTimes(TestTimingPoints{}, 1000.0);
		Check(empty.empty(), "empty timing map gives no lines");

		TestTimingPoints tiny = timing;
		tiny.points[0].beatDuration = 0.6; // 0.6 ms bars
		const auto capped = MeasureClock::BuildMeasureTimes(tiny, 1e9, 500);
		Check(capped.size() <= 500, "respects the line cap",
			"lines=" + std::to_string(capped.size()));
	}

	std::printf("\n[4] Gate;16 at 50%% on a 200 BPM chart: grid alignment\n");
	{
		const auto measureTimes = MeasureClock::BuildMeasureTimes(timing, offsetMs + measures * barMs);
		const double gateMs = barMs / 16.0; // 75 ms: one 1/16 note
		const double pulseMs = offsetMs + 51 * barMs + 15 * (barMs / 16.0); // Cherish m051.15
		const double jitterMs = 9.0; // the DSP object is built this long after the pulse
		const double constructMs = pulseMs - 3.0; // and its first block starts here
		const double holdEndMs = pulseMs + 4 * barMs;

		std::vector<double> grid;
		for (double t = offsetMs + 51 * barMs; t < holdEndMs + gateMs; t += gateMs) grid.push_back(t);

		auto measureOffset = [&](const Rises &r, const char *label) {
			double worst = 0.0;
			double sum = 0.0;
			int n = 0;
			int insideOpenWindow = 0;
			for (double g : grid)
			{
				// A window that opens before the DSP object exists cannot sound; compare
				// only the grid lines the effect is actually alive for.
				if (g < pulseMs + jitterMs || g >= holdEndMs) continue;
				// nearest rise at or after this grid line, within half a period
				auto it = std::lower_bound(r.riseMs.begin(), r.riseMs.end(), g - gateMs * 0.5);
				if (it == r.riseMs.end() || std::abs(*it - g) > gateMs * 0.5) { ++insideOpenWindow; continue; }
				const double d = std::abs(*it - g);
				worst = std::max(worst, d);
				sum += d;
				++n;
			}
			std::printf("      %-14s windows=%2d  mean offset=%6.2f ms  worst=%6.2f ms  missing=%d\n",
				label, n, n ? sum / n : 0.0, worst, insideOpenWindow);
			return worst;
		};

		// Today's behaviour: no measure times, phase free-runs from construction.
		TestAudioBase baseA;
		GateDSP freeRun(44100);
		freeRun.SetAudioBase(&baseA);
		freeRun.SetLength(gateMs);
		freeRun.SetGating(0.5f);
		freeRun.startTime = uint32(pulseMs + jitterMs);
		baseA.blockStartSample = uint64(constructMs * baseA.rate / 1000.0);
		const double freeWorst = measureOffset(
			RunGate(freeRun, baseA, pulseMs + jitterMs, constructMs, holdEndMs), "free-run");
		freeRun.RemoveAudioBase();

		// With the measure clock, same construction time.
		TestAudioBase baseB;
		GateDSP chartClock(44100);
		chartClock.SetAudioBase(&baseB);
		chartClock.SetLength(gateMs);
		chartClock.SetGating(0.5f);
		chartClock.SetMeasureTimes(measureTimes);
		chartClock.startTime = uint32(pulseMs + jitterMs);
		baseB.blockStartSample = uint64(constructMs * baseB.rate / 1000.0);
		const double clockWorst = measureOffset(
			RunGate(chartClock, baseB, pulseMs + jitterMs, constructMs, holdEndMs), "chart clock");
		chartClock.RemoveAudioBase();

		Check(freeWorst > 5.0, "free-run is measurably off the grid",
			"worst=" + std::to_string(freeWorst) + " ms");
		Check(clockWorst < 0.05, "chart clock lands on the grid",
			"worst=" + std::to_string(clockWorst) + " ms");
	}

	std::printf("\n[5] Open-window length is unchanged by the clock\n");
	{
		const auto measureTimes = MeasureClock::BuildMeasureTimes(timing, offsetMs + measures * barMs);
		const double gateMs = barMs / 16.0;
		const double pulseMs = offsetMs + 51 * barMs + 15 * (barMs / 16.0);

		TestAudioBase base;
		GateDSP gate(44100);
		gate.SetAudioBase(&base);
		gate.SetLength(gateMs);
		gate.SetGating(0.5f);
		gate.SetMeasureTimes(measureTimes);
		gate.startTime = uint32(pulseMs);
		base.blockStartSample = uint64(pulseMs * base.rate / 1000.0);
		const Rises r = RunGate(gate, base, pulseMs, pulseMs, pulseMs + barMs);
		gate.RemoveAudioBase();

		Check(!r.riseMs.empty() && !r.fallMs.empty(), "gate opens and closes",
			"rises=" + std::to_string(r.riseMs.size()) + " falls=" + std::to_string(r.fallMs.size()));
		// Gating 0.5 of a 75 ms period = 37.5 ms open.
		const double openMs = r.fallMs.front() - r.riseMs.front();
		Check(std::abs(openMs - gateMs * 0.5) < 1.0, "open window stays half the period",
			"open=" + std::to_string(openMs) + " ms");
		Check(std::abs(r.riseMs.front() - pulseMs) < 1.0, "first window starts on the pulse",
			"first rise=" + std::to_string(r.riseMs.front()) + " pulse=" + std::to_string(pulseMs));
	}

	std::printf("\n[6] Cherish's real tempo: 180 BPM, 4/4, o=69, Gate;16\n");
	{
		// Cherish_ex.ksh: t=180, beat=4/4, o=69, ver=160 (KSH v1), single tempo.
		const double chartBpm = 180.0;
		const double chartBeatMs = 60000.0 / chartBpm;   // 333.33 ms
		const double chartBarMs = chartBeatMs * 4.0;     // 1333.33 ms
		const double chartGateMs = chartBarMs / 16.0;    // 83.33 ms: one 1/16 note
		TestTimingPoints chartTiming;
		{
			TestTimingPoint p;
			p.time = 69;
			p.beatDuration = chartBeatMs;
			chartTiming.points.push_back(p);
		}
		const auto measureTimes = MeasureClock::BuildMeasureTimes(chartTiming, 69.0 + 100 * chartBarMs);
		const double pulseMs = 69.0 + 51 * chartBarMs; // a measure-line pulse
		const double jitterMs = 9.0;
		const double constructMs = pulseMs - 3.0;
		const double holdEndMs = pulseMs + 4 * chartBarMs;

		std::vector<double> grid;
		for (double t = pulseMs; t < holdEndMs + chartGateMs; t += chartGateMs) grid.push_back(t);

		auto offsetOf = [&](const Rises &r) {
			double worst = 0.0, sum = 0.0;
			int n = 0;
			for (double g : grid)
			{
				if (g < pulseMs + jitterMs || g >= holdEndMs) continue;
				auto it = std::lower_bound(r.riseMs.begin(), r.riseMs.end(), g - chartGateMs * 0.5);
				if (it == r.riseMs.end() || std::abs(*it - g) > chartGateMs * 0.5) continue;
				const double d = std::abs(*it - g);
				worst = std::max(worst, d);
				sum += d;
				++n;
			}
			return std::pair<double, double>{n ? sum / n : 0.0, worst};
		};

		TestAudioBase freeBase;
		GateDSP freeGate(44100);
		freeGate.SetAudioBase(&freeBase);
		freeGate.SetLength(chartGateMs);
		freeGate.SetGating(0.5f);
		freeGate.startTime = uint32(pulseMs + jitterMs);
		freeBase.blockStartSample = uint64(constructMs * freeBase.rate / 1000.0);
		const auto freeOff = offsetOf(RunGate(freeGate, freeBase, pulseMs + jitterMs, constructMs, holdEndMs));
		freeGate.RemoveAudioBase();

		TestAudioBase clockBase;
		GateDSP clockGate(44100);
		clockGate.SetAudioBase(&clockBase);
		clockGate.SetLength(chartGateMs);
		clockGate.SetGating(0.5f);
		clockGate.SetMeasureTimes(measureTimes);
		clockGate.startTime = uint32(pulseMs + jitterMs);
		clockBase.blockStartSample = uint64(constructMs * clockBase.rate / 1000.0);
		const auto clockOff = offsetOf(RunGate(clockGate, clockBase, pulseMs + jitterMs, constructMs, holdEndMs));
		clockGate.RemoveAudioBase();

		std::printf("      1/16 = %.2f ms; free-run mean %.2f ms (%.0f%% of a period), chart clock mean %.2f ms\n",
			chartGateMs, freeOff.first, 100.0 * freeOff.first / chartGateMs, clockOff.first);
		Check(freeOff.first > 5.0, "at Cherish's tempo the free-running gate sits off the grid",
			"mean=" + std::to_string(freeOff.first) + " ms");
		Check(clockOff.second < 0.1, "and the chart clock puts it back on the grid",
			"worst=" + std::to_string(clockOff.second) + " ms");
	}

	std::printf("\n[w8] %s\n", g_failures == 0 ? "PASS" : "FAILURES");
	return g_failures == 0 ? 0 : 1;
}

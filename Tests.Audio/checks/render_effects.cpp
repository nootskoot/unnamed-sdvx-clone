/*
	render_effects: render a chart's effect timeline through the clone's real DSP
	classes, offline, and write the result as float WAV plus the dry signal for
	comparison.

	Why this exists rather than reading the game's own output: the full game cannot
	be built on a machine without SDL2, ogg/vorbis and libarchive, and the question
	"does the effect land on its chart time" needs a per-sample answer, not a
	listening impression. So this binary drives the same DSP translation units the
	game links (Audio/src/DSP.cpp + Audio/src/AudioBase.cpp), at the same block size
	the device uses (384 frames), through the same AudioBase hooks, and writes the
	samples out for measurement.

	What it does NOT do
	------------------
	* It does not parse the chart. Tests.Audio/checks/extract_events.py does that
	  with a KShootMap-shaped parser and hands this binary a CSV of spans, so the
	  render side stays small and the parsing side stays debuggable.
	* It does not reproduce AudioPlayback. The two behaviours of AudioPlayback that
	  matter are modelled explicitly and named in the report:
	    - the block clock: AudioStreamBase::Process decodes a whole block and only
	      then advances m_samplePos, so a DSP that reads GetSamplePos() sees the end
	      of the block it is rendering. AUDIT_BEFORE emulates exactly that.
	    - the gate clock: the pre-fix AudioPlayback only gave a gate measure times
	      for KSON charts, so a KSH v1 gate free-ran from the frame that built it.
	      AUDIT_BEFORE skips SetMeasureTimes, which is that path.
	* It does not apply the laser filter to a separate laser bus. A laser-filter
	  span is applied to the whole mix at its own mix value, which is what the
	  effect sounds like; AudioPlayback's split into a laser sample bus is not
	  modelled. Stated in the report.

	Inputs
	------
	  --events <csv>   case,lane,type,p0,p1,mix,startMs,endMs  (see the script)
	  --pcm <raw>      interleaved stereo float32, 44100 Hz, decoded by ffmpeg
	  --start <ms>     window start
	  --end <ms>       window end
	  --out <wav>      processed output
	  --dry <wav>      the same window, unprocessed
	  --log <txt>      what was constructed, when, and what was skipped

	Everything is single threaded, so the render is deterministic and a before/after
	pair differs only by the tree it was compiled against and the AUDIT_BEFORE flag.
*/
#include "stdafx.h"
#include "AudioBase.hpp"
#include "DSP.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
	constexpr uint32 kSampleRate = 44100;
	constexpr uint32 kBlockFrames = 384;
	// The game thread clears an effect on a frame, and a frame is 60 Hz. The
	// pre-fix path therefore stops an effect at the first boundary at or after its
	// end, not on the end frame itself. Modelled at the block boundary, which is the
	// friendliest possible timing for the old path.
	constexpr double kGameFrameMs = 1000.0 / 60.0;

	// The game thread creates and clears effect objects on a frame boundary, so a
	// hold's DSP starts on the first frame at or after its chart time, not on the
	// chart time itself. Same rule for both flavours: the pair then differs only by
	// the code under test, not by when the object appears.
	double NextFrame(double ms)
	{
		return std::ceil(ms / kGameFrameMs) * kGameFrameMs;
	}

	// ---- block-start clock, only present after the timing fix lands ----
	template <typename T, typename = void>
	struct HasBlockStartSample : std::false_type
	{
	};

	template <typename T>
	struct HasBlockStartSample<T, std::void_t<decltype(std::declval<T &>().blockStartSample)>> : std::true_type
	{
	};

	// Written as a template so the assignment lives in a discarded branch that is
	// never instantiated against a tree without the field.
	template <typename B>
	void StoreBlockStart(B &base, uint64 value)
	{
		if constexpr (HasBlockStartSample<B>::value)
		{
			using ClockType = std::remove_reference_t<decltype(base.blockStartSample)>;
			base.blockStartSample = static_cast<ClockType>(value);
		}
		else
		{
			(void)base;
			(void)value;
		}
	}

	class RenderBase final : public AudioBase
	{
	public:
		explicit RenderBase(uint32 rate) : m_rate(rate) {}

		~RenderBase() override
		{
			for (DSP *dsp : DSPs)
				if (dsp)
					dsp->RemoveAudioBase();
			DSPs.clear();
		}

		void Process(float *out, uint32 numSamples) override { ProcessDSPs(out, numSamples); }
		[[nodiscard]] int32 GetPosition() const override
		{
			return static_cast<int32>(m_pos * 1000 / m_rate);
		}
		[[nodiscard]] uint32 GetSampleRate() const override { return m_rate; }
		// The stream position. AudioStreamBase::Process advances this past the whole
		// block before Audio_Impl calls ProcessDSPs, so during a block this reads the
		// END of that block. SetBlock() sets both values the way the real stream and
		// the fixed AudioBase do.
		[[nodiscard]] uint64 GetSamplePos() const override { return m_pos; }
		[[nodiscard]] float *GetPCM() override { return nullptr; }
		[[nodiscard]] uint64 GetPCMCount() const override { return 0; }
		void PreRenderDSPs(Vector<DSP *> &) override {}

		void SetBlock(uint64 blockStart, uint32 frames)
		{
			m_pos = blockStart + frames;
			StoreBlockStart(*this, blockStart);
		}

		void Attach(DSP *dsp)
		{
			DSPs.Add(dsp);
			dsp->SetAudioBase(this);
		}

		void Detach(DSP *dsp)
		{
			DSPs.Remove(dsp);
			dsp->RemoveAudioBase();
			delete dsp;
		}

	private:
		uint32 m_rate = kSampleRate;
		uint64 m_pos = 0;
	};

	struct Span
	{
		std::string lane;
		std::string type;
		double p0 = 0;
		double p1 = 0;
		double p2 = 0;
		double wholeNote = 0;
		double mix = 1.0;
		double startMs = 0;
		double endMs = 0;
		long line = 0;
	};

	struct Live
	{
		DSP *dsp = nullptr;
		const Span *span = nullptr;
		std::string name;
	};

	[[noreturn]] void Die(const char *what)
	{
		std::fprintf(stderr, "render_effects: %s\n", what);
		std::exit(2);
	}

	std::vector<std::string> SplitCsv(const std::string &line)
	{
		std::vector<std::string> out;
		std::string cur;
		for (char c : line)
		{
			if (c == ',')
			{
				out.push_back(cur);
				cur.clear();
			}
			else if (c != '\r')
				cur.push_back(c);
		}
		out.push_back(cur);
		return out;
	}

	double ToD(const std::string &s, double fallback = 0.0)
	{
		if (s.empty())
			return fallback;
		return std::atof(s.c_str());
	}

	// 32-bit float WAV, stereo, so nothing is quantised between the DSP and the
	// measurement. Deliberately the simplest header that a reader accepts.
	void WriteWav(const char *path, const std::vector<float> &samples, uint32 rate)
	{
		std::FILE *f = std::fopen(path, "wb");
		if (!f)
			Die("cannot open output wav");
		const uint32 dataBytes = static_cast<uint32>(samples.size() * sizeof(float));
		const uint32 riffSize = 36 + dataBytes;
		const uint16 channels = 2;
		const uint16 bits = 32;
		const uint16 format = 3; // IEEE float
		const uint32 byteRate = rate * channels * bits / 8;
		const uint16 blockAlign = static_cast<uint16>(channels * bits / 8);
		std::fwrite("RIFF", 1, 4, f);
		std::fwrite(&riffSize, 4, 1, f);
		std::fwrite("WAVEfmt ", 1, 8, f);
		const uint32 fmtSize = 16;
		std::fwrite(&fmtSize, 4, 1, f);
		std::fwrite(&format, 2, 1, f);
		std::fwrite(&channels, 2, 1, f);
		std::fwrite(&rate, 4, 1, f);
		std::fwrite(&byteRate, 4, 1, f);
		std::fwrite(&blockAlign, 2, 1, f);
		std::fwrite(&bits, 2, 1, f);
		std::fwrite("data", 1, 4, f);
		std::fwrite(&dataBytes, 4, 1, f);
		if (!samples.empty())
			std::fwrite(samples.data(), sizeof(float), samples.size(), f);
		std::fclose(f);
	}

	DSP *MakeDsp(const Span &span, uint32 rate, const std::vector<double> &measureTimes, double clockOffset,
				 std::string &nameOut)
	{
		const double wholeNoteMs = 4.0 * (60000.0 / 180.0); // replaced by the caller's tempo below
		(void)wholeNoteMs;

		if (span.type == "Gate")
		{
			auto *gate = new GateDSP(rate);
			// GameAudioEffects::SetParams: SetLength(wholeNote / param0), gating from
			// the effect default (0.5 for a KSH v1 Gate).
			const double whole = span.wholeNote > 0 ? span.wholeNote : 1333.3333333333333;
			gate->SetLength(whole / (span.p0 > 0 ? span.p0 : 4.0));
			gate->SetGating(0.5f);
#if !AUDIT_BEFORE
			// AudioPlayback::m_ConfigureGateClock on the fixed path: KSH v1 rebuilds
			// the measure grid from the timing map, so the gate cannot free-run.
			(void)clockOffset; // the gate clock no longer takes a calibration offset
			gate->SetMeasureTimes(measureTimes);
#endif
			nameOut = "GateDSP";
			return gate;
		}
		if (span.type == "BitCrusher")
		{
			auto *bc = new BitCrusherDSP(rate);
			bc->SetPeriod(static_cast<float>(span.p0));
			nameOut = "BitCrusherDSP";
			return bc;
		}
		if (span.type == "Retrigger")
		{
			auto *rt = new RetriggerDSP(rate);
			const double whole = span.wholeNote > 0 ? span.wholeNote : 1333.3333333333333;
			rt->SetLength(whole / (span.p0 > 0 ? span.p0 : 8.0));
			rt->SetGating(0.7f);
			nameOut = "RetriggerDSP";
			return rt;
		}
		if (span.type == "Flanger")
		{
			nameOut = "FlangerDSP";
			return new FlangerDSP(rate);
		}
		if (span.type == "Phaser")
		{
			nameOut = "PhaserDSP";
			return new PhaserDSP(rate);
		}
		if (span.type == "Wobble")
		{
			auto *wb = new WobbleDSP(rate);
			const double whole = span.wholeNote > 0 ? span.wholeNote : 1333.3333333333333;
			wb->SetLength(whole / (span.p0 > 0 ? span.p0 : 12.0));
			nameOut = "WobbleDSP";
			return wb;
		}
		if (span.type == "Echo")
		{
			auto *echo = new EchoDSP(rate);
			const double whole = span.wholeNote > 0 ? span.wholeNote : 1333.3333333333333;
			echo->SetLength(whole / (span.p0 > 0 ? span.p0 : 4.0));
			if (span.p1 >= 0 && span.p1 <= 100 && span.p1 > 0)
				echo->feedback = static_cast<float>(span.p1 / 100.0);
			nameOut = "EchoDSP";
			return echo;
		}
		if (span.type == "PeakingFilter" || span.type == "LowPassFilter" || span.type == "HighPassFilter")
		{
			auto *bq = new BQFDSP(rate);
			if (span.type == "PeakingFilter")
				bq->SetPeaking(static_cast<float>(span.p1 > 0 ? span.p1 : 4.0),
							   static_cast<float>(span.p0 > 0 ? span.p0 : 1000.0),
							   static_cast<float>(span.p2 != 0 ? span.p2 : 6.0));
			else if (span.type == "LowPassFilter")
				bq->SetLowPass(static_cast<float>(span.p1 > 0 ? span.p1 : 0.707),
							   static_cast<float>(span.p0 > 0 ? span.p0 : 800.0));
			else
				bq->SetHighPass(static_cast<float>(span.p1 > 0 ? span.p1 : 0.707),
								static_cast<float>(span.p0 > 0 ? span.p0 : 8000.0));
			nameOut = "BQFDSP";
			return bq;
		}
		if (span.type == "TapeStop")
		{
			auto *ts = new TapeStopDSP(rate);
			if (span.p0 > 0)
				ts->SetLength(1000.0 * 16.0 / span.p0);
			nameOut = "TapeStopDSP";
			return ts;
		}
		nameOut.clear();
		return nullptr;
	}
}

int main(int argc, char **argv)
{
	const char *eventsPath = nullptr, *pcmPath = nullptr, *outPath = nullptr, *dryPath = nullptr, *logPath = nullptr;
	double winStartMs = 0, winEndMs = 0;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		auto next = [&]() -> const char * {
			if (i + 1 >= argc)
				Die("missing value after flag");
			return argv[++i];
		};
		if (a == "--events")
			eventsPath = next();
		else if (a == "--pcm")
			pcmPath = next();
		else if (a == "--start")
			winStartMs = std::atof(next());
		else if (a == "--end")
			winEndMs = std::atof(next());
		else if (a == "--out")
			outPath = next();
		else if (a == "--dry")
			dryPath = next();
		else if (a == "--log")
			logPath = next();
		else
			Die("unknown flag");
	}
	if (!eventsPath || !pcmPath || !outPath || winEndMs <= winStartMs)
		Die("need --events --pcm --start --end --out");

	// ---- source ----
	std::FILE *pf = std::fopen(pcmPath, "rb");
	if (!pf)
		Die("cannot open pcm");
	std::fseek(pf, 0, SEEK_END);
	const long pcmBytes = std::ftell(pf);
	std::fseek(pf, 0, SEEK_SET);
	std::vector<float> source(static_cast<size_t>(pcmBytes / sizeof(float)));
	if (!source.empty() && std::fread(source.data(), sizeof(float), source.size(), pf) != source.size())
		Die("short read on pcm");
	std::fclose(pf);

	// ---- spans ----
	std::vector<Span> spans;
	{
		std::FILE *ef = std::fopen(eventsPath, "r");
		if (!ef)
			Die("cannot open events csv");
		char buf[1024];
		long lineNo = 0;
		while (std::fgets(buf, sizeof(buf), ef))
		{
			lineNo++;
			std::string line(buf);
			while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
				line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;
			const auto f = SplitCsv(line);
			if (f.size() < 9)
				continue;
			if (f[0] == "case")
				continue; // header
			Span s;
			s.lane = f[1];
			s.type = f[2];
			s.p0 = ToD(f[3]);
			s.p1 = ToD(f[4]);
			s.p2 = ToD(f[5]);
			s.mix = ToD(f[6], 1.0);
			s.startMs = ToD(f[7]);
			s.endMs = ToD(f[8]);
			s.wholeNote = f.size() > 9 ? ToD(f[9]) : 0;
			s.line = lineNo;
			spans.push_back(s);
		}
		std::fclose(ef);
	}

	std::vector<double> measureTimes;
	if (const char *mp = std::getenv("AUDIT_MEASURE_TIMES"))
	{
		std::FILE *mf = std::fopen(mp, "r");
		if (mf)
		{
			double v = 0;
			while (std::fscanf(mf, "%lf", &v) == 1)
				measureTimes.push_back(v);
			std::fclose(mf);
		}
	}
	double clockOffset = 0;
	if (const char *co = std::getenv("AUDIT_CLOCK_OFFSET"))
		clockOffset = std::atof(co);

	std::FILE *logf = logPath ? std::fopen(logPath, "w") : nullptr;
	auto Log = [&](const char *fmt, auto... args) {
		if (logf)
			std::fprintf(logf, fmt, args...);
	};

	const uint64 winStart = static_cast<uint64>(std::llround(winStartMs * kSampleRate / 1000.0));
	const uint64 winEnd = static_cast<uint64>(std::llround(winEndMs * kSampleRate / 1000.0));
	const uint32 frames = static_cast<uint32>(winEnd > winStart ? winEnd - winStart : 0);
	if (!frames)
		Die("empty window");

	std::vector<float> dry(frames * 2, 0.0f);
	for (uint32 i = 0; i < frames; i++)
	{
		const size_t src = static_cast<size_t>((winStart + i) * 2);
		if (src + 1 < source.size())
		{
			dry[i * 2] = source[src];
			dry[i * 2 + 1] = source[src + 1];
		}
	}
	if (dryPath)
		WriteWav(dryPath, dry, kSampleRate);

	std::vector<float> wet = dry;

	RenderBase base(kSampleRate);
	std::vector<Live> live;
	std::vector<std::string> skipped;

	// Spans sorted by start, so construction order matches the chart.
	std::vector<const Span *> ordered;
	for (const Span &s : spans)
	{
		// A span that ended before the window opens never ran inside it, so do not
		// construct and immediately tear it down on the first block.
		if (s.endMs > 0 && s.endMs <= winStartMs)
			continue;
		ordered.push_back(&s);
	}
	std::stable_sort(ordered.begin(), ordered.end(),
					 [](const Span *a, const Span *b) { return a->startMs < b->startMs; });

	size_t nextSpan = 0;
	for (uint32 offset = 0; offset < frames; offset += kBlockFrames)
	{
		const uint32 n = std::min(kBlockFrames, frames - offset);
		const uint64 blockStart = winStart + offset;
		const double blockTimeMs = double(blockStart) * 1000.0 / kSampleRate;
		const double blockEndTimeMs = double(blockStart + n) * 1000.0 / kSampleRate;

		// Construct anything whose start the audio clock has reached, and remove
		// anything whose end has passed. The game does both on a frame boundary; a
		// block boundary is the friendly case for the old path, and it is the same
		// rule for both trees so the pair differs only by the fix under test.
		while (nextSpan < ordered.size() && NextFrame(ordered[nextSpan]->startMs) <= blockTimeMs)
		{
			const Span *s = ordered[nextSpan++];
			std::string name;
			DSP *dsp = MakeDsp(*s, kSampleRate, measureTimes, clockOffset, name);
			if (!dsp)
			{
				skipped.push_back(s->type);
				Log("skip lane=%s type=%s at %.3f (unsupported here)\n", s->lane.c_str(), s->type.c_str(),
					s->startMs);
				continue;
			}
			dsp->startTime = static_cast<uint32>(std::llround(NextFrame(s->startMs)));
			dsp->mix = static_cast<float>(s->mix);
#if !AUDIT_BEFORE
			// The endTime stamp is the fix under test. Without it the effect runs until
			// the game thread removes it, which is what this loop does below.
			if (s->endMs > 0)
				dsp->endTime = static_cast<uint32>(std::llround(s->endMs));
#endif
			base.Attach(dsp);
			live.push_back({dsp, s, name});
			Log("add %s lane=%s start=%.2f end=%.2f p0=%.1f p1=%.1f mix=%.2f\n", name.c_str(), s->lane.c_str(),
				s->startMs, s->endMs, s->p0, s->p1, s->mix);
		}

		for (size_t i = 0; i < live.size();)
		{
			const Live &l = live[i];
			const bool expired = l.span->endMs > 0 && blockTimeMs >= NextFrame(l.span->endMs);
			if (expired)
			{
				Log("remove %s at %.2f (authored end %.2f)\n", l.name.c_str(), blockTimeMs, l.span->endMs);
				base.Detach(l.dsp);
				live.erase(live.begin() + static_cast<long>(i));
				continue;
			}
			// A span whose mix is automated (a laser filter ramping 0 -> 90%) is
			// applied per block, the way AudioPlayback sets dsp->mix on automation.
			if (l.span->mix >= 0.0 && std::fabs(l.dsp->mix - l.span->mix) > 1e-6)
				l.dsp->mix = static_cast<float>(l.span->mix);
			i++;
		}

		base.SetBlock(blockStart, n);
		base.ProcessDSPs(wet.data() + offset * 2, n);
		(void)blockEndTimeMs;
	}

	WriteWav(outPath, wet, kSampleRate);
	if (logf)
	{
		std::fprintf(logf, "# summary: %zu spans, %zu live at end, %zu skipped\n", spans.size(), live.size(),
					 skipped.size());
		std::fclose(logf);
	}
	std::printf("render_effects: %s (%.1f ms window, %u frames, %zu spans, %zu skipped)\n", outPath,
				winEndMs - winStartMs, frames, spans.size(), skipped.size());
	return 0;
}

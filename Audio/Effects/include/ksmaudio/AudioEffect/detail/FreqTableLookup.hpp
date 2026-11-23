#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ksmaudio::AudioEffect::detail
{
	constexpr std::size_t kFreqTableSize = 256;

	inline float FreqTableForwardLookup(const float* table, float v) noexcept
	{
		constexpr float kMaxIdx = static_cast<float>(kFreqTableSize - 1);

		if (v < 0.0f)
		{
			const float logSlope = std::log(table[1] / table[0]) * kMaxIdx;
			return table[0] * std::exp(logSlope * v);
		}

		if (v > 1.0f)
		{
			const float logSlope = std::log(table[kFreqTableSize - 1] / table[kFreqTableSize - 2]) * kMaxIdx;
			return table[kFreqTableSize - 1] * std::exp(logSlope * (v - 1.0f));
		}

		const float pos = v * kMaxIdx;
		const auto idx = static_cast<std::size_t>(pos);
		if (idx >= kFreqTableSize - 1)
		{
			return table[kFreqTableSize - 1];
		}
		const float frac = pos - static_cast<float>(idx);
		return std::lerp(table[idx], table[idx + 1], frac);
	}

	inline float FreqTableInverseLookup(const float* table, float freq) noexcept
	{
		constexpr float kMaxIdx = static_cast<float>(kFreqTableSize - 1);
		const bool increasing = table[kFreqTableSize - 1] > table[0];

		if ((increasing && freq <= table[0]) || (!increasing && freq >= table[0]))
		{
			const float logSlope = std::log(table[1] / table[0]) * kMaxIdx;
			if (std::abs(logSlope) < 1e-6f)
			{
				return 0.0f;
			}
			return std::log(freq / table[0]) / logSlope;
		}

		if ((increasing && freq >= table[kFreqTableSize - 1]) || (!increasing && freq <= table[kFreqTableSize - 1]))
		{
			const float logSlope = std::log(table[kFreqTableSize - 1] / table[kFreqTableSize - 2]) * kMaxIdx;
			if (std::abs(logSlope) < 1e-6f)
			{
				return 1.0f;
			}
			return 1.0f + std::log(freq / table[kFreqTableSize - 1]) / logSlope;
		}

		std::size_t lo = 0;
		std::size_t hi = kFreqTableSize - 1;
		while (lo + 1 < hi)
		{
			const std::size_t mid = (lo + hi) / 2;
			if ((increasing && table[mid] <= freq) || (!increasing && table[mid] >= freq))
			{
				lo = mid;
			}
			else
			{
				hi = mid;
			}
		}

		const float frac = (freq - table[lo]) / (table[hi] - table[lo]);
		return (static_cast<float>(lo) + frac) / kMaxIdx;
	}

	inline float FreqTableClampedLookup(const float* table, float v) noexcept
	{
		constexpr float kMaxIdx = static_cast<float>(kFreqTableSize - 1);
		const float clampedV = std::clamp(v, 0.0f, 1.0f);
		const float pos = clampedV * kMaxIdx;
		const auto idx = static_cast<std::size_t>(pos);
		if (idx >= kFreqTableSize - 1)
		{
			return table[kFreqTableSize - 1];
		}
		const float frac = pos - static_cast<float>(idx);
		return std::lerp(table[idx], table[idx + 1], frac);
	}
}

#pragma once
// Shim stream: the minimal surface AudioEffectBus needs (sampleRate, numChannels,
// addAudioEffect, removeAudioEffect). Effects run in BASS priority order (highest first).
#include <algorithm>
#include <cstddef>
#include <vector>
#include "ksmaudio/Backend.hpp"
#include "ksmaudio/AudioEffect/AudioEffect.hpp"

namespace ksmaudio
{
	class Stream
	{
		struct Entry { HDSP h; int priority; AudioEffect::IAudioEffect* fx; };
		std::size_t m_sr, m_ch;
		mutable std::vector<Entry> m_chain;
		mutable HDSP m_next = 1;
	public:
		Stream(std::size_t sampleRate, std::size_t numChannels) : m_sr(sampleRate), m_ch(numChannels) {}
		std::size_t sampleRate() const { return m_sr; }
		std::size_t numChannels() const { return m_ch; }
		HDSP addAudioEffect(AudioEffect::IAudioEffect* p, int priority) const
		{
			m_chain.push_back({ m_next, priority, p });
			std::stable_sort(m_chain.begin(), m_chain.end(), [](const Entry& a, const Entry& b) { return a.priority > b.priority; });
			return m_next++;
		}
		void removeAudioEffect(HDSP h) const
		{
			m_chain.erase(std::remove_if(m_chain.begin(), m_chain.end(), [h](const Entry& e) { return e.h == h; }), m_chain.end());
		}
		std::size_t chainSize() const { return m_chain.size(); }
		// Offline render hook: run interleaved float buffer through the chain.
		void processOffline(float* data, std::size_t numFloats) const
		{
			for (const auto& e : m_chain) e.fx->process(data, numFloats);
		}
	};
}

#pragma once
#include <set>

namespace ksmaudio::AudioEffect::detail
{
    class UpdateTriggerTimeline
    {
	private:
		const std::set<float> m_updateTriggerTiming;
		std::set<float>::const_iterator m_updateTriggerTimingCursor;
		float m_secUntilTrigger = -1.0f;

	public:
		explicit UpdateTriggerTimeline(const std::set<float>& updateTriggerTiming)
			: m_updateTriggerTiming(updateTriggerTiming)
			, m_updateTriggerTimingCursor(m_updateTriggerTiming.begin())
		{
		}

		void update(float currentTimeSec, float playbackSpeed = 1.0f)
		{
			// ループ再生などで時間が巻き戻るケースがあるため、カーソルは毎回探し直す
			m_updateTriggerTimingCursor = m_updateTriggerTiming.lower_bound(currentTimeSec);

			if (m_updateTriggerTimingCursor == m_updateTriggerTiming.end())
			{
				// 負の値はDSP側で無視される
				m_secUntilTrigger = -1.0f;
				return;
			}

			// Chart time -> output time: the DSP counts output frames (post time-stretch), so a countdown in
			// song seconds must be divided by the playback speed.
			const float speed = playbackSpeed > 0.0f ? playbackSpeed : 1.0f;
			m_secUntilTrigger = (*m_updateTriggerTimingCursor - currentTimeSec) / speed;
		}

		float secUntilTrigger() const
		{
			return m_secUntilTrigger;
		}
    };
}

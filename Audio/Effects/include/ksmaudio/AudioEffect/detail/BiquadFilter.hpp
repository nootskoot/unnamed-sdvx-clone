#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ksmaudio::AudioEffect::detail
{
    template <typename T>
    class BiquadFilter
    {
    private:
        // ナイキスト周波数以上を指定すると発散するため、sampleRateの0.49倍を上限とする
        static T ClampFreqBelowNyquist(T freq, T sampleRate)
        {
            return (std::min)(freq, sampleRate * T{ 0.49 });
        }

        // qが0以下だとゼロ除算になるため、下限を設ける
        static T ClampQ(T q)
        {
            return (std::max)(q, T{ 0.01 });
        }

        // bandwidthが負だと発散し、大きすぎるとsinhが溢れるため、仕様の範囲(0.1〜10.0)に丸める
        static T ClampBandwidth(T bandwidth)
        {
            return std::clamp(bandwidth, T{ 0.1 }, T{ 10.0 });
        }

        T m_a0 = T{ 1 };
        T m_a1 = T{ 0 };
        T m_a2 = T{ 0 };
        T m_b0 = T{ 1 };
        T m_b1 = T{ 0 };
        T m_b2 = T{ 0 };
        T m_input1 = T{ 0 };
        T m_input2 = T{ 0 };
        T m_output1 = T{ 0 };
        T m_output2 = T{ 0 };

    public:
        BiquadFilter() = default;

        T process(T input)
        {
            const T output
                = m_b0 / m_a0 * input
                + m_b1 / m_a0 * m_input1
                + m_b2 / m_a0 * m_input2
                - m_a1 / m_a0 * m_output1
                - m_a2 / m_a0 * m_output2;

            m_input2 = m_input1;
            m_input1 = input;
            m_output2 = m_output1;
            m_output1 = output;

            return output;
        }

        void setLowPassFilter(T freq, T q, T sampleRate)
        {
            freq = ClampFreqBelowNyquist(freq, sampleRate);
            q = ClampQ(q);

            const T omega = std::numbers::pi_v<T> * 2 * freq / sampleRate;
            const T alpha = std::sin(omega) / (q * 2);

            const T cosOmega = std::cos(omega);
            m_a0 = T{ 1 } + alpha;
            m_a1 = -T{ 2 } * cosOmega;
            m_a2 = T{ 1 } - alpha;
            m_b0 = (T{ 1 } - cosOmega) / 2;
            m_b1 = T{ 1 } - cosOmega;
            m_b2 = (T{ 1 } - cosOmega) / 2;
        }

        void setLowShelfFilter(T freq, T q, T gainDb, T sampleRate)
        {
            freq = ClampFreqBelowNyquist(freq, sampleRate);

            const T omega = std::numbers::pi_v<T> * 2 * freq / sampleRate;
            const T A = std::pow(T{ 10 }, gainDb / 40);
            const T beta = std::sqrt(A) / q;

            const T sinOmega = std::sin(omega);
            const T cosOmega = std::cos(omega);
            m_a0 = (A + T{ 1 }) + (A - T{ 1 }) * cosOmega + beta * sinOmega;
            m_a1 = -T{ 2 } * ((A - T{ 1 }) + (A + T{ 1 }) * cosOmega);
            m_a2 = (A + T{ 1 }) + (A - T{ 1 }) * cosOmega - beta * sinOmega;
            m_b0 = A * ((A + T{ 1 }) - (A - T{ 1 }) * cosOmega + beta * sinOmega);
            m_b1 = T{ 2 } * A * ((A - T{ 1 }) - (A + T{ 1 }) * cosOmega);
            m_b2 = A * ((A + T{ 1 }) - (A - T{ 1 }) * cosOmega - beta * sinOmega);
        }

        void setHighPassFilter(T freq, T q, T sampleRate)
        {
            freq = ClampFreqBelowNyquist(freq, sampleRate);
            q = ClampQ(q);

            const T omega = std::numbers::pi_v<T> * 2 * freq / sampleRate;
            const T alpha = std::sin(omega) / (q * 2);

            const T cosOmega = std::cos(omega);
            m_a0 = T{ 1 } + alpha;
            m_a1 = -T{ 2 } * cosOmega;
            m_a2 = T{ 1 } - alpha;
            m_b0 = (T{ 1 } + cosOmega) / 2;
            m_b1 = -T{ 1 } - cosOmega;
            m_b2 = (T{ 1 } + cosOmega) / 2;
        }

        void setHighShelfFilter(T freq, T q, T gainDb, T sampleRate)
        {
            freq = ClampFreqBelowNyquist(freq, sampleRate);

            const T omega = std::numbers::pi_v<T> * 2 * freq / sampleRate;
            const T A = std::pow(T{ 10 }, gainDb / 40);
            const T beta = std::sqrt(A) / q;

            const T sinOmega = std::sin(omega);
            const T cosOmega = std::cos(omega);
            m_a0 = (A + T{ 1 }) - (A - T{ 1 }) * cosOmega + beta * sinOmega;
            m_a1 = T{ 2 } *((A - T{ 1 }) - (A + T{ 1 }) * cosOmega);
            m_a2 = (A + T{ 1 }) - (A - T{ 1 }) * cosOmega - beta * sinOmega;
            m_b0 = A * ((A + T{ 1 }) + (A - T{ 1 }) * cosOmega + beta * sinOmega);
            m_b1 = -T{ 2 } * A * ((A - T{ 1 }) + (A + T{ 1 }) * cosOmega);
            m_b2 = A * ((A + T{ 1 }) + (A - T{ 1 }) * cosOmega - beta * sinOmega);
        }

        void setPeakingFilter(T freq, T bandwidth, T gainDb, T sampleRate)
        {
            freq = ClampFreqBelowNyquist(freq, sampleRate);
            bandwidth = ClampBandwidth(bandwidth);

            // ナイキスト周波数付近ではsinhがfloatの範囲を超えるため、係数はdoubleで求めてa0で正規化してから格納する
            const double omega = std::numbers::pi * 2 * freq / sampleRate;
            const double alpha = std::sin(omega) * std::sinh(std::log(2.0) / 2 * bandwidth * omega / std::sin(omega));
            const double A = std::pow(10.0, gainDb / 40);

            const double cosOmega = std::cos(omega);
            const double a0 = 1 + alpha / A;
            m_a0 = T{ 1 };
            m_a1 = static_cast<T>(-2 * cosOmega / a0);
            m_a2 = static_cast<T>((1 - alpha / A) / a0);
            m_b0 = static_cast<T>((1 + alpha * A) / a0);
            m_b1 = static_cast<T>(-2 * cosOmega / a0);
            m_b2 = static_cast<T>((1 - alpha * A) / a0);
        }

        void setAllPassFilter(T freq, T q, T sampleRate)
        {
            freq = ClampFreqBelowNyquist(freq, sampleRate);
            q = ClampQ(q);

            const T omega = std::numbers::pi_v<T> * 2 * freq / sampleRate;
            const T alpha = std::sin(omega) / (q * 2);

            const T cosOmega = std::cos(omega);
            m_a0 = T{ 1 } + alpha;
            m_a1 = -T{ 2 } * cosOmega;
            m_a2 = T{ 1 } - alpha;
            m_b0 = T{ 1 } - alpha;
            m_b1 = -T{ 2 } * cosOmega;
            m_b2 = T{ 1 } + alpha;
        }
    };
}

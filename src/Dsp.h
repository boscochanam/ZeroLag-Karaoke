// Real-time DSP building blocks. Everything here is allocation-free after
// prepare() and safe to call from the audio thread.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dsp {

constexpr float kPi = 3.14159265358979f;

inline float dbToLin(float db) { return std::exp(db * 0.11512925464970229f); } // ln(10)/20
inline float linToDb(float x) { return 8.685889638065035f * std::log(std::max(x, 1e-9f)); }

// Coefficient for a one-pole smoother with the given time constant.
inline float onePoleCoef(float ms, float sr) { return 1.f - std::exp(-1.f / (0.001f * ms * sr)); }

// Parameter smoother to avoid zipper noise when sliders move.
struct Smoothed {
    float value = 0.f, coef = 1.f;
    void prepare(float sr, float ms, float initial) { coef = onePoleCoef(ms, sr); value = initial; }
    inline float next(float target) { value += coef * (target - value); return value; }
};

// RBJ biquad, transposed direct form II.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() { z1 = z2 = 0; }

    inline float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setHighPass(float sr, float f, float q = 0.7071f) {
        const float w = 2 * kPi * f / sr, cs = std::cos(w), alpha = std::sin(w) / (2 * q);
        const float a0 = 1 + alpha;
        b0 = (1 + cs) * 0.5f / a0; b1 = -(1 + cs) / a0; b2 = b0;
        a1 = -2 * cs / a0; a2 = (1 - alpha) / a0;
    }

    void setPeak(float sr, float f, float q, float db) {
        const float A = std::pow(10.f, db / 40.f);
        const float w = 2 * kPi * f / sr, cs = std::cos(w), alpha = std::sin(w) / (2 * q);
        const float a0 = 1 + alpha / A;
        b0 = (1 + alpha * A) / a0; b1 = -2 * cs / a0; b2 = (1 - alpha * A) / a0;
        a1 = -2 * cs / a0; a2 = (1 - alpha / A) / a0;
    }

    void setLowShelf(float sr, float f, float db) {
        const float A = std::pow(10.f, db / 40.f);
        const float w = 2 * kPi * f / sr, cs = std::cos(w), sn = std::sin(w);
        const float k = 2 * std::sqrt(A) * (sn * 0.70710678f); // 2*sqrt(A)*alpha, S = 1
        const float a0 = (A + 1) + (A - 1) * cs + k;
        b0 = A * ((A + 1) - (A - 1) * cs + k) / a0;
        b1 = 2 * A * ((A - 1) - (A + 1) * cs) / a0;
        b2 = A * ((A + 1) - (A - 1) * cs - k) / a0;
        a1 = -2 * ((A - 1) + (A + 1) * cs) / a0;
        a2 = ((A + 1) + (A - 1) * cs - k) / a0;
    }

    void setHighShelf(float sr, float f, float db) {
        const float A = std::pow(10.f, db / 40.f);
        const float w = 2 * kPi * f / sr, cs = std::cos(w), sn = std::sin(w);
        const float k = 2 * std::sqrt(A) * (sn * 0.70710678f);
        const float a0 = (A + 1) - (A - 1) * cs + k;
        b0 = A * ((A + 1) + (A - 1) * cs + k) / a0;
        b1 = -2 * A * ((A - 1) + (A + 1) * cs) / a0;
        b2 = A * ((A + 1) + (A - 1) * cs - k) / a0;
        a1 = 2 * ((A - 1) - (A + 1) * cs) / a0;
        a2 = ((A + 1) - (A - 1) * cs - k) / a0;
    }
};

// Noise gate with hold, so breaths/room noise don't get monitored.
struct NoiseGate {
    float env = 0, gain = 0;
    float envAtt = 0, envRel = 0, openC = 0, closeC = 0;
    int holdSamples = 0, holdCount = 0;

    void prepare(float sr) {
        envAtt = onePoleCoef(0.3f, sr);
        envRel = onePoleCoef(40.f, sr);
        openC = onePoleCoef(0.8f, sr);
        closeC = onePoleCoef(80.f, sr);
        holdSamples = static_cast<int>(0.06f * sr);
        env = gain = 0; holdCount = 0;
    }

    inline float process(float x, float threshLin) {
        const float a = std::fabs(x);
        env += (a > env ? envAtt : envRel) * (a - env);
        float target;
        if (env > threshLin) { holdCount = holdSamples; target = 1.f; }
        else if (holdCount > 0) { --holdCount; target = 1.f; }
        else target = 0.f;
        gain += (target > gain ? openC : closeC) * (target - gain);
        return x * gain;
    }
};

// Feed-forward peak compressor (hard knee).
struct Compressor {
    float env = 0, att = 0, rel = 0, lastGain = 1;

    void prepare(float sr) { att = onePoleCoef(3.f, sr); rel = onePoleCoef(120.f, sr); env = 0; lastGain = 1; }

    inline float process(float x, float threshDb, float slope /* 1 - 1/ratio */) {
        const float a = std::fabs(x);
        env += (a > env ? att : rel) * (a - env);
        const float over = linToDb(env) - threshDb;
        lastGain = over > 0.f ? dbToLin(-over * slope) : 1.f;
        return x * lastGain;
    }
};

// "Voice boost": soft-knee 4:1 leveler whose threshold is derived from the
// boost amount so quiet singing gains `boostDb` while 0 dBFS peaks stay put.
// Louder overall, no extra headroom used.
struct Leveler {
    static constexpr float kRatio = 4.f, kSlope = 1.f - 1.f / kRatio, kKneeDb = 6.f;
    float env = 0, att = 0, rel = 0, lastGainDb = 0;

    void prepare(float sr) { att = onePoleCoef(5.f, sr); rel = onePoleCoef(150.f, sr); env = 0; lastGainDb = 0; }

    inline float process(float x, float boostDb) {
        const float a = std::fabs(x);
        env += (a > env ? att : rel) * (a - env);
        const float thresh = -boostDb / kSlope; // makes gain at 0 dBFS == 0 dB
        const float over = linToDb(env) - thresh;
        float reduce;
        if (over <= -0.5f * kKneeDb) reduce = 0.f;
        else if (over >= 0.5f * kKneeDb) reduce = over * kSlope;
        else { const float k = over + 0.5f * kKneeDb; reduce = kSlope * k * k / (2.f * kKneeDb); }
        lastGainDb = boostDb - reduce;
        return x * dbToLin(lastGainDb);
    }
};

// Zero-latency stereo-linked peak limiter: instant attack, smooth release.
// Guarantees |out| <= ceiling without adding any buffering.
struct Limiter {
    float gain = 1.f, rel = 0.f;

    void prepare(float sr) { rel = onePoleCoef(80.f, sr); gain = 1.f; }

    inline void process(float& l, float& r, float ceiling) {
        const float peak = std::max(std::fabs(l), std::fabs(r));
        const float target = peak > ceiling ? ceiling / peak : 1.f;
        gain = target < gain ? target : gain + rel * (target - gain);
        l *= gain;
        r *= gain;
    }
};

// Feedback delay with smoothed, interpolated delay time and a darkening
// low-pass in the feedback path.
struct Echo {
    std::vector<float> buf;
    size_t mask = 0, w = 0;
    double delay = -1;
    float smoothC = 0, lp = 0;

    void prepare(float sr, float maxSeconds) {
        const size_t need = static_cast<size_t>(sr * maxSeconds) + 4;
        size_t n = 1;
        while (n < need) n <<= 1;
        buf.assign(n, 0.f);
        mask = n - 1; w = 0; delay = -1; lp = 0;
        smoothC = onePoleCoef(60.f, sr);
    }

    void clear() { std::fill(buf.begin(), buf.end(), 0.f); lp = 0; }

    inline float process(float x, float delaySamples, float feedback) {
        if (delay < 0) delay = delaySamples;
        delay += smoothC * (delaySamples - delay);
        double rp = static_cast<double>(w) - delay;
        if (rp < 0) rp += static_cast<double>(buf.size());
        const size_t i0 = static_cast<size_t>(rp);
        const float fr = static_cast<float>(rp - static_cast<double>(i0));
        const float y = buf[i0 & mask] + fr * (buf[(i0 + 1) & mask] - buf[i0 & mask]);
        lp += 0.45f * (y - lp);
        buf[w] = x + lp * feedback;
        w = (w + 1) & mask;
        return y;
    }
};

// Freeverb (Jezar at Dreampoint, public domain) – stereo Schroeder/Moorer reverb.
struct Reverb {
    struct Comb {
        std::vector<float> buf; size_t idx = 0; float store = 0;
        inline float process(float in, float fb, float d1, float d2) {
            const float out = buf[idx];
            store = out * d2 + store * d1;
            buf[idx] = in + store * fb;
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf; size_t idx = 0;
        inline float process(float in) {
            const float bo = buf[idx];
            buf[idx] = in + bo * 0.5f;
            if (++idx >= buf.size()) idx = 0;
            return bo - in;
        }
    };

    static constexpr int kCombs = 8, kAllpasses = 4, kSpread = 23;
    Comb combL[kCombs], combR[kCombs];
    Allpass apL[kAllpasses], apR[kAllpasses];

    void prepare(float sr) {
        static constexpr int combT[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static constexpr int apT[kAllpasses] = {556, 441, 341, 225};
        const float s = sr / 44100.f;
        auto len = [s](int t) { return static_cast<size_t>(std::max(1.f, t * s)); };
        for (int i = 0; i < kCombs; ++i) {
            combL[i] = {std::vector<float>(len(combT[i]), 0.f)};
            combR[i] = {std::vector<float>(len(combT[i] + kSpread), 0.f)};
        }
        for (int i = 0; i < kAllpasses; ++i) {
            apL[i] = {std::vector<float>(len(apT[i]), 0.f)};
            apR[i] = {std::vector<float>(len(apT[i] + kSpread), 0.f)};
        }
    }

    void clear() {
        for (auto* c : {combL, combR})
            for (int i = 0; i < kCombs; ++i) { std::fill(c[i].buf.begin(), c[i].buf.end(), 0.f); c[i].store = 0; }
        for (auto* a : {apL, apR})
            for (int i = 0; i < kAllpasses; ++i) std::fill(a[i].buf.begin(), a[i].buf.end(), 0.f);
    }

    // size, damp in [0,1]
    inline void process(float in, float size, float damp, float& outL, float& outR) {
        const float fb = size * 0.28f + 0.7f;
        const float d1 = damp * 0.4f, d2 = 1.f - d1;
        const float x = in * 0.015f;
        float l = 0, r = 0;
        for (int i = 0; i < kCombs; ++i) {
            l += combL[i].process(x, fb, d1, d2);
            r += combR[i].process(x, fb, d1, d2);
        }
        for (int i = 0; i < kAllpasses; ++i) {
            l = apL[i].process(l);
            r = apR[i].process(r);
        }
        outL = l; outR = r;
    }
};

// Transparent below 0.9, smoothly saturates to +/-1 above – protects ears.
inline float safetyClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.9f) return x;
    const float over = (a - 0.9f) / 0.1f;
    const float y = 0.9f + 0.1f * (over / (1.f + over));
    return x < 0 ? -y : y;
}

} // namespace dsp

#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <RtAudio.h>

#include "Dsp.h"
#include "WasapiExclusive.h"

// Everything the UI can tweak. Written by the UI thread, read once per block
// by the audio thread (relaxed atomics – no locks on the real-time path).
struct Params {
    // USB mics often sit at a low Windows level (Discord's auto gain likes to
    // pull them down), so start with boost; the voice must sit on top of music.
    std::atomic<float> inputGainDb{12.f};
    std::atomic<float> outputGainDb{0.f};
    std::atomic<bool> mute{false};
    std::atomic<float> voiceBoostDb{0.f}; // louder without clipping (leveler + limiter)
    std::atomic<bool> testTone{false}; // 440 Hz beep to check the headphone path

    std::atomic<bool> gateOn{true};
    std::atomic<float> gateThreshDb{-40.f};

    std::atomic<bool> eqOn{false};
    std::atomic<float> eqLowCutHz{80.f};
    std::atomic<float> eqLowDb{0.f};
    std::atomic<float> eqMidDb{0.f};
    std::atomic<float> eqMidHz{2500.f};
    std::atomic<float> eqHighDb{0.f};

    std::atomic<bool> compOn{false};
    std::atomic<float> compThreshDb{-20.f};
    std::atomic<float> compRatio{3.f};
    std::atomic<float> compMakeupDb{4.f};

    std::atomic<bool> echoOn{false};
    std::atomic<float> echoMs{300.f};
    std::atomic<float> echoFeedback{0.3f};
    std::atomic<float> echoMix{0.25f};

    std::atomic<bool> reverbOn{true};
    std::atomic<float> reverbSize{0.55f};
    std::atomic<float> reverbDamp{0.5f};
    std::atomic<float> reverbMix{0.2f};

    // WASAPI exclusive only: extra FIFO headroom between the two devices.
    std::atomic<float> jitterMs{1.0f};
};

// Written by the audio thread, read by the UI.
struct Meters {
    std::atomic<float> inPeak{0.f};      // max since last UI read
    std::atomic<float> outPeak{0.f};
    std::atomic<float> gateGain{0.f};
    std::atomic<float> compGainDb{0.f};
    std::atomic<float> boostGainDb{0.f};   // what Voice boost is currently adding
    std::atomic<float> limiterGainDb{0.f}; // <0 when the output limiter is catching peaks
    std::atomic<float> cpuLoad{0.f};     // fraction of the buffer period used by DSP
    std::atomic<float> cpuPeak{0.f};
    std::atomic<uint32_t> xruns{0};
    std::atomic<uint64_t> callbacks{0};  // lets the UI detect a stalled driver
};

struct DeviceInfo {
    unsigned id = 0;
    std::wstring endpointId; // WASAPI exclusive only
    std::string name;
    unsigned inputs = 0, outputs = 0;
    std::vector<unsigned> sampleRates;
    unsigned preferredRate = 0;
    bool isDefaultIn = false, isDefaultOut = false;
};

struct DriverChoice {
    std::string name;
    bool exclusive = false;               // our native WASAPI exclusive backend
    RtAudio::Api api = RtAudio::UNSPECIFIED; // otherwise an RtAudio API
};

struct StreamConfig {
    unsigned inDevice = 0, outDevice = 0;
    std::wstring inEndpoint, outEndpoint; // WASAPI exclusive only
    unsigned inChannel = 0;       // mono mic input channel
    unsigned outFirstChannel = 0; // stereo pair starting here
    unsigned sampleRate = 48000;
    unsigned bufferFrames = 1;    // 1 = ask driver for its minimum
    WasapiMode inMode = WasapiMode::Auto;    // native WASAPI only
    WasapiMode outMode = WasapiMode::Shared;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    static std::vector<DriverChoice> drivers();
    void setDriver(const DriverChoice& d);
    bool exclusive() const { return exclusive_; }
    RtAudio::Api api() const { return exclusive_ ? RtAudio::UNSPECIFIED : api_; }
    const WasapiExclusive& wasapi() const { return wasapi_; }
    std::vector<DeviceInfo> devices();

    bool start(const StreamConfig& cfg);
    void stop();
    bool running() const { return running_; }

    unsigned streamSampleRate() const { return sampleRate_; }
    unsigned streamBufferFrames() const { return bufferFrames_; }
    long streamLatencyFrames() const { return latencyFrames_; }
    std::string lastError();

    // Round-trip test: patch an output into the input with a cable (or hold
    // headphones to the mic), then call this. Normal monitoring pauses.
    void requestLatencyTest() { ltState_.store(kLtRequested, std::memory_order_release); }
    bool latencyTestBusy() const;
    // > 0: measured round trip in frames; 0: none yet; -1: timed out.
    long latencyTestResult() const { return ltResult_.load(std::memory_order_acquire); }

    Params params;
    Meters meters;

private:
    static int rtCallback(void* out, void* in, unsigned int nFrames, double streamTime,
                          RtAudioStreamStatus status, void* user);
    static void wasapiPrepare(void* user, unsigned rate, unsigned maxFrames);
    static void wasapiProcess(void* user, const float* in, float* outL, float* outR, unsigned n, bool xrun);
    void setApi(RtAudio::Api api);
    bool startRtAudio(const StreamConfig& cfg);
    bool startExclusive(const StreamConfig& cfg);
    void processBlock(const float* in, float* outL, float* outR, unsigned n, bool xrun);
    void prepare(float sr, unsigned maxFrames);
    void process(const float* in, float* outL, float* outR, unsigned n);
    bool runLatencyTest(const float* in, float* outL, float* outR, unsigned n);

    std::unique_ptr<RtAudio> rt_;
    RtAudio::Api api_ = RtAudio::UNSPECIFIED;
    bool exclusive_ = false;
    WasapiExclusive wasapi_;
    bool running_ = false;
    unsigned sampleRate_ = 0, bufferFrames_ = 0;
    long latencyFrames_ = 0;
    std::mutex errMutex_;
    std::string lastError_;

    // ---- audio-thread state (touched only by the callback after start) ----
    float sr_ = 48000.f;
    std::vector<float> scratch_;
    dsp::Smoothed inGain_, outGain_;
    dsp::NoiseGate gate_;
    dsp::Biquad lowCut_, lowShelf_, midPeak_, highShelf_;
    float eqCache_[5] = {-1, -1, -1, -1, -1};
    dsp::Compressor comp_;
    dsp::Leveler leveler_;
    dsp::Limiter limiter_;
    dsp::Echo echo_;
    dsp::Reverb reverb_;
    bool echoWasOn_ = false, reverbWasOn_ = false;
    float tonePhase_ = 0.f;

    enum { kLtIdle = 0, kLtRequested, kLtRunning };
    std::atomic<int> ltState_{kLtIdle};
    std::atomic<long> ltResult_{0};
    long ltCount_ = 0, ltPulseAt_ = 0;
    float ltNoise_ = 0;
};

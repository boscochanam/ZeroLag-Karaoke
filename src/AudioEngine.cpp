#include "AudioEngine.h"

#include <chrono>
#include <cmath>
#include <xmmintrin.h>

namespace {
constexpr auto kRelaxed = std::memory_order_relaxed;

// Keep the max since the UI last read (UI resets with exchange(0)).
inline void storeMax(std::atomic<float>& a, float v) {
    if (v > a.load(kRelaxed)) a.store(v, kRelaxed);
}
} // namespace

AudioEngine::AudioEngine() {
    // Native WASAPI exclusive first: lowest latency with ordinary USB/onboard
    // devices and it uses the Windows default mic/headphones out of the box.
    setDriver(drivers().front());
}

AudioEngine::~AudioEngine() { stop(); }

std::vector<DriverChoice> AudioEngine::drivers() {
    std::vector<DriverChoice> out;
    out.push_back({"WASAPI Low Latency (native)", true, RtAudio::UNSPECIFIED});
    std::vector<RtAudio::Api> apis;
    RtAudio::getCompiledApi(apis);
    // ASIO first among RtAudio APIs (for real interfaces), then shared WASAPI etc.
    std::stable_partition(apis.begin(), apis.end(), [](RtAudio::Api a) { return a == RtAudio::WINDOWS_ASIO; });
    for (auto a : apis) {
        std::string name = RtAudio::getApiDisplayName(a);
        if (a == RtAudio::WINDOWS_WASAPI) name += " Shared";
        out.push_back({name, false, a});
    }
    return out;
}

void AudioEngine::setDriver(const DriverChoice& d) {
    stop();
    exclusive_ = d.exclusive;
    if (!exclusive_) setApi(d.api);
}

void AudioEngine::setApi(RtAudio::Api api) {
    stop();
    rt_.reset();
    api_ = api;
    rt_ = std::make_unique<RtAudio>(api, [this](RtAudioErrorType, const std::string& msg) {
        std::lock_guard lock(errMutex_);
        lastError_ = msg;
    });
    rt_->showWarnings(false);
}

std::string AudioEngine::lastError() {
    std::lock_guard lock(errMutex_);
    return lastError_;
}

std::vector<DeviceInfo> AudioEngine::devices() {
    std::vector<DeviceInfo> out;
    if (exclusive_) {
        unsigned idx = 0;
        for (auto& ep : WasapiExclusive::enumerate()) {
            DeviceInfo d;
            d.id = idx++;
            d.endpointId = ep.id;
            d.name = ep.name;
            (ep.capture ? d.inputs : d.outputs) = ep.channels;
            d.sampleRates = ep.rates;
            d.preferredRate = 48000;
            d.isDefaultIn = ep.capture && ep.isDefault;
            d.isDefaultOut = !ep.capture && ep.isDefault;
            out.push_back(std::move(d));
        }
        return out;
    }
    if (!rt_) return out;
    for (unsigned id : rt_->getDeviceIds()) {
        RtAudio::DeviceInfo info = rt_->getDeviceInfo(id);
        DeviceInfo d;
        d.id = id;
        d.name = info.name;
        d.inputs = info.inputChannels;
        d.outputs = info.outputChannels;
        d.sampleRates = info.sampleRates;
        d.preferredRate = info.preferredSampleRate;
        d.isDefaultIn = info.isDefaultInput;
        d.isDefaultOut = info.isDefaultOutput;
        out.push_back(std::move(d));
    }
    return out;
}

bool AudioEngine::start(const StreamConfig& cfg) {
    stop();
    {
        std::lock_guard lock(errMutex_);
        lastError_.clear();
    }
    running_ = exclusive_ ? startExclusive(cfg) : startRtAudio(cfg);
    return running_;
}

bool AudioEngine::startExclusive(const StreamConfig& cfg) {
    WasapiExclusive::Config wc;
    wc.captureId = cfg.inEndpoint;
    wc.renderId = cfg.outEndpoint;
    wc.captureChannel = cfg.inChannel;
    wc.renderFirstChannel = cfg.outFirstChannel;
    wc.sampleRate = cfg.sampleRate;
    wc.captureMode = cfg.inMode;
    wc.renderMode = cfg.outMode;
    std::string err;
    if (!wasapi_.start(wc, &AudioEngine::wasapiPrepare, &AudioEngine::wasapiProcess, this, &params.jitterMs, err)) {
        std::lock_guard lock(errMutex_);
        lastError_ = err;
        return false;
    }
    const auto& info = wasapi_.info();
    sampleRate_ = info.renderRate;
    bufferFrames_ = info.renderPeriod;
    latencyFrames_ = static_cast<long>(info.estimatedLatencyMs * 0.001 * info.renderRate);
    return true;
}

bool AudioEngine::startRtAudio(const StreamConfig& cfg) {
    RtAudio::StreamParameters ip, op;
    ip.deviceId = cfg.inDevice;
    ip.nChannels = 1;
    ip.firstChannel = cfg.inChannel;
    op.deviceId = cfg.outDevice;
    op.nChannels = 2;
    op.firstChannel = cfg.outFirstChannel;

    RtAudio::StreamOptions opt;
    // Non-interleaved saves RtAudio an (de)interleave pass for ASIO, whose
    // native buffers are per channel anyway.
    opt.flags = RTAUDIO_NONINTERLEAVED | RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_MINIMIZE_LATENCY;
    opt.numberOfBuffers = 2;
    opt.streamName = "LowLatencyKaraoke";

    unsigned frames = cfg.bufferFrames;
    if (rt_->openStream(&op, &ip, RTAUDIO_FLOAT32, cfg.sampleRate, &frames, &AudioEngine::rtCallback, this, &opt)
        != RTAUDIO_NO_ERROR) {
        if (rt_->isStreamOpen()) rt_->closeStream();
        return false;
    }

    sampleRate_ = rt_->getStreamSampleRate();
    bufferFrames_ = frames;
    latencyFrames_ = rt_->getStreamLatency();
    prepare(static_cast<float>(sampleRate_), frames);

    if (rt_->startStream() != RTAUDIO_NO_ERROR) {
        rt_->closeStream();
        return false;
    }
    return true;
}

void AudioEngine::stop() {
    wasapi_.stop();
    if (rt_) {
        if (rt_->isStreamRunning()) rt_->stopStream();
        if (rt_->isStreamOpen()) rt_->closeStream();
    }
    running_ = false;
    ltState_.store(kLtIdle);
}

bool AudioEngine::latencyTestBusy() const { return ltState_.load(std::memory_order_acquire) != kLtIdle; }

void AudioEngine::prepare(float sr, unsigned maxFrames) {
    sr_ = sr;
    scratch_.assign(std::max(maxFrames, 4096u), 0.f);
    inGain_.prepare(sr, 20.f, dsp::dbToLin(params.inputGainDb.load()));
    outGain_.prepare(sr, 20.f, params.mute.load() ? 0.f : dsp::dbToLin(params.outputGainDb.load()));
    gate_.prepare(sr);
    for (auto* b : {&lowCut_, &lowShelf_, &midPeak_, &highShelf_}) { *b = dsp::Biquad{}; }
    std::fill(std::begin(eqCache_), std::end(eqCache_), -1.f);
    comp_.prepare(sr);
    leveler_.prepare(sr);
    limiter_.prepare(sr);
    echo_.prepare(sr, 2.0f);
    reverb_.prepare(sr);
    echoWasOn_ = reverbWasOn_ = false;
    meters.xruns.store(0);
    meters.cpuPeak.store(0);
}

int AudioEngine::rtCallback(void* out, void* in, unsigned int n, double, RtAudioStreamStatus status, void* user) {
    auto* self = static_cast<AudioEngine*>(user);
    float* outL = static_cast<float*>(out);
    float* outR = outL + n;
    if (!in) {
        std::fill(outL, outL + 2 * n, 0.f);
        return 0;
    }
    self->processBlock(static_cast<const float*>(in), outL, outR, n, status != 0);
    return 0;
}

void AudioEngine::wasapiPrepare(void* user, unsigned rate, unsigned maxFrames) {
    static_cast<AudioEngine*>(user)->prepare(static_cast<float>(rate), maxFrames);
}

void AudioEngine::wasapiProcess(void* user, const float* in, float* outL, float* outR, unsigned n, bool xrun) {
    static_cast<AudioEngine*>(user)->processBlock(in, outL, outR, n, xrun);
}

void AudioEngine::processBlock(const float* in, float* outL, float* outR, unsigned n, bool xrun) {
    const auto t0 = std::chrono::steady_clock::now();

    // Flush denormals to zero: decaying reverb/filter tails would otherwise
    // fall into denormal range and cost 100x per operation.
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040); // FTZ | DAZ

    meters.callbacks.fetch_add(1, kRelaxed);
    if (xrun) meters.xruns.fetch_add(1, kRelaxed);

    if (!runLatencyTest(in, outL, outR, n)) {
        const unsigned cap = static_cast<unsigned>(scratch_.size());
        for (unsigned off = 0; off < n; off += cap) {
            const unsigned c = std::min(cap, n - off);
            process(in + off, outL + off, outR + off, c);
        }
    }

    _mm_setcsr(csr);

    const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    const float load = elapsed * sr_ / static_cast<float>(n);
    meters.cpuLoad.store(load, kRelaxed);
    storeMax(meters.cpuPeak, load);
}

void AudioEngine::process(const float* in, float* outL, float* outR, unsigned n) {
    using namespace dsp;
    Params& p = params;
    float* x = scratch_.data();

    // ---- input gain -------------------------------------------------------
    const float gIn = dbToLin(p.inputGainDb.load(kRelaxed));
    float inPk = 0.f;
    for (unsigned k = 0; k < n; ++k) {
        inPk = std::max(inPk, std::fabs(in[k]));
        x[k] = in[k] * inGain_.next(gIn);
    }
    storeMax(meters.inPeak, inPk);

    // ---- noise gate -------------------------------------------------------
    if (p.gateOn.load(kRelaxed)) {
        const float th = dbToLin(p.gateThreshDb.load(kRelaxed));
        for (unsigned k = 0; k < n; ++k) x[k] = gate_.process(x[k], th);
        meters.gateGain.store(gate_.gain, kRelaxed);
    } else {
        meters.gateGain.store(1.f, kRelaxed);
    }

    // ---- EQ ---------------------------------------------------------------
    if (p.eqOn.load(kRelaxed)) {
        const float v[5] = {p.eqLowCutHz.load(kRelaxed), p.eqLowDb.load(kRelaxed), p.eqMidDb.load(kRelaxed),
                            p.eqMidHz.load(kRelaxed), p.eqHighDb.load(kRelaxed)};
        if (v[0] != eqCache_[0]) lowCut_.setHighPass(sr_, v[0]);
        if (v[1] != eqCache_[1]) lowShelf_.setLowShelf(sr_, 200.f, v[1]);
        if (v[2] != eqCache_[2] || v[3] != eqCache_[3]) midPeak_.setPeak(sr_, v[3], 1.0f, v[2]);
        if (v[4] != eqCache_[4]) highShelf_.setHighShelf(sr_, 8000.f, v[4]);
        std::copy(v, v + 5, eqCache_);
        for (unsigned k = 0; k < n; ++k)
            x[k] = highShelf_.process(midPeak_.process(lowShelf_.process(lowCut_.process(x[k]))));
    }

    // ---- compressor -------------------------------------------------------
    if (p.compOn.load(kRelaxed)) {
        const float th = p.compThreshDb.load(kRelaxed);
        const float slope = 1.f - 1.f / std::max(1.f, p.compRatio.load(kRelaxed));
        const float makeup = dbToLin(p.compMakeupDb.load(kRelaxed));
        for (unsigned k = 0; k < n; ++k) x[k] = comp_.process(x[k], th, slope) * makeup;
        meters.compGainDb.store(linToDb(comp_.lastGain), kRelaxed);
    } else {
        meters.compGainDb.store(0.f, kRelaxed);
    }

    // ---- voice boost ------------------------------------------------------
    const float boost = std::clamp(p.voiceBoostDb.load(kRelaxed), 0.f, 18.f);
    if (boost > 0.05f) {
        for (unsigned k = 0; k < n; ++k) x[k] = leveler_.process(x[k], boost);
        meters.boostGainDb.store(leveler_.lastGainDb, kRelaxed);
    } else {
        meters.boostGainDb.store(0.f, kRelaxed);
    }

    // ---- echo + reverb + output ------------------------------------------
    const bool echoOn = p.echoOn.load(kRelaxed);
    const bool revOn = p.reverbOn.load(kRelaxed);
    if (echoOn && !echoWasOn_) echo_.clear();
    if (revOn && !reverbWasOn_) reverb_.clear();
    echoWasOn_ = echoOn;
    reverbWasOn_ = revOn;

    const float echoSamples = std::min(p.echoMs.load(kRelaxed) * 0.001f * sr_, sr_ * 1.9f);
    const float echoFb = std::clamp(p.echoFeedback.load(kRelaxed), 0.f, 0.95f);
    const float echoMix = p.echoMix.load(kRelaxed);
    const float revSize = p.reverbSize.load(kRelaxed);
    const float revDamp = p.reverbDamp.load(kRelaxed);
    const float revMix = p.reverbMix.load(kRelaxed) * 3.f; // Freeverb wet scale
    const float gOut = p.mute.load(kRelaxed) ? 0.f : dbToLin(p.outputGainDb.load(kRelaxed));

    const bool tone = p.testTone.load(kRelaxed);
    const float toneInc = 2.f * kPi * 440.f / sr_;

    float outPk = 0.f;
    for (unsigned k = 0; k < n; ++k) {
        const float dry = x[k];
        float l = dry, r = dry;
        if (tone) {
            const float t = 0.1f * std::sin(tonePhase_);
            tonePhase_ += toneInc;
            if (tonePhase_ > 2.f * kPi) tonePhase_ -= 2.f * kPi;
            l += t; r += t;
        }
        if (echoOn) {
            const float e = echo_.process(dry, echoSamples, echoFb) * echoMix;
            l += e; r += e;
        }
        if (revOn) {
            float rl, rr;
            reverb_.process(dry, revSize, revDamp, rl, rr);
            l += rl * revMix; r += rr * revMix;
        }
        const float g = outGain_.next(gOut);
        l *= g;
        r *= g;
        limiter_.process(l, r, 0.891f); // -1 dBFS ceiling, no added latency
        l = safetyClip(l);              // backstop only; never reached after the limiter
        r = safetyClip(r);
        outL[k] = l;
        outR[k] = r;
        outPk = std::max(outPk, std::max(std::fabs(l), std::fabs(r)));
    }
    storeMax(meters.outPeak, outPk);
    meters.limiterGainDb.store(linToDb(limiter_.gain), kRelaxed);
}

bool AudioEngine::runLatencyTest(const float* in, float* outL, float* outR, unsigned n) {
    int st = ltState_.load(std::memory_order_acquire);
    if (st == kLtIdle) return false;
    if (st == kLtRequested) {
        ltCount_ = 0;
        ltPulseAt_ = static_cast<long>(sr_ * 0.3f); // 300 ms of silence first
        ltNoise_ = 0.f;
        ltResult_.store(0, std::memory_order_release);
        ltState_.store(kLtRunning, std::memory_order_release);
    }

    const long timeout = ltPulseAt_ + static_cast<long>(sr_);
    bool done = false;
    for (unsigned k = 0; k < n; ++k, ++ltCount_) {
        float o = 0.f;
        if (!done) {
            const float a = std::fabs(in[k]);
            if (ltCount_ < ltPulseAt_) {
                if (ltCount_ > ltPulseAt_ / 2) ltNoise_ = std::max(ltNoise_, a); // learn noise floor
            } else {
                const long rel = ltCount_ - ltPulseAt_;
                if (rel < 4) o = 0.9f;
                const float th = std::clamp(ltNoise_ * 4.f, 0.02f, 0.5f);
                if (rel > 0 && a > th) {
                    ltResult_.store(rel, std::memory_order_release);
                    done = true;
                } else if (ltCount_ >= timeout) {
                    ltResult_.store(-1, std::memory_order_release);
                    done = true;
                }
            }
        }
        outL[k] = o;
        outR[k] = o;
    }
    if (done) ltState_.store(kLtIdle, std::memory_order_release);
    return true;
}

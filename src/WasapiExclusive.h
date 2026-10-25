// Native WASAPI exclusive-mode duplex for a separate mic and headphone device.
//
// Each device is opened in exclusive, event-driven mode at its minimum period
// (bypassing the Windows audio engine and its ~10 ms mixing period). One
// MMCSS "Pro Audio" thread services both. Because the two devices run on
// independent clocks, captured audio passes through a small FIFO that is read
// with a slowly-adapting fractional rate (PI-controlled on FIFO fill), which
// also converts between different device sample rates.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <vector>

// Auto = exclusive if the device is free, otherwise shared alongside other apps.
enum class WasapiMode { Auto, Exclusive, Shared };

struct WasapiEndpoint {
    std::wstring id;
    std::string name;
    bool capture = false;
    bool isDefault = false;
    bool exclusiveBlocked = false; // "Allow applications to take exclusive control" is off
    unsigned channels = 0;
    std::vector<unsigned> rates; // rates usable in exclusive mode
};

class WasapiExclusive {
public:
    // Audio thread, once per render period: `n` frames of mic audio already on
    // the render clock -> fill outL/outR.
    using ProcessFn = void (*)(void* user, const float* inMono, float* outL, float* outR, unsigned n, bool xrun);
    // Audio thread, after the devices are opened and before they start.
    using PrepareFn = void (*)(void* user, unsigned renderRate, unsigned maxFrames);

    struct Config {
        std::wstring captureId, renderId;
        unsigned captureChannel = 0;
        unsigned renderFirstChannel = 0;
        unsigned sampleRate = 48000;
        WasapiMode captureMode = WasapiMode::Auto;
        WasapiMode renderMode = WasapiMode::Shared; // keep other apps' audio (the song!) playing
    };

    struct Info {
        unsigned captureRate = 0, renderRate = 0;
        unsigned captureBits = 0, renderBits = 0;
        unsigned captureChannels = 0, renderChannels = 0;
        unsigned capturePeriod = 0, renderPeriod = 0; // frames
        bool captureExclusive = false, renderExclusive = false;
        double estimatedLatencyMs = 0;
    };

    ~WasapiExclusive() { stop(); }

    // Call from a COM-initialised thread.
    static std::vector<WasapiEndpoint> enumerate();

    bool start(const Config& cfg, PrepareFn prepare, ProcessFn process, void* user,
               const std::atomic<float>* jitterMs, std::string& error);
    void stop();
    bool running() const { return thread_.joinable(); }
    const Info& info() const { return info_; }

    std::atomic<float> fifoMs{0.f};      // current jitter-buffer fill
    std::atomic<float> ratePpm{0.f};     // drift correction being applied
    std::atomic<float> autoJitterMs{0.f}; // extra margin added after underruns
    std::atomic<bool> deviceLost{false};
    std::atomic<uint64_t> capturedFrames{0}; // total mic frames received

private:
    struct Side;
    void run(Config cfg, std::promise<std::string>* ready);
    void session(const Config& cfg, std::promise<std::string>* ready);
    void drainCapture(Side& cap);
    bool pullResampled(float* out, unsigned n);
    double targetFill() const;

    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    Info info_;
    PrepareFn prepare_ = nullptr;
    ProcessFn process_ = nullptr;
    void* user_ = nullptr;
    const std::atomic<float>* jitterMs_ = nullptr;

    // ---- audio-thread state ----
    unsigned capChannel_ = 0, renFirst_ = 0;
    std::vector<float> ring_;
    size_t ringMask_ = 0;
    uint64_t written_ = 0;
    double readPos_ = 0, baseStep_ = 1, avgFill_ = 0, integ_ = 0;
    double autoExtra_ = 0, stableSec_ = 0; // adaptive margin (capture frames)
    bool primed_ = false, overflowed_ = false;
    std::vector<float> inBuf_, outL_, outR_;
};

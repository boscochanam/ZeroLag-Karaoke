#include "WasapiExclusive.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT and PKEY_Device_FriendlyName, defined
// locally to avoid INITGUID games.
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

enum class SampleKind { F32, I32, I24, I16 };

struct FormatCandidate { WORD bits, valid; SampleKind kind; };
constexpr FormatCandidate kFormats[] = {
    {32, 32, SampleKind::F32}, {32, 24, SampleKind::I32}, {32, 32, SampleKind::I32},
    {24, 24, SampleKind::I24}, {16, 16, SampleKind::I16},
};
constexpr unsigned kRates[] = {44100, 48000, 88200, 96000, 176400, 192000};

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

WAVEFORMATEXTENSIBLE makeFormat(unsigned rate, unsigned channels, const FormatCandidate& c) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = c.bits;
    f.Format.nBlockAlign = static_cast<WORD>(channels * c.bits / 8);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = c.valid;
    f.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                    : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                    : 0;
    f.SubFormat = c.kind == SampleKind::F32 ? kSubtypeFloat : kSubtypePcm;
    return f;
}

// S_OK when found; AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED if the device forbids
// exclusive access entirely; AUDCLNT_E_UNSUPPORTED_FORMAT otherwise.
HRESULT findFormat(IAudioClient* c, unsigned rate, unsigned channels, WAVEFORMATEXTENSIBLE& out, SampleKind& kind) {
    for (const auto& cand : kFormats) {
        WAVEFORMATEXTENSIBLE f = makeFormat(rate, channels, cand);
        const HRESULT hr = c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, nullptr);
        if (hr == S_OK) {
            out = f;
            kind = cand.kind;
            return S_OK;
        }
        if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) return hr;
    }
    return AUDCLNT_E_UNSUPPORTED_FORMAT;
}

std::string describe(HRESULT hr, const char* what) {
    std::string s = std::string(what) + ": ";
    switch (hr) {
    case AUDCLNT_E_DEVICE_IN_USE:
        return s + "the device is in use. Close other apps using it (Discord, browser, OBS, the Sound control panel) "
                   "or switch them to another device.";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        return s + "exclusive mode is disabled for this device. Win+R > mmsys.cpl > (Recording or Playback tab) > "
                   "the device > Properties > Advanced > tick 'Allow applications to take exclusive control'.";
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return s + "no supported exclusive-mode format.";
    case AUDCLNT_E_DEVICE_INVALIDATED:
        return s + "device was unplugged or disabled.";
    case AUDCLNT_E_CPUUSAGE_EXCEEDED:
        return s + "CPU usage exceeded.";
    default: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "WASAPI error 0x%08lX", static_cast<unsigned long>(hr));
        return s + buf;
    }
    }
}

inline float readSample(const BYTE* p, SampleKind k) {
    switch (k) {
    case SampleKind::F32: { float v; std::memcpy(&v, p, 4); return v; }
    case SampleKind::I32: { int32_t v; std::memcpy(&v, p, 4); return static_cast<float>(v) * (1.f / 2147483648.f); }
    case SampleKind::I24: {
        const int32_t v = static_cast<int32_t>((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24));
        return static_cast<float>(v) * (1.f / 2147483648.f);
    }
    case SampleKind::I16: { int16_t v; std::memcpy(&v, p, 2); return static_cast<float>(v) * (1.f / 32768.f); }
    }
    return 0.f;
}

inline void writeSample(BYTE* p, SampleKind k, float x) {
    x = std::clamp(x, -1.f, 1.f);
    switch (k) {
    case SampleKind::F32: std::memcpy(p, &x, 4); break;
    case SampleKind::I32: { const int32_t v = static_cast<int32_t>(static_cast<double>(x) * 2147483647.0); std::memcpy(p, &v, 4); break; }
    case SampleKind::I24: {
        const int32_t v = static_cast<int32_t>(x * 8388607.f);
        p[0] = BYTE(v); p[1] = BYTE(v >> 8); p[2] = BYTE(v >> 16);
        break;
    }
    case SampleKind::I16: { const int16_t v = static_cast<int16_t>(x * 32767.f); std::memcpy(p, &v, 2); break; }
    }
}

unsigned validBits(const WAVEFORMATEXTENSIBLE& f) {
    return f.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE ? f.Samples.wValidBitsPerSample : f.Format.wBitsPerSample;
}

} // namespace

// One opened endpoint.
struct WasapiExclusive::Side {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
    HANDLE event = nullptr;
    WAVEFORMATEXTENSIBLE fmt{};
    SampleKind kind = SampleKind::F32;
    unsigned channels = 0, rate = 0, bytesPerSample = 0, bytesPerFrame = 0;
    UINT32 bufferFrames = 0; // whole endpoint buffer
    UINT32 periodFrames = 0; // one device/engine period
    bool exclusive = false;

    ~Side() {
        if (client) client->Stop();
        if (event) CloseHandle(event);
    }

    std::string open(IMMDeviceEnumerator* en, const std::wstring& id, unsigned wantRate, WasapiMode mode,
                     const char* what) {
        ComPtr<IMMDevice> dev;
        HRESULT hr = en->GetDevice(id.c_str(), &dev);
        if (FAILED(hr)) return describe(hr, what);

        if (mode != WasapiMode::Shared) {
            hr = openExclusive(dev.Get(), wantRate);
            if (SUCCEEDED(hr)) {
                exclusive = true;
                return finish();
            }
            client.Reset();
            // Auto: if another app holds the device (or exclusive is disabled
            // or unsupported), share it instead of failing.
            const bool fallBack = hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED ||
                                  hr == AUDCLNT_E_UNSUPPORTED_FORMAT;
            if (mode == WasapiMode::Exclusive || !fallBack) return describe(hr, what);
        }
        hr = openShared(dev.Get());
        if (FAILED(hr)) { client.Reset(); return describe(hr, what); }
        exclusive = false;
        return finish();
    }

private:
    std::string finish() {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        client->SetEventHandle(event);
        client->GetBufferSize(&bufferFrames);
        if (periodFrames == 0 || periodFrames > bufferFrames) periodFrames = bufferFrames;
        return {};
    }

    // Shared mode: the Windows audio engine mixes us with other apps. Use
    // IAudioClient3's smallest engine period where the driver allows it.
    HRESULT openShared(IMMDevice* dev) {
        WAVEFORMATEX* mix = nullptr;
        ComPtr<IAudioClient3> c3;
        HRESULT hr = dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &c3);
        if (SUCCEEDED(hr)) {
            hr = c3->GetMixFormat(&mix);
            if (FAILED(hr)) return hr;
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            hr = c3->GetSharedModeEnginePeriod(mix, &def, &fund, &mn, &mx);
            if (SUCCEEDED(hr)) hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mix, nullptr);
            if (SUCCEEDED(hr)) {
                // Another app may already be running the engine at a larger period.
                WAVEFORMATEX* cur = nullptr;
                UINT32 curPeriod = mn;
                if (SUCCEEDED(c3->GetCurrentSharedModeEnginePeriod(&cur, &curPeriod))) CoTaskMemFree(cur);
                periodFrames = curPeriod;
                client = c3;
            }
        }
        if (!client) {
            // Plain shared mode (engine default period, ~10 ms).
            if (mix) { CoTaskMemFree(mix); mix = nullptr; }
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
            if (FAILED(hr)) return hr;
            hr = client->GetMixFormat(&mix);
            if (FAILED(hr)) return hr;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
            if (FAILED(hr)) { CoTaskMemFree(mix); return hr; }
            REFERENCE_TIME defPeriod = 0, minPeriod = 0;
            client->GetDevicePeriod(&defPeriod, &minPeriod);
            periodFrames = static_cast<UINT32>(defPeriod * mix->nSamplesPerSec / 10000000);
        }

        std::memcpy(&fmt, mix, std::min<size_t>(sizeof(fmt), sizeof(WAVEFORMATEX) + mix->cbSize));
        const bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                             (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt.SubFormat == kSubtypeFloat);
        kind = isFloat ? SampleKind::F32
             : mix->wBitsPerSample == 16 ? SampleKind::I16
             : mix->wBitsPerSample == 24 ? SampleKind::I24
                                         : SampleKind::I32;
        channels = mix->nChannels;
        rate = mix->nSamplesPerSec;
        bytesPerSample = mix->wBitsPerSample / 8;
        bytesPerFrame = mix->nBlockAlign;
        CoTaskMemFree(mix);
        return S_OK;
    }

    HRESULT openExclusive(IMMDevice* dev, unsigned wantRate) {
        HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (FAILED(hr)) return hr;

        WAVEFORMATEX* mix = nullptr;
        unsigned mixCh = 2, mixRate = 48000;
        if (SUCCEEDED(client->GetMixFormat(&mix))) { mixCh = mix->nChannels; mixRate = mix->nSamplesPerSec; CoTaskMemFree(mix); }

        // Prefer the requested rate so no rate conversion is needed; fall back
        // to whatever the device supports.
        HRESULT fr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        for (unsigned r : {wantRate, mixRate, 48000u, 44100u, 96000u}) {
            for (unsigned ch : {mixCh, 2u, 1u}) {
                fr = findFormat(client.Get(), r, ch, fmt, kind);
                if (fr != AUDCLNT_E_UNSUPPORTED_FORMAT) break;
            }
            if (fr != AUDCLNT_E_UNSUPPORTED_FORMAT) break;
        }
        if (fr != S_OK) return fr;
        channels = fmt.Format.nChannels;
        rate = fmt.Format.nSamplesPerSec;
        bytesPerSample = fmt.Format.wBitsPerSample / 8;
        bytesPerFrame = fmt.Format.nBlockAlign;

        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        client->GetDevicePeriod(&defPeriod, &minPeriod);
        hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, minPeriod,
                                &fmt.Format, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // Documented dance: get the aligned size, re-activate, re-init.
            UINT32 frames = 0;
            client->GetBufferSize(&frames);
            const REFERENCE_TIME aligned = static_cast<REFERENCE_TIME>(1e7 * frames / rate + 0.5);
            client.Reset();
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
            if (FAILED(hr)) return hr;
            hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, aligned, aligned,
                                    &fmt.Format, nullptr);
        }
        periodFrames = 0; // exclusive event mode: period == whole buffer
        return hr;
    }
};

std::vector<WasapiEndpoint> WasapiExclusive::enumerate() {
    std::vector<WasapiEndpoint> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;

    for (EDataFlow flow : {eCapture, eRender}) {
        std::wstring defId;
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &def))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id))) { defId = id; CoTaskMemFree(id); }
        }
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) continue;
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(coll->Item(i, &dev))) continue;
            WasapiEndpoint ep;
            ep.capture = flow == eCapture;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id))) { ep.id = id; CoTaskMemFree(id); }
            ep.isDefault = ep.id == defId;
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) ep.name = narrow(v.pwszVal);
                PropVariantClear(&v);
            }
            ComPtr<IAudioClient> c;
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c))) continue;
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(c->GetMixFormat(&mix))) { ep.channels = mix->nChannels; CoTaskMemFree(mix); }
            for (unsigned r : kRates) {
                WAVEFORMATEXTENSIBLE f;
                SampleKind k;
                HRESULT hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
                for (unsigned ch : {ep.channels, 2u, 1u}) {
                    hr = findFormat(c.Get(), r, ch, f, k);
                    if (hr != AUDCLNT_E_UNSUPPORTED_FORMAT) break;
                }
                if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) { ep.exclusiveBlocked = true; break; }
                if (hr == S_OK) ep.rates.push_back(r);
            }
            if (ep.exclusiveBlocked) ep.name += "  [exclusive mode disabled in Windows]";
            out.push_back(std::move(ep));
        }
    }
    return out;
}

bool WasapiExclusive::start(const Config& cfg, PrepareFn prepare, ProcessFn process, void* user,
                            const std::atomic<float>* jitterMs, std::string& error) {
    stop();
    prepare_ = prepare;
    process_ = process;
    user_ = user;
    jitterMs_ = jitterMs;
    deviceLost.store(false);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::promise<std::string> ready;
    auto fut = ready.get_future();
    thread_ = std::thread(&WasapiExclusive::run, this, cfg, &ready);
    error = fut.get();
    if (!error.empty()) {
        thread_.join();
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        return false;
    }
    return true;
}

void WasapiExclusive::stop() {
    if (!thread_.joinable()) return;
    SetEvent(stopEvent_);
    thread_.join();
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
}

double WasapiExclusive::targetFill() const {
    // Captured audio arrives a period at a time and render consumes a period
    // at a time, so the FIFO must on average hold half a capture period plus a
    // render period, plus a user-tunable safety margin for scheduling jitter.
    const double capRate = info_.captureRate;
    const double jitter = jitterMs_ ? std::max(0.f, jitterMs_->load(std::memory_order_relaxed)) : 1.0;
    return 0.5 * info_.capturePeriod + info_.renderPeriod * baseStep_ + jitter * 0.001 * capRate + autoExtra_ + 2.0;
}

void WasapiExclusive::drainCapture(Side& cap) {
    UINT32 packet = 0;
    while (SUCCEEDED(cap.capture->GetNextPacketSize(&packet)) && packet > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        const HRESULT hr = cap.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { deviceLost.store(true); return; }
        if (FAILED(hr)) return;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) overflowed_ = true;
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const BYTE* p = data + capChannel_ * cap.bytesPerSample;
        for (UINT32 f = 0; f < frames; ++f, p += cap.bytesPerFrame)
            ring_[(written_ + f) & ringMask_] = silent ? 0.f : readSample(p, cap.kind);
        written_ += frames;
        cap.capture->ReleaseBuffer(frames);
    }
    capturedFrames.store(written_, std::memory_order_relaxed);
}

bool WasapiExclusive::pullResampled(float* out, unsigned n) {
    const double T = targetFill();
    double fill = static_cast<double>(written_) - readPos_;
    bool xrun = overflowed_;
    overflowed_ = false;

    if (!primed_) {
        if (fill < T + 2) {
            std::fill(out, out + n, 0.f);
            fifoMs.store(static_cast<float>(fill * 1000.0 / info_.captureRate), std::memory_order_relaxed);
            return xrun;
        }
        primed_ = true;
        readPos_ = static_cast<double>(written_) - T;
        avgFill_ = T;
        integ_ = 0;
        fill = T;
    }

    // Way too much buffered (e.g. render stalled): jump back to the target.
    const double maxFill = std::min(T + 0.05 * info_.captureRate, 0.5 * static_cast<double>(ring_.size()));
    if (fill > maxFill) {
        readPos_ = static_cast<double>(written_) - T;
        avgFill_ = fill = T;
        integ_ = 0;
        xrun = true;
    }

    // PI controller on (smoothed) FIFO fill -> tiny playback-rate trim. This
    // absorbs the clock drift between two independent USB devices.
    // Fill is a sawtooth (packets arrive in chunks), so average over ~0.5 s
    // before steering; gains give a critically-ish damped ~0.3 rad/s loop.
    constexpr double kP = 0.5, kI = 0.1, kMaxAdj = 0.005, kAvgSec = 0.5;
    const double dt = static_cast<double>(n) / info_.renderRate;
    avgFill_ += (1.0 - std::exp(-dt / kAvgSec)) * (fill - avgFill_);
    const double errSec = (avgFill_ - T) / info_.captureRate;
    integ_ = std::clamp(integ_ + kI * errSec * dt, -kMaxAdj, kMaxAdj);
    const double adj = std::clamp(kP * errSec + integ_, -kMaxAdj, kMaxAdj);
    const double step = baseStep_ * (1.0 + adj);

    const double end = static_cast<double>(written_) - 1.0;
    for (unsigned k = 0; k < n; ++k) {
        if (readPos_ >= end) {
            // Underrun: pad with silence, grow the adaptive margin (some
            // devices deliver in bursts far larger than their period) and
            // re-prime.
            std::fill(out + k, out + n, 0.f);
            primed_ = false;
            xrun = true;
            autoExtra_ = std::min(autoExtra_ + 0.002 * info_.captureRate, 0.1 * info_.captureRate);
            stableSec_ = 0;
            break;
        }
        const uint64_t i0 = static_cast<uint64_t>(readPos_);
        const float fr = static_cast<float>(readPos_ - static_cast<double>(i0));
        const float a = ring_[i0 & ringMask_], b = ring_[(i0 + 1) & ringMask_];
        out[k] = a + fr * (b - a);
        readPos_ += step;
    }

    // After 5 s without trouble, give the adaptive margin back at 0.1 ms/s;
    // the rate controller follows the shrinking target smoothly.
    if (primed_) {
        stableSec_ += dt;
        if (stableSec_ > 5.0 && autoExtra_ > 0) autoExtra_ = std::max(0.0, autoExtra_ - 0.0001 * info_.captureRate * dt);
    }

    fifoMs.store(static_cast<float>(fill * 1000.0 / info_.captureRate), std::memory_order_relaxed);
    ratePpm.store(static_cast<float>(adj * 1e6), std::memory_order_relaxed);
    autoJitterMs.store(static_cast<float>(autoExtra_ * 1000.0 / info_.captureRate), std::memory_order_relaxed);
    return xrun;
}

void WasapiExclusive::run(Config cfg, std::promise<std::string>* ready) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    session(cfg, ready); // all COM objects are released inside
    CoUninitialize();
}

void WasapiExclusive::session(const Config& cfg, std::promise<std::string>* ready) {
    {
        ComPtr<IMMDeviceEnumerator> en;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (FAILED(hr)) {
            ready->set_value(describe(hr, "Device enumerator"));
            return;
        }

        Side cap, ren;
        std::string err = cap.open(en.Get(), cfg.captureId, cfg.sampleRate, cfg.captureMode, "Mic");
        if (err.empty()) err = ren.open(en.Get(), cfg.renderId, cfg.sampleRate, cfg.renderMode, "Headphones");
        if (err.empty()) {
            HRESULT h = cap.client->GetService(IID_PPV_ARGS(&cap.capture));
            if (SUCCEEDED(h)) h = ren.client->GetService(IID_PPV_ARGS(&ren.render));
            if (FAILED(h)) err = describe(h, "GetService");
        }
        if (err.empty() && cfg.captureChannel >= cap.channels) err = "Mic channel is out of range for this device.";
        if (err.empty() && cfg.renderFirstChannel >= ren.channels) err = "Headphone channel is out of range.";
        if (!err.empty()) {
            ready->set_value(err);
            return;
        }

        capChannel_ = cfg.captureChannel;
        renFirst_ = ren.channels >= 2 ? std::min(cfg.renderFirstChannel, ren.channels - 2) : 0;
        info_.captureRate = cap.rate;
        info_.renderRate = ren.rate;
        info_.captureBits = validBits(cap.fmt);
        info_.renderBits = validBits(ren.fmt);
        info_.captureExclusive = cap.exclusive;
        info_.renderExclusive = ren.exclusive;
        info_.captureChannels = cap.channels;
        info_.renderChannels = ren.channels;
        info_.capturePeriod = cap.periodFrames;
        info_.renderPeriod = ren.periodFrames;
        baseStep_ = static_cast<double>(cap.rate) / ren.rate;
        // Capture period + FIFO + two render periods (+ one engine period per
        // shared side). The Measure button gives the real number including
        // USB and converters.
        info_.estimatedLatencyMs =
            1000.0 * ((cap.exclusive ? 1.0 : 2.0) * cap.periodFrames / cap.rate + targetFill() / cap.rate +
                      (ren.exclusive ? 2.0 : 3.0) * ren.periodFrames / ren.rate);

        // Ring of ~0.35 s at the capture rate (power of two).
        size_t ringSize = 1;
        while (ringSize < cap.rate / 3) ringSize <<= 1;
        ring_.assign(ringSize, 0.f);
        ringMask_ = ringSize - 1;
        written_ = 0;
        readPos_ = 0;
        primed_ = overflowed_ = false;
        avgFill_ = integ_ = autoExtra_ = stableSec_ = 0;
        inBuf_.assign(ren.bufferFrames, 0.f);
        outL_.assign(ren.bufferFrames, 0.f);
        outR_.assign(ren.bufferFrames, 0.f);
        if (prepare_) prepare_(user_, ren.rate, ren.bufferFrames);

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);

        // Pre-roll silence (a whole buffer in exclusive mode, one period in
        // shared mode), then start both devices.
        BYTE* data = nullptr;
        const UINT32 preroll = ren.exclusive ? ren.bufferFrames : ren.periodFrames;
        if (SUCCEEDED(ren.render->GetBuffer(preroll, &data))) ren.render->ReleaseBuffer(preroll, AUDCLNT_BUFFERFLAGS_SILENT);
        HRESULT h = cap.client->Start();
        if (SUCCEEDED(h)) h = ren.client->Start();
        if (FAILED(h)) {
            ready->set_value(describe(h, "Start"));
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            return;
        }
        ready->set_value({}); // `ready` is dead after this line

        const unsigned rch = ren.channels, rbps = ren.bytesPerSample, rbpf = ren.bytesPerFrame;
        const HANDLE waits[3] = {stopEvent_, cap.event, ren.event};
        for (;;) {
            const DWORD w = WaitForMultipleObjects(3, waits, FALSE, 500);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_TIMEOUT) continue; // UI shows "no audio flowing"
            drainCapture(cap);
            if (deviceLost.load(std::memory_order_relaxed)) break;
            if (w != WAIT_OBJECT_0 + 2) continue;

            // Exclusive: refill the whole device buffer each event. Shared:
            // keep just two engine periods queued so latency stays minimal.
            UINT32 n = ren.bufferFrames;
            if (!ren.exclusive) {
                UINT32 padding = 0;
                if (ren.client->GetCurrentPadding(&padding) == AUDCLNT_E_DEVICE_INVALIDATED) {
                    deviceLost.store(true);
                    break;
                }
                const UINT32 want = 2 * ren.periodFrames;
                n = padding >= want ? 0 : std::min(want - padding, ren.bufferFrames - padding);
                if (n == 0) continue;
            }
            const HRESULT gh = ren.render->GetBuffer(n, &data);
            if (gh == AUDCLNT_E_DEVICE_INVALIDATED) { deviceLost.store(true); break; }
            if (FAILED(gh)) continue;
            const bool xrun = pullResampled(inBuf_.data(), n);
            process_(user_, inBuf_.data(), outL_.data(), outR_.data(), n, xrun);

            if (rch > 2) std::memset(data, 0, size_t(n) * rbpf);
            BYTE* p = data + renFirst_ * rbps;
            if (rch == 1) {
                for (UINT32 f = 0; f < n; ++f, p += rbpf) writeSample(p, ren.kind, 0.5f * (outL_[f] + outR_[f]));
            } else {
                for (UINT32 f = 0; f < n; ++f, p += rbpf) {
                    writeSample(p, ren.kind, outL_[f]);
                    writeSample(p + rbps, ren.kind, outR_[f]);
                }
            }
            ren.render->ReleaseBuffer(n, 0);
        }

        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }
}

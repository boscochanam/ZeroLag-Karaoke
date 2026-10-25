// Low Latency Karaoke – mic -> effects -> headphones with the smallest
// possible buffer. Dear ImGui (Win32 + DX11) front end; the audio path lives
// entirely in AudioEngine and never waits on this thread.
#include <windows.h>
#include <d3d11.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "AudioEngine.h"

// ---------------------------------------------------------------------------
// D3D11 boilerplate (adapted from the Dear ImGui example)
// ---------------------------------------------------------------------------
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static bool g_occluded = false;
static UINT g_resizeW = 0, g_resizeH = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

static void CleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swapChain, &g_device, &got, &g_context);
    if (hr != S_OK) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_resizeW = LOWORD(lParam);
        g_resizeH = HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // no ALT menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Process-level tweaks for steady real-time scheduling
// ---------------------------------------------------------------------------
static void TuneProcessForAudio() {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    // Opt out of EcoQoS: otherwise Windows 11 may park us on efficiency cores
    // or clock us down whenever the window is in the background.
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
namespace ui {

constexpr auto kRelaxed = std::memory_order_relaxed;

bool Slider(const char* label, std::atomic<float>& a, float lo, float hi, const char* fmt,
            ImGuiSliderFlags flags = 0) {
    float v = a.load(kRelaxed);
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, fmt, flags);
    if (changed) a.store(v, kRelaxed);
    return changed;
}

void HelpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool Check(const char* label, std::atomic<bool>& a) {
    bool v = a.load(kRelaxed);
    const bool changed = ImGui::Checkbox(label, &v);
    if (changed) a.store(v, kRelaxed);
    return changed;
}

// Level meter with fast attack / slow fall; `peak` is the max since last frame.
void Meter(const char* label, float& shown, float peak) {
    shown = std::max(peak, shown * 0.9f);
    const float db = dsp::linToDb(shown);
    const float frac = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
    const ImVec4 col = db > -3.f ? ImVec4(0.90f, 0.25f, 0.20f, 1) : db > -12.f ? ImVec4(0.95f, 0.75f, 0.20f, 1)
                                                                              : ImVec4(0.30f, 0.80f, 0.40f, 1);
    char text[48];
    if (db <= -59.f) std::snprintf(text, sizeof text, "%s  -inf", label);
    else std::snprintf(text, sizeof text, "%s  %.1f dB", label, db);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(frac, ImVec2(-1, 0), text);
    ImGui::PopStyleColor();
}

struct ModeChoice { WasapiMode mode; const char* label; };
constexpr ModeChoice kModes[] = {
    {WasapiMode::Auto, "Auto (exclusive if free)"},
    {WasapiMode::Exclusive, "Exclusive only"},
    {WasapiMode::Shared, "Shared (other apps keep working)"},
};

void modeCombo(const char* label, int& idx) {
    if (ImGui::BeginCombo(label, kModes[idx].label)) {
        for (int i = 0; i < (int)std::size(kModes); ++i)
            if (ImGui::Selectable(kModes[i].label, i == idx)) idx = i;
        ImGui::EndCombo();
    }
}

struct BufferChoice { unsigned frames; const char* label; };
constexpr BufferChoice kBuffers[] = {
    {1, "Driver minimum"}, {0, "Driver preferred"}, {16, "16"}, {32, "32"}, {48, "48"}, {64, "64"},
    {96, "96"}, {128, "128"}, {192, "192"}, {256, "256"}, {512, "512"},
};

struct App {
    AudioEngine engine;
    std::vector<DriverChoice> drivers = AudioEngine::drivers();
    int driverIdx = 0; // engine starts on drivers[0]
    std::vector<DeviceInfo> devs;
    int inDev = -1, outDev = -1;
    int inCh = 0, outPair = 0;
    unsigned rate = 48000;
    int bufIdx = 0;
    // Index into kModes. Mic: Auto; headphones: Shared so the song keeps playing.
    int micMode = 0, hpMode = 2;
    std::string error;
    float inShown = 0, outShown = 0;
    uint64_t lastCallbacks = 0, lastCaptured = 0;
    double lastCallbackTime = 0, lastCaptureTime = 0;

    static constexpr int kFirstFixedBuffer = 2; // entries before this are ASIO-only
    static constexpr int kWasapiDefaultBuffer = 3; // 32 frames

    bool isAsio() const { return engine.api() == RtAudio::WINDOWS_ASIO; }

    void refreshDevices() {
        devs = engine.devices();
        // "Driver minimum/preferred" only make sense for ASIO; on WASAPI a
        // 1-frame request opens fine but never delivers audio.
        if (isAsio()) bufIdx = 0;
        else if (bufIdx < kFirstFixedBuffer) bufIdx = kWasapiDefaultBuffer;
        inDev = outDev = -1;
        for (int i = 0; i < (int)devs.size(); ++i) {
            const auto& d = devs[i];
            if (isAsio()) {
                if (d.inputs > 0 && d.outputs >= 2) { inDev = outDev = i; break; }
            } else {
                if (d.isDefaultIn && d.inputs > 0) inDev = i;
                if (d.isDefaultOut && d.outputs >= 2) outDev = i;
            }
        }
        if (!isAsio()) {
            for (int i = 0; i < (int)devs.size() && inDev < 0; ++i) if (devs[i].inputs > 0) inDev = i;
            for (int i = 0; i < (int)devs.size() && outDev < 0; ++i) if (devs[i].outputs >= 2) outDev = i;
        }
        inCh = outPair = 0;
        pickRate();
    }

    std::vector<unsigned> commonRates() const {
        std::vector<unsigned> r;
        if (inDev < 0 || outDev < 0) return r;
        for (unsigned a : devs[inDev].sampleRates)
            if (std::find(devs[outDev].sampleRates.begin(), devs[outDev].sampleRates.end(), a) !=
                devs[outDev].sampleRates.end())
                r.push_back(a);
        // Exclusive mode can bridge mismatched rates (it resamples the mic).
        if (r.empty() && engine.exclusive()) r = devs[outDev].sampleRates;
        return r;
    }

    void pickRate() {
        auto rates = commonRates();
        if (rates.empty() || std::find(rates.begin(), rates.end(), rate) != rates.end()) return;
        // Higher rates shave a little converter latency; 48k is the sweet spot.
        rate = std::find(rates.begin(), rates.end(), 48000u) != rates.end() ? 48000u : rates.back();
    }

    void startStop() {
        error.clear();
        if (engine.running()) { engine.stop(); return; }
        if (inDev < 0 || outDev < 0) { error = "Select an input and an output device."; return; }
        StreamConfig cfg;
        cfg.inDevice = devs[inDev].id;
        cfg.outDevice = devs[outDev].id;
        cfg.inEndpoint = devs[inDev].endpointId;
        cfg.outEndpoint = devs[outDev].endpointId;
        cfg.inChannel = (unsigned)inCh;
        cfg.outFirstChannel = (unsigned)outPair * 2;
        cfg.sampleRate = rate;
        cfg.bufferFrames = kBuffers[bufIdx].frames;
        cfg.inMode = kModes[micMode].mode;
        cfg.outMode = kModes[hpMode].mode;
        if (!engine.start(cfg)) {
            error = engine.lastError();
            if (error.empty()) error = "Could not open the audio stream.";
        }
        lastCallbacks = engine.meters.callbacks.load();
        lastCallbackTime = lastCaptureTime = ImGui::GetTime();
        lastCaptured = 0;
    }

    // A driver can open "successfully" yet never run (e.g. ASIO4ALL with no
    // devices enabled). Flag it instead of sitting there silently.
    bool audioStalled() {
        const uint64_t c = engine.meters.callbacks.load(kRelaxed);
        const double now = ImGui::GetTime();
        if (c != lastCallbacks) { lastCallbacks = c; lastCallbackTime = now; }
        return now - lastCallbackTime > 1.0;
    }

    bool deviceCombo(const char* label, int& idx, bool wantInput, bool wantOutput) {
        const char* preview = idx >= 0 ? devs[idx].name.c_str() : "(none)";
        bool changed = false;
        if (ImGui::BeginCombo(label, preview)) {
            for (int i = 0; i < (int)devs.size(); ++i) {
                const auto& d = devs[i];
                if ((wantInput && d.inputs == 0) || (wantOutput && d.outputs < 2)) continue;
                char name[320];
                std::snprintf(name, sizeof name, "%s  (%u in / %u out)##%d", d.name.c_str(), d.inputs, d.outputs, i);
                if (ImGui::Selectable(name, i == idx)) { idx = i; changed = true; }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    void drawStatus() {
        if (engine.running()) {
            const float sr = (float)engine.streamSampleRate();
            const unsigned frames = engine.streamBufferFrames();
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1), "LIVE");
            ImGui::SameLine();
            if (engine.exclusive()) {
                const auto& w = engine.wasapi();
                const auto& i = w.info();
                ImGui::Text("Mic: %s %u Hz %u-bit, %.2f ms period  |  Headphones: %s %u Hz %u-bit, %.2f ms period",
                            i.captureExclusive ? "EXCLUSIVE" : "shared", i.captureRate, i.captureBits,
                            1000.f * i.capturePeriod / i.captureRate, i.renderExclusive ? "EXCLUSIVE" : "shared",
                            i.renderRate, i.renderBits, 1000.f * i.renderPeriod / i.renderRate);
                const float autoMs = w.autoJitterMs.load(kRelaxed);
                ImGui::Text("Est. round trip %.1f ms  |  jitter buffer %.2f ms (auto +%.1f ms)  |  drift trim %+.0f ppm",
                            1000.f * engine.streamLatencyFrames() / sr + autoMs, w.fifoMs.load(kRelaxed), autoMs,
                            w.ratePpm.load(kRelaxed));
                if (w.deviceLost.load(kRelaxed))
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "A device was unplugged or disabled. Press Stop.");
                // Some USB mics accept exclusive mode but never deliver data.
                const uint64_t cf = w.capturedFrames.load(kRelaxed);
                const double now = ImGui::GetTime();
                if (cf != lastCaptured) { lastCaptured = cf; lastCaptureTime = now; }
                if (now - lastCaptureTime > 1.0)
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1),
                                       "The mic isn't delivering audio. Stop, set Mic mode to Shared, and Start again.");
            } else {
                ImGui::Text("%u Hz  |  buffer %u frames (%.2f ms)  |  driver I/O latency %.2f ms",
                            engine.streamSampleRate(), frames, 1000.f * frames / sr,
                            1000.f * engine.streamLatencyFrames() / sr);
            }
            const float load = engine.meters.cpuLoad.load(kRelaxed);
            const float peak = engine.meters.cpuPeak.load(kRelaxed);
            const uint32_t xr = engine.meters.xruns.load(kRelaxed);
            ImGui::Text("DSP load %.1f%% (peak %.1f%%)  |  ", 100.f * load, 100.f * peak);
            ImGui::SameLine(0, 0);
            if (xr) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "dropouts: %u", xr);
            else ImGui::TextDisabled("dropouts: 0");
            ImGui::SameLine();
            if (ImGui::SmallButton("reset")) { engine.meters.xruns.store(0); engine.meters.cpuPeak.store(0); }
            if (audioStalled())
                ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1),
                                   "No audio is flowing from the driver. Stop and try another driver/device or a larger "
                                   "buffer (ASIO4ALL needs its devices enabled in its own panel).");
        } else {
            ImGui::TextDisabled("STOPPED  -  choose your interface and press Start");
        }
    }

    void drawDevice() {
        ImGui::SeparatorText("Audio device");
        const bool running = engine.running();
        ImGui::BeginDisabled(running);

        if (ImGui::BeginCombo("Driver", drivers[driverIdx].name.c_str())) {
            for (int i = 0; i < (int)drivers.size(); ++i) {
                if (ImGui::Selectable(drivers[i].name.c_str(), i == driverIdx) && i != driverIdx) {
                    driverIdx = i;
                    engine.setDriver(drivers[i]);
                    refreshDevices();
                }
            }
            ImGui::EndCombo();
        }
        if (!isAsio() && !engine.exclusive()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "(shared mode adds ~10-20 ms)");
        }

        if (isAsio()) {
            if (deviceCombo("Interface", outDev, true, true)) { inDev = outDev; inCh = outPair = 0; pickRate(); }
        } else {
            if (deviceCombo("Mic input", inDev, true, false)) { inCh = 0; pickRate(); }
            if (deviceCombo("Headphones", outDev, false, true)) { outPair = 0; pickRate(); }
            if (engine.exclusive()) {
                modeCombo("Mic mode", micMode);
                modeCombo("Headphone mode", hpMode);
                if (kModes[hpMode].mode != WasapiMode::Shared)
                    ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1),
                                       "Exclusive headphones mute every other app (including your music).");
            }
        }

        if (inDev >= 0) {
            char preview[32];
            std::snprintf(preview, sizeof preview, "Input %d", inCh + 1);
            if (ImGui::BeginCombo("Mic channel", preview)) {
                for (unsigned c = 0; c < devs[inDev].inputs; ++c) {
                    char n[32];
                    std::snprintf(n, sizeof n, "Input %u", c + 1);
                    if (ImGui::Selectable(n, (int)c == inCh)) inCh = (int)c;
                }
                ImGui::EndCombo();
            }
        }
        if (outDev >= 0) {
            char preview[32];
            std::snprintf(preview, sizeof preview, "Out %d/%d", outPair * 2 + 1, outPair * 2 + 2);
            if (ImGui::BeginCombo("Headphone pair", preview)) {
                for (unsigned c = 0; c + 1 < devs[outDev].outputs; c += 2) {
                    char n[32];
                    std::snprintf(n, sizeof n, "Out %u/%u", c + 1, c + 2);
                    if (ImGui::Selectable(n, (int)c / 2 == outPair)) outPair = (int)c / 2;
                }
                ImGui::EndCombo();
            }
        }

        auto rates = commonRates();
        char rateText[32];
        std::snprintf(rateText, sizeof rateText, "%u Hz", rate);
        if (ImGui::BeginCombo("Sample rate", rateText)) {
            for (unsigned r : rates) {
                char n[32];
                std::snprintf(n, sizeof n, "%u Hz", r);
                if (ImGui::Selectable(n, r == rate)) rate = r;
            }
            ImGui::EndCombo();
        }
        if (engine.exclusive()) {
            ImGui::TextDisabled("Buffer: each device's minimum period (automatic)");
        } else if (ImGui::BeginCombo("Buffer", kBuffers[bufIdx].label)) {
            for (int i = isAsio() ? 0 : kFirstFixedBuffer; i < (int)std::size(kBuffers); ++i)
                if (ImGui::Selectable(kBuffers[i].label, i == bufIdx)) bufIdx = i;
            ImGui::EndCombo();
        }
        if (isAsio()) ImGui::TextDisabled("Many ASIO drivers fix the buffer in their own control panel.");
        if (ImGui::Button("Rescan devices")) refreshDevices();
        ImGui::EndDisabled();

        if (engine.exclusive()) {
            // Live-adjustable: trades latency for robustness against crackles.
            Slider("Jitter buffer", engine.params.jitterMs, 0.f, 10.f, "%.1f ms");
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, running ? ImVec4(0.65f, 0.2f, 0.2f, 1) : ImVec4(0.2f, 0.55f, 0.3f, 1));
        if (ImGui::Button(running ? "Stop" : "Start", ImVec2(-1, 40 * ImGui::GetIO().FontGlobalScale))) startStop();
        ImGui::PopStyleColor();
        if (!error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", error.c_str());
    }

    void drawLevels() {
        ImGui::SeparatorText("Levels");
        Meter("Mic", inShown, engine.meters.inPeak.exchange(0.f, kRelaxed));
        Meter("Out", outShown, engine.meters.outPeak.exchange(0.f, kRelaxed));
        auto& p = engine.params;
        Slider("Mic gain", p.inputGainDb, -24.f, 36.f, "%+.1f dB");
        ImGui::TextDisabled("Sing at full voice: the Mic meter should peak around -12 dB. Raise Mic gain if lower.");
        Slider("Voice boost", p.voiceBoostDb, 0.f, 18.f, "+%.1f dB");
        ImGui::SameLine();
        HelpMarker("Makes your voice louder without clipping: quiet and medium singing is lifted by up to this "
                   "amount, loud notes are left alone, and a limiter catches any peak. Use this instead of Mic gain.");
        const float lim = engine.meters.limiterGainDb.load(kRelaxed);
        ImGui::TextDisabled("Boost now: +%.1f dB  |  Limiter: %s", engine.meters.boostGainDb.load(kRelaxed),
                            lim < -0.1f ? "catching peaks" : "idle");
        Slider("Headphone volume", p.outputGainDb, -60.f, 6.f, "%+.1f dB");
        Check("Mute  (M)", p.mute);
        ImGui::SameLine();
        Check("Test tone", p.testTone);
    }

    void drawLatencyTest() {
        ImGui::SeparatorText("Measure real round-trip latency");
        ImGui::TextWrapped("Patch a headphone output into the mic input with a cable (turn the volume down first), "
                           "then press Measure. A short click is sent out and timed until it returns.");
        const bool busy = engine.latencyTestBusy();
        ImGui::BeginDisabled(!engine.running() || busy);
        if (ImGui::Button(busy ? "Measuring..." : "Measure")) engine.requestLatencyTest();
        ImGui::EndDisabled();
        const long r = engine.latencyTestResult();
        ImGui::SameLine();
        if (r > 0)
            ImGui::Text("Round trip: %ld samples = %.2f ms", r, 1000.f * r / (float)engine.streamSampleRate());
        else if (r < 0)
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "No click detected - check the loopback cable / levels.");
    }

    void drawEffects() {
        auto& p = engine.params;
        ImGui::SeparatorText("Noise gate");
        ImGui::PushID("gate");
        Check("Enabled", p.gateOn);
        ImGui::SameLine();
        ImGui::TextDisabled(engine.meters.gateGain.load(kRelaxed) > 0.5f ? "(open)" : "(closed)");
        Slider("Threshold", p.gateThreshDb, -80.f, -10.f, "%.0f dB");
        ImGui::PopID();

        ImGui::SeparatorText("EQ");
        ImGui::PushID("eq");
        Check("Enabled", p.eqOn);
        Slider("Low cut", p.eqLowCutHz, 20.f, 300.f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
        Slider("Bass (200 Hz)", p.eqLowDb, -12.f, 12.f, "%+.1f dB");
        Slider("Mid", p.eqMidDb, -12.f, 12.f, "%+.1f dB");
        Slider("Mid freq", p.eqMidHz, 300.f, 6000.f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
        Slider("Air (8 kHz)", p.eqHighDb, -12.f, 12.f, "%+.1f dB");
        ImGui::PopID();

        ImGui::SeparatorText("Compressor");
        ImGui::PushID("comp");
        Check("Enabled", p.compOn);
        ImGui::SameLine();
        ImGui::TextDisabled("GR %.1f dB", engine.meters.compGainDb.load(kRelaxed));
        Slider("Threshold", p.compThreshDb, -50.f, 0.f, "%.0f dB");
        Slider("Ratio", p.compRatio, 1.f, 20.f, "%.1f : 1", ImGuiSliderFlags_Logarithmic);
        Slider("Makeup", p.compMakeupDb, 0.f, 24.f, "%+.1f dB");
        ImGui::PopID();

        ImGui::SeparatorText("Echo");
        ImGui::PushID("echo");
        Check("Enabled", p.echoOn);
        Slider("Time", p.echoMs, 20.f, 1500.f, "%.0f ms", ImGuiSliderFlags_Logarithmic);
        Slider("Feedback", p.echoFeedback, 0.f, 0.95f, "%.2f");
        Slider("Mix", p.echoMix, 0.f, 1.f, "%.2f");
        ImGui::PopID();

        ImGui::SeparatorText("Reverb");
        ImGui::PushID("rev");
        Check("Enabled", p.reverbOn);
        Slider("Room size", p.reverbSize, 0.f, 1.f, "%.2f");
        Slider("Damping", p.reverbDamp, 0.f, 1.f, "%.2f");
        Slider("Mix", p.reverbMix, 0.f, 1.f, "%.2f");
        ImGui::PopID();
    }

    void draw() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_M, false))
            engine.params.mute.store(!engine.params.mute.load());

        drawStatus();
        ImGui::Separator();

        if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextColumn();
            ImGui::PushItemWidth(-140 * ImGui::GetIO().FontGlobalScale);
            drawDevice();
            drawLevels();
            drawLatencyTest();
            ImGui::PopItemWidth();
            ImGui::TableNextColumn();
            ImGui::PushItemWidth(-140 * ImGui::GetIO().FontGlobalScale);
            drawEffects();
            ImGui::PopItemWidth();
            ImGui::EndTable();
        }
        ImGui::End();
    }
};

} // namespace ui

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // device enumeration (ASIO wants STA too)
    TuneProcessForAudio();
    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"LowLatencyKaraoke", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Low Latency Karaoke", WS_OVERLAPPEDWINDOW, 100, 100,
                              (int)(1100 * dpi), (int)(780 * dpi), nullptr, nullptr, hInst, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInst);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 4.f;
    style.GrabRounding = 4.f;
    style.ScaleAllSizes(dpi);
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.f * dpi);
    else
        io.FontGlobalScale = dpi;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    {
        ui::App app;
        app.refreshDevices();

        bool done = false;
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;

            if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                Sleep(16);
                continue;
            }
            g_occluded = false;

            if (g_resizeW != 0 && g_resizeH != 0) {
                CleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
                g_resizeW = g_resizeH = 0;
                CreateRenderTarget();
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.draw();
            ImGui::Render();

            const float clear[4] = {0.08f, 0.08f, 0.10f, 1.f};
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_context->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_occluded = g_swapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED; // vsync keeps the UI cheap
        }
        app.engine.stop();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}

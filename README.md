# Low Latency Karaoke

Mic → effects → headphones on Windows, with as little delay as the hardware allows.

Three ways to drive the audio:

| Driver | Use when | Typical round trip |
|---|---|---|
| **WASAPI Low Latency** (default, native) | Separate USB mic + headphones (e.g. HyperX SoloCast + USB-C adapter), onboard audio | ~10–55 ms depending on per-device mode (see below) |
| **ASIO** (via RtAudio) | A real audio interface with its own ASIO driver | ~3–6 ms |
| WASAPI Shared (via RtAudio) | Fallback when exclusive mode isn't possible | ~25–40 ms |

## Build

Requires Visual Studio 2019+ (C++ workload or Build Tools). CMake comes bundled with it.

```bat
build.bat
```

The output is `build\Release\LowLatencyKaraoke.exe`, a single static exe with no DLLs. RtAudio and Dear ImGui are downloaded automatically on the first configure.

## Use (USB mic + headphones)

Each device has a mode:

| Mode | What it does |
|---|---|
| **Auto** (mic default) | Exclusive if the device is free (lowest latency); shared if another app such as Discord is using it |
| **Shared** (headphones default) | Mixed by Windows alongside other apps, so **your music/backing track keeps playing** |
| **Exclusive only** | Fastest, but locks out every other app on that device. Exclusive headphones mute your music. |

Typical round trip: mic exclusive + headphones shared ≈ 30 ms. Both shared ≈ 50 ms. Windows can't let one app take
a device another app is holding, so with Discord on the mic, the mic runs shared. For the lowest latency, switch Discord's
input to another device (Settings → Voice & Video) or quit it while singing.

1. Optional, for exclusive mic: Win+R → `mmsys.cpl` → *Recording* → your mic → Properties → Advanced → tick
   **Allow applications to take exclusive control**. Devices with this turned off show **[exclusive mode disabled in Windows]**.
2. **Driver:** WASAPI Low Latency. Pick the **Mic input** and **Headphones**, then press **Start**. The status line shows
   which mode each device actually got.
3. If a red message says the mic isn't delivering audio, set **Mic mode** to Shared. Some USB mics accept exclusive
   mode but never send data.
4. Set **Mic gain** so the Mic meter peaks around -12 dB, then set the headphone volume.
5. **Jitter buffer:** leave it at 1 ms. If you hear crackles, raise it. The engine also adds margin by itself after
   an underrun (shown as "auto +X ms") and slowly gives it back once things are stable.

### Use (ASIO interface)
Choose **ASIO**, pick the interface, and set the buffer to *Driver minimum* (raise it to 64 or 128 if you hear crackles).
Turn off the interface's own "Direct Monitor"; otherwise you hear the dry and processed signals together, which sounds phasey.

`M` toggles mute.

### Measuring true latency
Connect a cable from a headphone/line output to an input, select that input, and press **Measure**. The app sends a click and times how long it takes to come back,
including converters, USB, and every buffer.

## How the exclusive engine works

- Each device is opened in **exclusive, event-driven mode at its minimum period** (usually 3 ms). This skips
  the Windows audio engine and its ~10 ms mixing period entirely.
- One **MMCSS "Pro Audio"** thread at critical priority services both devices.
- The mic and headphones run on **independent clocks**, so captured audio goes through a small FIFO. The FIFO is read
  at a fractional rate trimmed by a PI controller on the FIFO fill level. This absorbs clock drift (typically tens of ppm)
  without clicks, and also converts between sample rates when the devices differ (e.g. 44.1 → 48 kHz).
- The FIFO target is about half a capture period plus a render period plus your jitter setting. If a device delivers in bursts, the
  adaptive margin grows until underruns stop.
- Effects run at the headphone rate, right before writing the output buffer.

## Design notes (why it's fast)

- **Real-time safe audio thread.** It never allocates, locks, logs, or makes blocking syscalls. UI → audio parameters
  are relaxed atomics read once per block. Audio → UI meters are atomics too.
- **FTZ/DAZ** are set for each block, so decaying reverb tails never hit slow denormals.
- **Cheap DSP.** Biquad EQ, a peak gate and compressor, an interpolated echo, and Freeverb. Measured at about 1–2% of a 3 ms period.
- **Process tuning:** `HIGH_PRIORITY_CLASS` and opting out of Windows 11 EcoQoS throttling.
- The UI renders with vsync and sleeps while minimized or occluded.

## If you get dropouts

- Windows power plan: **High/Ultimate performance**. Disable *USB selective suspend*.
- Plug the USB devices straight into the PC, not through a hub.
- Run [LatencyMon](https://www.resplendence.com/latencymon) to find drivers with high DPC latency (Wi-Fi, Bluetooth, GPU).
- Avoid Bluetooth headphones. They add 100–250 ms that no software can remove.

## Web version (`web/index.html`)

The same effects chain runs in the browser as an AudioWorklet. Nothing to install: serve the folder and open it in Chrome or Edge.

```bash
python -m http.server 8642 --directory web
```

Then open http://localhost:8642 (microphone access needs `localhost` or https). Browsers only use Windows' shared
mode, so expect roughly 30–60 ms. Use **Measure** to compare against the native app on your machine.

## Future features

### Live pitch graph
A scrolling graph of the note you're singing, in both the native app and the web version (web first).

- **Detection:** YIN or MPM (McLeod Pitch Method) with parabolic interpolation. Use 2048-sample windows
  with a 256-sample hop at 48 kHz, which gives about 190 pitch readings per second.
- **Target accuracy:** about ±5 cents on held notes. The graph should trail your voice by about 20–30 ms, and none of this
  touches the audio path.
- **Read the dry mic signal,** before reverb and echo, so effect tails don't smear notes together.
- **Cleanup:** a confidence threshold so breaths and consonants show as gaps, and a median filter to remove octave jumps.
- **Display:** note lanes (C, C♯, D…), your pitch as a line, and the current note with a ±cents readout.
- **Native:** analysis runs on its own low-priority thread, fed by a lock-free ring buffer from the audio thread.
- **Later:** karaoke scoring against song melodies (e.g. UltraStar-format note files).

### UI cleanup
- Group controls into clear sections: **Setup** (devices, modes), **Sing** (levels, voice boost, pitch graph) and
  **Effects**, so the main screen shows only what's needed while singing.
- Effect presets (e.g. *Dry*, *Studio*, *Hall*, *Stadium*) with an "Advanced" toggle for individual sliders.
- Collapse technical readouts (periods, ppm, jitter buffer) into a single latency badge with details on hover.
- Remember device and effect settings between launches in the native app (the web version already does).
- Use the same visual style for the native and web versions.

## Layout

```
src/Dsp.h               effect building blocks (allocation-free, header-only)
src/WasapiExclusive.*   native exclusive-mode duplex: two devices, drift-compensating FIFO
src/AudioEngine.*       driver selection, effect chain, latency test (RtAudio for ASIO/shared)
src/main.cpp            Win32 + DX11 + Dear ImGui front end
```

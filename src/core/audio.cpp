#include "core/audio.h"

#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include "core/config.h"
#include "core/controllers.h"
#include "core/hands.h"
#include "core/log.h"
#include "core/xr.h"

namespace rdrvr::audio {
namespace {

constexpr int kRate = 48000;
constexpr int kSynthSamples = kRate * 70 / 1000;  // the synthesised click: 70 ms, mono
constexpr int kVoices = 4;                        // clicks that can sound at once

// the clicks' samples, all at the voices' format (48 kHz mono): 0 the default (EmptyClickWav or the synthesised one),
// 1 revolvers and pistols, 2 rifles, 3 shotguns (the user's sounds, round 9). Filled once by the audio thread before
// g_pcm_ready, then only read.
constexpr int kSounds = 4;
const char* const kFamilyName[kSounds] = {"default", "revolver", "rifle", "shotgun"};
const char* const kFamilyKey[kSounds] = {"EmptyClickWav", "EmptyClickRevolver", "EmptyClickRifle", "EmptyClickShotgun"};
const char* const kFamilyFile[kSounds] = {"", "RDRVR_click_revolver.wav", "RDRVR_click_rifle.wav", "RDRVR_click_shotgun.wav"};
struct Sound {
    std::vector<int16_t> pcm;  // 48 kHz mono
    bool from_wav = false;
    std::string path;          // as configured
    uint32_t src_rate = 0, src_ch = 0;  // the WAV's (0: synthesised); from 2 channels it plays 3 dB up (play_one)
    float trim_ms = 0.0f;
};
Sound g_snd[kSounds];
WAVEFORMATEX g_wf{WAVE_FORMAT_PCM, 1, 48000, 48000 * 2, 2, 16, 0};
bool g_from_wav = false;  // the default sound is EmptyClickWav's
bool g_override = false;  // EmptyClickWav set: it plays for every family
std::vector<int16_t>& g_pcm = g_snd[0].pcm;
std::atomic<bool> g_pcm_ready{false};
std::atomic<int> g_req_sound{0};  // the sound of the latest request (as the pan)
std::atomic<double> g_req_ms{0.0};  // when the latest request came (log::now_ms), for the click's delay in the log
std::atomic<uint64_t> g_family_played[kSounds] = {};

std::atomic<int> g_mode{0};  // ClickMode
std::atomic<float> g_volume{0.6f}, g_pan_max{0.5f};
std::atomic<uint32_t> g_req{0}, g_pan_bits{0};
std::atomic<uint64_t> g_requested{0}, g_played{0}, g_ended{0}, g_dropped{0};
std::atomic<bool> g_ready{false}, g_lost{false};
std::atomic<uint32_t> g_channels{0}, g_out_rate{0}, g_latency{0}, g_glitches{0};
std::atomic<const char*> g_fail_what{nullptr};
std::atomic<long> g_fail_hr{0};
HANDLE g_wake = nullptr;  // auto-reset: a click asked for, the mode changed, or the device lost
std::string g_wav_path;   // [Reload] EmptyClickWav, read by the audio thread at its start
std::string g_family_path[kSounds];  // [Reload] EmptyClickRevolver / Rifle / Shotgun (index 1-3), read at init

// the engine: the audio thread only (opened, used and released there; never from a callback or DllMain)
IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_voice[kVoices] = {};
uint32_t g_next = 0;

struct EngineCb final : IXAudio2EngineCallback {
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT) override {  // the device lost or changed: reopened on the thread
        g_lost = true;
        SetEvent(g_wake);
    }
};
struct VoiceCb final : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { g_ended.fetch_add(1, std::memory_order_relaxed); }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};
EngineCb g_engine_cb;
VoiceCb g_voice_cb;

const char* mode_name(int m) { return m == 1 ? "buzz" : m == 2 ? "both" : m == 3 ? "off" : "sound"; }
int parse_mode(const std::string& s) { return s == "buzz" ? 1 : s == "both" ? 2 : s == "off" ? 3 : 0; }
float clamp01(float v) { return !(v >= 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v; }

// A hammer falling on an empty chamber: a 4 ms high-passed noise burst and four inharmonic metal modes (2.3-7.3 kHz,
// 4-12 ms decays) with a low thump, then a smaller second strike 9 ms later (the hammer's bounce); -6 dBFS peak, 0.3 ms
// fade in, 10 ms fade out. Deterministic (a fixed noise seed).
void synth_click(std::vector<int16_t>& out) {
    std::vector<float> x(kSynthSamples, 0.0f);
    uint32_t seed = 0x2545F491u;
    auto noise = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<int32_t>(seed)) * (1.0f / 2147483648.0f);
    };
    struct Mode {
        float f, a, tau;
    };
    const Mode ring[] = {{2350, 0.35f, 0.012f}, {3720, 0.30f, 0.009f}, {5180, 0.22f, 0.006f}, {7300, 0.12f, 0.004f}, {190, 0.25f, 0.005f}};
    auto strike = [&](int at, float gain) {
        float hp = 0, prev = 0;
        for (int i = 0; at + i < kSynthSamples; ++i) {
            const float t = static_cast<float>(i) / kRate;
            float s = 0;
            if (t < 0.004f) {  // decaying in 0.6 ms, one-pole high-passed at about 900 Hz
                const float w = noise() * std::exp(-t / 0.0006f);
                hp = 0.89f * (hp + w - prev);
                prev = w;
                s += 0.9f * hp;
            }
            for (const Mode& m : ring) s += m.a * std::sin(6.2831853f * m.f * t) * std::exp(-t / m.tau);
            x[static_cast<size_t>(at + i)] += gain * s;
        }
    };
    strike(0, 1.0f);
    strike(kRate * 9 / 1000, 0.35f);
    float peak = 1e-6f;
    for (float v : x) peak = std::max(peak, std::fabs(v));
    const int fin = kRate * 3 / 10000, fout = kRate / 100;
    out.resize(kSynthSamples);
    for (int i = 0; i < kSynthSamples; ++i) {
        float g = 0.5f / peak;
        if (i < fin) g *= static_cast<float>(i) / fin;
        if (i > kSynthSamples - fout) g *= static_cast<float>(kSynthSamples - i) / fout;
        out[static_cast<size_t>(i)] = static_cast<int16_t>(std::lrintf(std::clamp(x[static_cast<size_t>(i)] * g, -1.0f, 1.0f) * 32767.0f));
    }
}

// [Reload] EmptyClickWav and the families' WAVs: a RIFF WAVE of 16-bit PCM, 1 or 2 channels, 8-96 kHz; at most 2 s are
// kept.
enum class WavRead { Ok, CannotOpen, NotPcm };
WavRead read_wav(const std::wstring& path, std::vector<int16_t>& pcm, WAVEFORMATEX& wf) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return WavRead::CannotOpen;
    std::vector<uint8_t> d;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long n = std::ftell(f);
        if (n > 12 && n < 16 * 1024 * 1024 && std::fseek(f, 0, SEEK_SET) == 0) {
            d.resize(static_cast<size_t>(n));
            if (std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
        }
    }
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(&d[0], "RIFF", 4) != 0 || std::memcmp(&d[8], "WAVE", 4) != 0) return WavRead::NotPcm;
    uint16_t tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    bool fmt = false;
    for (size_t p = 12; p + 8 <= d.size();) {
        uint32_t len = 0;
        std::memcpy(&len, &d[p + 4], 4);
        const size_t body = p + 8;
        if (len > d.size() - body) len = static_cast<uint32_t>(d.size() - body);
        if (std::memcmp(&d[p], "fmt ", 4) == 0 && len >= 16) {
            std::memcpy(&tag, &d[body], 2);
            std::memcpy(&ch, &d[body + 2], 2);
            std::memcpy(&rate, &d[body + 4], 4);
            std::memcpy(&bits, &d[body + 14], 2);
            if (tag == 0xFFFE && len >= 26) std::memcpy(&tag, &d[body + 24], 2);  // WAVE_FORMAT_EXTENSIBLE: the subformat
            fmt = true;
        } else if (std::memcmp(&d[p], "data", 4) == 0 && fmt) {
            if (tag != WAVE_FORMAT_PCM || bits != 16 || ch < 1 || ch > 2 || rate < 8000 || rate > 96000) return WavRead::NotPcm;
            size_t n = len / 2;
            n = std::min<size_t>(n, static_cast<size_t>(rate) * ch * 2);
            n -= n % ch;
            if (!n) return WavRead::NotPcm;
            pcm.assign(n, 0);
            std::memcpy(pcm.data(), &d[body], n * 2);
            wf = {WAVE_FORMAT_PCM, ch, rate, rate * ch * 2, static_cast<WORD>(ch * 2), 16, 0};
            return WavRead::Ok;
        }
        p = body + len + (len & 1);
    }
    return WavRead::NotPcm;
}

// A source above 48 kHz before it is decimated: a 63-tap Blackman-windowed sinc at 20 kHz (flat to 16 kHz, 64 dB or more
// down from 24 kHz and 80 from 28 kHz at 88.2 and 96 kHz), so nothing above the voices' 24 kHz folds back into the
// click. Zero phase.
void low_pass_20k(std::vector<float>& x, uint32_t rate) {
    constexpr int kHalf = 31;
    constexpr double kPi = 3.14159265358979;
    const double fc = 20000.0 / static_cast<double>(rate);  // cycles a sample
    float h[2 * kHalf + 1];
    double sum = 0;
    for (int k = -kHalf; k <= kHalf; ++k) {
        const double s = k ? std::sin(2.0 * kPi * fc * k) / (kPi * k) : 2.0 * fc;
        const double w = 0.42 + 0.5 * std::cos(kPi * k / kHalf) + 0.08 * std::cos(2.0 * kPi * k / kHalf);
        h[k + kHalf] = static_cast<float>(s * w);
        sum += s * w;
    }
    for (float& c : h) c = static_cast<float>(c / sum);  // unity at DC
    std::vector<float> pad(x.size() + 2 * kHalf, 0.0f);  // zeros beyond both ends
    std::copy(x.begin(), x.end(), pad.begin() + kHalf);
    for (size_t i = 0; i < x.size(); ++i) {
        float v = 0;
        for (int k = 0; k <= 2 * kHalf; ++k) v += h[k] * pad[i + static_cast<size_t>(k)];
        x[i] = v;
    }
}

// Any 16-bit PCM (1-2 channels, any rate) to the voices' 48 kHz mono: the channels averaged, low-passed when above
// 48 kHz, linear interpolation, the leading samples below -50 dBFS dropped (the click at once), at most 2 s kept.
void to_voice_format(const std::vector<int16_t>& in, const WAVEFORMATEX& f, std::vector<int16_t>& out, float* trim_ms) {
    const int ch = f.nChannels ? f.nChannels : 1;
    const size_t frames = in.size() / static_cast<size_t>(ch);
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        float v = 0;
        for (int c = 0; c < ch; ++c) v += in[i * ch + c];
        mono[i] = v / static_cast<float>(ch);
    }
    if (f.nSamplesPerSec > static_cast<DWORD>(kRate)) low_pass_20k(mono, f.nSamplesPerSec);
    size_t first = 0;
    while (first < frames && std::fabs(mono[first]) < 104.0f) ++first;  // -50 dBFS of 32767
    if (first == frames) first = 0;
    if (trim_ms) *trim_ms = 1000.0f * static_cast<float>(first) / static_cast<float>(f.nSamplesPerSec ? f.nSamplesPerSec : 1);
    const double step = static_cast<double>(f.nSamplesPerSec) / static_cast<double>(kRate);
    out.clear();
    for (double t = static_cast<double>(first); t + 1.0 < static_cast<double>(frames) && out.size() < static_cast<size_t>(kRate) * 2; t += step) {
        const size_t i = static_cast<size_t>(t);
        const float a = static_cast<float>(t - static_cast<double>(i));
        const float v = mono[i] * (1.0f - a) + mono[i + 1] * a;
        out.push_back(static_cast<int16_t>(std::lrintf(std::clamp(v, -32768.0f, 32767.0f))));
    }
}

// A path from the ini, made wide (its bytes as UTF-8, else in the ANSI code page): absolute, or a name next to RDR.exe
// (the deploy's folder). Never narrowed: the game's folder may hold any character.
std::wstring resolve(const std::string& p) {
    auto widen = [&p](UINT cp, DWORD flags) {
        const int n = MultiByteToWideChar(cp, flags, p.data(), static_cast<int>(p.size()), nullptr, 0);
        std::wstring w(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
        if (n > 0) MultiByteToWideChar(cp, flags, p.data(), static_cast<int>(p.size()), w.data(), n);
        return w;
    };
    std::wstring w = widen(CP_UTF8, MB_ERR_INVALID_CHARS);
    if (w.empty()) w = widen(CP_ACP, 0);
    if (w.empty() || (w.size() > 1 && w[1] == L':') || w[0] == L'\\' || w[0] == L'/') return w;
    wchar_t buf[MAX_PATH];
    return log::path_in_game_dir(w.c_str(), buf, MAX_PATH);
}

void engine_close() {
    g_ready = false;
    for (auto& v : g_voice)
        if (v) {
            v->DestroyVoice();
            v = nullptr;
        }
    if (g_master) {
        g_master->DestroyVoice();
        g_master = nullptr;
    }
    if (g_xa) {
        g_xa->Release();
        g_xa = nullptr;
    }
}

bool fail(const char* what, HRESULT hr) {
    engine_close();
    if (g_fail_what.load() != what || g_fail_hr.load() != hr)  // once per failure, not at every retry
        log::warn("[audio] %s failed (0x%08lx): no click sound until it opens", what, static_cast<unsigned long>(hr));
    g_fail_what = what;
    g_fail_hr = hr;
    return false;
}

// The game's own way (FUN_140f69f10): xaudio2_9.dll from System32, the default device, the GameEffects category.
bool engine_open() {
    static HMODULE m = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using CreateV = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR, DWORD);
    using Create = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    HRESULT hr = E_FAIL;
    if (auto cv = m ? reinterpret_cast<CreateV>(GetProcAddress(m, "XAudio2CreateWithVersionInfo")) : nullptr)
        hr = cv(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR, 0x0A000008);
    else if (auto c = m ? reinterpret_cast<Create>(GetProcAddress(m, "XAudio2Create")) : nullptr)
        hr = c(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa) return fail(m ? "XAudio2Create" : "loading xaudio2_9.dll", FAILED(hr) ? hr : E_FAIL);
    g_xa->RegisterForCallbacks(&g_engine_cb);
    hr = g_xa->CreateMasteringVoice(&g_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr,
                                    AudioCategory_GameEffects);
    if (FAILED(hr)) return fail("CreateMasteringVoice", hr);
    XAUDIO2_VOICE_DETAILS vd{};
    g_master->GetVoiceDetails(&vd);
    g_channels = vd.InputChannels;
    g_out_rate = vd.InputSampleRate;
    for (auto& v : g_voice)
        if (FAILED(hr = g_xa->CreateSourceVoice(&v, &g_wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &g_voice_cb))) return fail("CreateSourceVoice", hr);
    g_fail_what = nullptr;
    g_fail_hr = 0;
    g_ready = true;
    log::info("[audio] ready: the default device, %u channels at %u Hz, %d click voices", vd.InputChannels, vd.InputSampleRate, kVoices);
    return true;
}

void play_one() {
    if (!g_ready.load()) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float pan = std::bit_cast<float>(g_pan_bits.load(std::memory_order_relaxed));
    const float a = (pan + 1.0f) * 0.78539816f;  // constant power: cos/sin over 0..pi/2
    int si = g_req_sound.load(std::memory_order_relaxed);
    si = si < 0 || si >= kSounds || g_snd[si].pcm.empty() || g_override ? 0 : si;
    // a sound from 2 channels (averaged) gets the stereo voice's sqrt 2 back in the matrix, not in its samples (they
    // would clip): a file with L = R plays at unity on each side when centred, as before the voices were mono
    const float g = g_snd[si].from_wav && g_snd[si].src_ch == 2 ? 1.41421356f : 1.0f;
    const UINT32 dc = g_channels.load();
    for (int k = 0; k < kVoices; ++k) {
        IXAudio2SourceVoice* v = g_voice[g_next++ % kVoices];
        XAUDIO2_VOICE_STATE s{};
        v->GetState(&s, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (s.BuffersQueued) continue;  // still sounding
        float lv[8] = {};               // the mono voice (g_wf) to each output channel
        if (dc == 1) {
            lv[0] = 1.0f;  // the average: what the stereo voice's 0.5 + 0.5 gave
        } else {
            lv[0] = std::cos(a) * g;
            lv[1] = std::sin(a) * g;
        }
        if (dc >= 1 && dc <= 8) v->SetOutputMatrix(nullptr, 1, dc, lv);
        v->SetVolume(g_volume.load());
        XAUDIO2_BUFFER b{};
        b.Flags = XAUDIO2_END_OF_STREAM;
        const std::vector<int16_t>& pcm = g_snd[si].pcm;
        b.AudioBytes = static_cast<UINT32>(pcm.size() * sizeof(int16_t));
        b.pAudioData = reinterpret_cast<const BYTE*>(pcm.data());
        if (SUCCEEDED(v->SubmitSourceBuffer(&b)) && SUCCEEDED(v->Start(0))) {
            g_played.fetch_add(1, std::memory_order_relaxed);
            g_family_played[si].fetch_add(1, std::memory_order_relaxed);
            log::info("[audio] click: the %s's (%s, pan %+.2f), %.1f ms after the trigger", kFamilyName[si],
                      g_snd[si].from_wav ? g_snd[si].path.c_str() : "synthesised", pan, log::now_ms() - g_req_ms.load(std::memory_order_relaxed));
        } else {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    g_dropped.fetch_add(1, std::memory_order_relaxed);
}

DWORD WINAPI audio_thread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // this thread only
    for (int si = 0; si < kSounds; ++si) {  // the default (0), then the families' (1-3)
        Sound& snd = g_snd[si];
        snd.path = si == 0 ? g_wav_path : g_family_path[si];
        std::vector<int16_t> pcm;
        WAVEFORMATEX wf{};
        bool wav = false;
        if (!snd.path.empty()) {  // the configured name in the log lines (the wide path may not print)
            const WavRead r = read_wav(resolve(snd.path), pcm, wf);
            wav = r == WavRead::Ok;
            const char* instead = si == 0 ? "synthesised click" : "default click";
            if (r == WavRead::CannotOpen)
                log::warn("[audio] %s: cannot open %s; the %s", kFamilyKey[si], snd.path.c_str(), instead);
            else if (r == WavRead::NotPcm)
                log::warn("[audio] %s %s: not a 16-bit PCM WAV of 1-2 channels at 8-96 kHz; the %s", kFamilyKey[si], snd.path.c_str(), instead);
        }
        if (wav) {
            to_voice_format(pcm, wf, snd.pcm, &snd.trim_ms);
            snd.src_rate = wf.nSamplesPerSec;
            snd.src_ch = wf.nChannels;
            snd.from_wav = !snd.pcm.empty();
            log::info("[audio] the %s's click from %s (%u Hz, %u channels; %zu samples at 48 kHz mono, %.1f ms of silence trimmed)",
                      kFamilyName[si], snd.path.c_str(), snd.src_rate, snd.src_ch, snd.pcm.size(), snd.trim_ms);
        }
        if (si == 0 && !snd.from_wav) synth_click(snd.pcm);  // 48 kHz mono already
    }
    g_from_wav = g_snd[0].from_wav;
    g_override = g_from_wav;  // EmptyClickWav, when set, plays for every family
    g_pcm_ready.store(true, std::memory_order_release);
    uint32_t seen = g_req.load(std::memory_order_acquire);
    for (;;) {
        const int m = g_mode.load();
        const bool want = m == 0 || m == 2;
        if (g_lost.exchange(false)) {
            engine_close();
            if (want) log::warn("[audio] the device was lost or changed: reopening");
        }
        if (!want && g_xa) {
            engine_close();
            log::info("[audio] closed (the empty gun does not click a sound)");
        }
        if (want && !g_ready.load()) engine_open();  // tried again at each wake while it fails
        const uint32_t cur = g_req.load(std::memory_order_acquire);
        for (uint32_t k = 0; k < cur - seen; ++k) {
            if (k < kVoices)
                play_one();
            else
                g_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        seen = cur;
        if (g_xa) {
            XAUDIO2_PERFORMANCE_DATA pd{};
            g_xa->GetPerformanceData(&pd);
            g_latency = pd.CurrentLatencyInSamples;
            g_glitches = pd.GlitchesSinceEngineStarted;
        }
        WaitForSingleObject(g_wake, 5000);
    }
}

void request(float pan, int sound = 0) {
    g_req_sound.store(sound, std::memory_order_relaxed);
    g_req_ms.store(log::now_ms(), std::memory_order_relaxed);
    g_pan_bits.store(std::bit_cast<uint32_t>(pan), std::memory_order_relaxed);
    g_requested.fetch_add(1, std::memory_order_relaxed);
    g_req.fetch_add(1, std::memory_order_release);
    if (g_wake) SetEvent(g_wake);
}

// The gun hand's side of the head (both in the OpenXR LOCAL space), up to EmptyClickPan.
float click_pan(int h) {
    const float pm = g_pan_max.load(std::memory_order_relaxed);
    xr::EyeView ev[2];
    const hands::Hand hd = hands::get(h);
    if (hd.valid && xr::eye_views_peek(ev)) {
        const float* q = ev[0].orientation;  // x y z w; the head's right: the rotation's first column
        const float r[3] = {1 - 2 * (q[1] * q[1] + q[2] * q[2]), 2 * (q[0] * q[1] + q[3] * q[2]), 2 * (q[0] * q[2] - q[3] * q[1])};
        float x = 0;
        for (int k = 0; k < 3; ++k) x += (hd.pos[k] - 0.5f * (ev[0].position[k] + ev[1].position[k])) * r[k];
        return std::clamp(x / 0.35f, -1.0f, 1.0f) * pm;
    }
    return (h == 1 ? 1.0f : -1.0f) * pm;
}

}  // namespace

void init() {
    g_mode = parse_mode(config::get_string("Reload", "EmptyClick", "sound"));
    g_volume = clamp01(config::get_float("Reload", "EmptyClickVolume", 0.6f));
    g_pan_max = clamp01(config::get_float("Reload", "EmptyClickPan", 0.5f));
    g_wav_path = config::get_string("Reload", "EmptyClickWav", "");
    for (int si = 1; si < kSounds; ++si) g_family_path[si] = config::get_string("Reload", kFamilyKey[si], kFamilyFile[si]);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool started = false;
    if (g_wake)
        if (HANDLE t = CreateThread(nullptr, 0, audio_thread, nullptr, 0, nullptr)) {
            SetThreadDescription(t, L"RDRVR audio");
            CloseHandle(t);
            started = true;
        }
    log::info("[audio] empty gun: %s (volume %.2f, pan %.2f)%s", mode_name(g_mode.load()), g_volume.load(), g_pan_max.load(),
              started ? "" : "; NO AUDIO THREAD: the buzz only");
}

int click_family(int w) {
    if (w >= 0 && w <= 7) return 1;                                     // revolvers and pistols
    if ((w >= 8 && w <= 14) || w == 19 || w == 20 || w == 31) return 2;  // repeaters and rifles, the explosive rifle
    if ((w >= 15 && w <= 18) || w == 34) return 3;                      // shotguns, the blunderbuss
    return 0;
}

void empty_click(int h, int weapon) {
    const int m = g_mode.load(std::memory_order_relaxed);
    if (m == 1 || m == 2) controllers::pulse(h, 0.6f, 12);  // the buzz
    if (m == 0 || m == 2) request(click_pan(h), click_family(weapon));
}

void preview(int weapon) {
    const int f = click_family(weapon);
    request(click_pan(1), f ? f : 1);  // the right hand's side, as "sound click"
}

ClickMode empty_click_mode() { return static_cast<ClickMode>(g_mode.load()); }
void set_empty_click_mode(ClickMode m) {
    const int v = static_cast<int>(m);
    if (g_mode.exchange(v) != v) log::info("[audio] empty gun: %s", mode_name(v));
    config::set("Reload", "EmptyClick", mode_name(v));
    if (g_wake) SetEvent(g_wake);
}
float click_volume() { return g_volume.load(); }
void set_click_volume(float v, bool save) {
    g_volume = clamp01(v);
    if (save) {
        char s[16];
        std::snprintf(s, sizeof(s), "%.2f", g_volume.load());
        config::set("Reload", "EmptyClickVolume", s);
    }
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, v;
    in >> c >> v;
    if (v == "click") {
        std::string side, fam;
        in >> side >> fam;
        const int si = fam == "revolver" ? 1 : fam == "rifle" ? 2 : fam == "shotgun" ? 3 : 0;
        request(click_pan(side == "left" ? 0 : 1), si);
    } else if (v == "mode") {  // the session only (the menu writes the user ini)
        std::string m;
        in >> m;
        g_mode = parse_mode(m);
        if (g_wake) SetEvent(g_wake);
    } else if (v == "volume") {
        float x = 0.6f;
        in >> x;
        g_volume = clamp01(x);
    } else if (v == "pan") {
        float x = 0.5f;
        in >> x;
        g_pan_max = clamp01(x);
    } else if (v == "reset") {
        g_lost = true;
        if (g_wake) SetEvent(g_wake);
    } else if (v == "dump") {
        std::string path;
        in >> path;
        if (!g_pcm_ready.load(std::memory_order_acquire)) return "ERROR the click is not made yet";
        FILE* f = nullptr;
        if (path.empty() || fopen_s(&f, path.c_str(), "wb") != 0 || !f) return "ERROR cannot write " + path;
        const uint32_t data = static_cast<uint32_t>(g_pcm.size() * 2), riff = 36 + data, fmt_len = 16;
        const uint16_t pcm_tag = WAVE_FORMAT_PCM;
        std::fwrite("RIFF", 1, 4, f);
        std::fwrite(&riff, 4, 1, f);
        std::fwrite("WAVEfmt ", 1, 8, f);
        std::fwrite(&fmt_len, 4, 1, f);
        std::fwrite(&pcm_tag, 2, 1, f);
        std::fwrite(&g_wf.nChannels, 2, 1, f);
        std::fwrite(&g_wf.nSamplesPerSec, 4, 1, f);
        std::fwrite(&g_wf.nAvgBytesPerSec, 4, 1, f);
        std::fwrite(&g_wf.nBlockAlign, 2, 1, f);
        std::fwrite(&g_wf.wBitsPerSample, 2, 1, f);
        std::fwrite("data", 1, 4, f);
        std::fwrite(&data, 4, 1, f);
        std::fwrite(g_pcm.data(), 2, g_pcm.size(), f);
        std::fclose(f);
        return "wrote " + path;
    }
    const char* fw = g_fail_what.load();
    std::string fams;
    if (g_pcm_ready.load(std::memory_order_acquire))
        for (int si = 1; si < kSounds; ++si) {
            char fb[120];
            std::snprintf(fb, sizeof(fb), " | %s: %s (played %llu)", kFamilyName[si],
                          g_override ? "EmptyClickWav's" : g_snd[si].from_wav ? g_snd[si].path.c_str() : "the default",
                          static_cast<unsigned long long>(g_family_played[si].load()));
            fams += fb;
        }
    char b[420];
    std::snprintf(b, sizeof(b),
                  "sound: mode %s, volume %.2f, pan %.2f, ready %d (%u channels, %u Hz), voices %d, requested %llu, played %llu, ended %llu, "
                  "dropped %llu, latency %u samples, glitches %u, click %s (%zu samples, %u Hz, %u ch)%s%s",
                  mode_name(g_mode.load()), g_volume.load(), g_pan_max.load(), g_ready.load() ? 1 : 0, g_channels.load(), g_out_rate.load(),
                  kVoices, static_cast<unsigned long long>(g_requested.load()), static_cast<unsigned long long>(g_played.load()),
                  static_cast<unsigned long long>(g_ended.load()), static_cast<unsigned long long>(g_dropped.load()), g_latency.load(),
                  g_glitches.load(), g_pcm_ready.load() ? (g_from_wav ? "from the WAV" : "synthesised") : "not made",
                  g_pcm_ready.load() ? g_pcm.size() / (g_wf.nChannels ? g_wf.nChannels : 1) : 0, g_pcm_ready.load() ? static_cast<unsigned>(g_wf.nSamplesPerSec) : 0u,
                  g_pcm_ready.load() ? static_cast<unsigned>(g_wf.nChannels) : 0u, fw ? ", failed: " : "", fw ? fw : "");
    return std::string(b) + fams;
}

}  // namespace rdrvr::audio

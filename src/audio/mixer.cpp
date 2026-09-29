#include "audio/mixer.h"
#include "cpu.h"
#include "foundation/runloop.h"
#include "libc/pthread.h"
#include "objc/runtime.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>
#include <windows.h>
#ifdef _WIN32
#include <audioclient.h>
#include <mmdeviceapi.h>
#else
#include <aaudio/AAudio.h>
#endif

#define MINIMP3_IMPLEMENTATION
#include <minimp3/minimp3_ex.h>

namespace libc {
u64 mach_ticks();
}

namespace audio {

std::function<void(u64)> g_on_music_finished;
std::string g_wav_path;

struct Music {
    std::vector<s16> pcm;  // interleaved
    int channels = 2;
    int rate = 44100;
    u64 frames = 0;
    double pos = 0;  // in source frames
    std::atomic<bool> playing{false};
    double start_at = 0;  // NSDate reference seconds; 0 = immediately
    float volume = 1, pan = 0;
    s64 loops = 0;
    u64 owner = 0;
    bool movie = false;  // a movie's soundtrack (speech and music): not affected by the music volume
};

namespace {

struct Bus {
    std::atomic<GuestAddr> proc{0};
    GuestAddr refcon = 0;
    std::atomic<bool> enabled{false};
    std::atomic<float> gain_db{0}, azimuth{0}, distance{0}, rate{1};
    std::atomic<double> input_rate{44100.0};
    std::vector<float> fifo;
    double pos = 0;
    double sample_time = 0;
};

Bus g_buses[kMaxBuses];
std::atomic<bool> g_running{false};
std::atomic<float> g_master{1.0f};
std::atomic<float> g_music_gain{1.0f}, g_effects_gain{1.0f};  // user settings
std::atomic<bool> g_started{false};

std::mutex g_music_mutex;
std::vector<std::shared_ptr<Music>> g_music;  // tracks currently playing or scheduled

// Guest-visible scratch for render callbacks (only the audio thread uses it).
struct CallbackScratch {
    u8* abl;     // AudioBufferList
    u8* ts;      // AudioTimeStamp
    u32* flags;  // AudioUnitRenderActionFlags
    s16* data;
};
constexpr u32 kMaxPull = 8192;

void pull(Bus& b, u32 bus_index, u32 frames, CallbackScratch& s) {
    frames = std::min(frames, kMaxPull);
    // AudioBufferList { u32 mNumberBuffers; pad; AudioBuffer { u32 mNumberChannels; u32 mDataByteSize; void* mData; } }
    *reinterpret_cast<u32*>(s.abl) = 1;
    *reinterpret_cast<u32*>(s.abl + 8) = 1;
    *reinterpret_cast<u32*>(s.abl + 12) = frames * 2;
    *reinterpret_cast<u64*>(s.abl + 16) = gaddr(s.data);
    std::memset(s.ts, 0, 64);
    *reinterpret_cast<double*>(s.ts) = b.sample_time;
    *reinterpret_cast<u64*>(s.ts + 8) = libc::mach_ticks();
    *reinterpret_cast<u32*>(s.ts + 56) = 3;  // sample time + host time valid
    *s.flags = 0;
    std::memset(s.data, 0, frames * 2);
    cpu::current().call(b.proc, {b.refcon, gaddr(s.flags), gaddr(s.ts), bus_index, frames, gaddr(s.abl)});
    b.sample_time += frames;
    bool silent = (*s.flags & 0x10) != 0;  // kAudioUnitRenderAction_OutputIsSilence
    for (u32 i = 0; i < frames; i++) b.fifo.push_back(silent ? 0.0f : s.data[i] / 32768.0f);
}

void mix_buses(float* mix, u32 n, int out_rate, CallbackScratch& s) {
    if (!g_running) return;
    for (u32 bi = 0; bi < kMaxBuses; bi++) {
        Bus& b = g_buses[bi];
        GuestAddr proc = b.proc.load();
        if (!proc || !b.enabled) {
            b.fifo.clear();
            b.pos = 0;
            continue;
        }
        double step = b.input_rate.load() * std::clamp<double>(b.rate.load(), 0.05, 8.0) / out_rate;
        size_t needed = (size_t)std::floor(b.pos + (n - 1) * step) + 2;
        if (b.fifo.size() < needed) {
            u32 want = (u32)(needed - b.fifo.size());
            pull(b, bi, std::max<u32>(want, 256), s);
        }
        float gain = std::pow(10.0f, b.gain_db.load() / 20.0f) * g_effects_gain.load();
        float az = b.azimuth.load() * 3.14159265f / 180.0f;
        float pan = std::clamp(std::sin(az), -1.0f, 1.0f);
        float gl = gain * std::cos((pan + 1) * 0.785398f), gr = gain * std::sin((pan + 1) * 0.785398f);
        for (u32 i = 0; i < n; i++) {
            double p = b.pos + i * step;
            size_t i0 = (size_t)p;
            float t = (float)(p - i0);
            float v = b.fifo[i0] * (1 - t) + b.fifo[i0 + 1] * t;
            mix[2 * i] += v * gl;
            mix[2 * i + 1] += v * gr;
        }
        b.pos += n * step;
        size_t drop = std::min((size_t)b.pos, b.fifo.size());
        b.fifo.erase(b.fifo.begin(), b.fifo.begin() + drop);
        b.pos -= drop;
    }
}

void mix_music(float* mix, u32 n, int out_rate) {
    std::vector<u64> finished;
    {
        std::lock_guard lock(g_music_mutex);
        double now = ns::now_ref();
        for (auto it = g_music.begin(); it != g_music.end();) {
            Music& m = **it;
            if (!m.playing) {
                it = g_music.erase(it);
                continue;
            }
            if (m.start_at > now) {
                ++it;
                continue;
            }
            double step = (double)m.rate / out_rate;
            float vol = m.volume * (m.movie ? 1.0f : g_music_gain.load());
            float gl = vol * std::cos((m.pan + 1) * 0.785398f) * 1.41421f;
            float gr = vol * std::sin((m.pan + 1) * 0.785398f) * 1.41421f;
            bool done = false;
            for (u32 i = 0; i < n; i++) {
                if (m.pos >= m.frames) {
                    if (m.loops != 0) {
                        m.pos -= m.frames;
                        if (m.loops > 0) m.loops--;
                    } else {
                        done = true;
                        break;
                    }
                }
                u64 f0 = (u64)m.pos;
                u64 f1 = std::min(f0 + 1, m.frames - 1);
                float t = (float)(m.pos - f0);
                float l, r;
                if (m.channels >= 2) {
                    l = (m.pcm[f0 * m.channels] * (1 - t) + m.pcm[f1 * m.channels] * t) / 32768.0f;
                    r = (m.pcm[f0 * m.channels + 1] * (1 - t) + m.pcm[f1 * m.channels + 1] * t) / 32768.0f;
                } else {
                    l = r = (m.pcm[f0] * (1 - t) + m.pcm[f1] * t) / 32768.0f;
                }
                mix[2 * i] += l * gl;
                mix[2 * i + 1] += r * gr;
                m.pos += step;
            }
            if (done) {
                m.playing = false;
                m.pos = 0;
                finished.push_back(m.owner);
                it = g_music.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (u64 owner : finished)
        if (g_on_music_finished && owner) g_on_music_finished(owner);
}

// Test mode: mix in real time but write 16-bit stereo 48 kHz to a WAV file instead of the speakers.
void wav_thread(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const int rate = 48000;
    auto header = [&](u32 data_bytes) {
        u32 riff = 36 + data_bytes, fmt_size = 16, byte_rate = rate * 4, sr = rate;
        u16 pcm = 1, ch = 2, align = 4, bits = 16;
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f), std::fwrite(&riff, 4, 1, f), std::fwrite("WAVEfmt ", 1, 8, f);
        std::fwrite(&fmt_size, 4, 1, f), std::fwrite(&pcm, 2, 1, f), std::fwrite(&ch, 2, 1, f), std::fwrite(&sr, 4, 1, f);
        std::fwrite(&byte_rate, 4, 1, f), std::fwrite(&align, 2, 1, f), std::fwrite(&bits, 2, 1, f);
        std::fwrite("data", 1, 4, f), std::fwrite(&data_bytes, 4, 1, f);
        std::fseek(f, 0, SEEK_END);
    };
    header(0);
    CallbackScratch s;
    s.abl = static_cast<u8*>(std::calloc(1, 64));
    s.ts = static_cast<u8*>(std::calloc(1, 64));
    s.flags = static_cast<u32*>(std::calloc(1, 16));
    s.data = static_cast<s16*>(std::calloc(kMaxPull, 2));
    const u32 n = rate / 100;
    std::vector<float> mix;
    std::vector<s16> out(n * 2);
    u32 written = 0;
    u64 next = GetTickCount64();
    for (;;) {
        mix.assign(n * 2, 0.0f);
        u64 pool = objc::pool_push();
        mix_buses(mix.data(), n, rate, s);
        objc::pool_pop(pool);
        mix_music(mix.data(), n, rate);
        for (u32 i = 0; i < n * 2; i++) out[i] = (s16)(std::clamp(mix[i] * g_master.load(), -1.0f, 1.0f) * 32767);
        std::fwrite(out.data(), 2, out.size(), f);
        written += (u32)out.size() * 2;
        if (written % (rate * 4) < n * 4) header(written);  // keep the header valid about once a second
        next += 10;
        u64 now = GetTickCount64();
        if (next > now) Sleep((DWORD)(next - now));
    }
}

#ifndef _WIN32
// Android: AAudio with blocking writes, 48 kHz float stereo.
void audio_thread() {
    if (!g_wav_path.empty()) {
        wav_thread(g_wav_path);
        return;
    }
    AAudioStreamBuilder* builder = nullptr;
    AAudioStream* stream = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        LOG_ERROR("audio: AAudio unavailable; running silent");
        return;
    }
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    bool ok = AAudioStreamBuilder_openStream(builder, &stream) == AAUDIO_OK;
    AAudioStreamBuilder_delete(builder);
    if (!ok || AAudioStream_requestStart(stream) != AAUDIO_OK) {
        LOG_ERROR("audio: could not open an AAudio output stream; running silent");
        return;
    }
    const int rate = AAudioStream_getSampleRate(stream);
    LOG_INFO("audio: AAudio %d Hz, %d-frame bursts", rate, AAudioStream_getFramesPerBurst(stream));

    CallbackScratch s;
    s.abl = static_cast<u8*>(std::calloc(1, 64));
    s.ts = static_cast<u8*>(std::calloc(1, 64));
    s.flags = static_cast<u32*>(std::calloc(1, 16));
    s.data = static_cast<s16*>(std::calloc(kMaxPull, 2));
    std::vector<float> mix;
    const u32 n = rate / 100;  // 10 ms
    for (;;) {
        mix.assign(n * 2, 0.0f);
        u64 pool = objc::pool_push();
        mix_buses(mix.data(), n, rate, s);
        objc::pool_pop(pool);
        mix_music(mix.data(), n, rate);
        float master = g_master.load();
        for (float& v : mix) v = std::clamp(v * master, -1.0f, 1.0f);
        if (AAudioStream_write(stream, mix.data(), (int32_t)n, 100000000 /* 100 ms */) < 0) {
            // Disconnected (e.g. headphones unplugged): reopen on the new default device.
            AAudioStream_close(stream);
            LOG_INFO("audio: output stream lost; reopening");
            audio_thread();
            return;
        }
    }
}
#else
void audio_thread() {
    if (!g_wav_path.empty()) {
        wav_thread(g_wav_path);
        return;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumr = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    UINT32 buffer_frames = 0;
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumr)) &&
              SUCCEEDED(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
              SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) &&
              SUCCEEDED(client->GetMixFormat(&fmt)) &&
              SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 500000 /* 50 ms */, 0, fmt, nullptr)) &&
              SUCCEEDED(client->GetBufferSize(&buffer_frames)) &&
              SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render)) && SUCCEEDED(client->Start());
    if (!ok || fmt->wBitsPerSample != 32) {
        LOG_ERROR("audio: could not open the default output device; running silent");
        return;
    }
    const int channels = fmt->nChannels, rate = (int)fmt->nSamplesPerSec;
    LOG_INFO("audio: WASAPI %d Hz, %d channels, %u-frame buffer", rate, channels, buffer_frames);

    CallbackScratch s;
    s.abl = static_cast<u8*>(std::calloc(1, 64));
    s.ts = static_cast<u8*>(std::calloc(1, 64));
    s.flags = static_cast<u32*>(std::calloc(1, 16));
    s.data = static_cast<s16*>(std::calloc(kMaxPull, 2));
    std::vector<float> mix;
    const UINT32 chunk = rate / 100;  // 10 ms
    for (;;) {
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        UINT32 avail = buffer_frames - padding;
        if (avail < chunk) {
            Sleep(2);
            continue;
        }
        UINT32 n = std::min(avail, chunk * 2);
        mix.assign(n * 2, 0.0f);
        u64 pool = objc::pool_push();
        mix_buses(mix.data(), n, rate, s);
        objc::pool_pop(pool);
        mix_music(mix.data(), n, rate);
        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(n, &out))) continue;
        float* f = reinterpret_cast<float*>(out);
        float master = g_master.load();
        for (UINT32 i = 0; i < n; i++) {
            float l = std::clamp(mix[2 * i] * master, -1.0f, 1.0f), r = std::clamp(mix[2 * i + 1] * master, -1.0f, 1.0f);
            for (int c = 0; c < channels; c++) f[i * channels + c] = c == 0 ? l : c == 1 ? r : 0.0f;
        }
        render->ReleaseBuffer(n, 0);
    }
}
#endif

void ensure_started() {
    bool expected = false;
    if (g_started.compare_exchange_strong(expected, true)) libc::spawn_guest_thread("audio", audio_thread, 1 << 20);
}

}  // namespace

void mixer_set_callback(u32 bus, GuestAddr proc, GuestAddr refcon) {
    if (bus >= kMaxBuses) return;
    g_buses[bus].refcon = refcon;
    g_buses[bus].proc = proc;
}

void mixer_set_param(u32 bus, u32 param, float v) {
    if (bus >= kMaxBuses) return;
    Bus& b = g_buses[bus];
    switch (param) {
    case 0: b.azimuth = v; break;
    case 2: b.distance = v; break;
    case 3: b.gain_db = v; break;
    case 4: b.rate = v; break;
    case 5: b.enabled = v != 0; break;
    default: break;
    }
}

void mixer_set_running(bool running) {
    g_running = running;
    if (running) ensure_started();
}

void mixer_set_input_rate(u32 bus, double hz) {
    if (bus < kMaxBuses && hz > 1000) g_buses[bus].input_rate = hz;
}

void set_master_volume(float v) { g_master = v; }
void set_volumes(float music, float effects) {
    g_music_gain = music;
    g_effects_gain = effects;
}

std::shared_ptr<Music> music_load(const std::string& host_path) {
    mp3dec_t dec;
    mp3dec_file_info_t info{};
    if (mp3dec_load(&dec, host_path.c_str(), &info, nullptr, nullptr) || !info.samples || !info.channels) {
        LOG_WARN("audio: could not decode %s", host_path.c_str());
        std::free(info.buffer);
        return nullptr;
    }
    auto m = std::make_shared<Music>();
    m->channels = info.channels;
    m->rate = info.hz;
    m->frames = info.samples / info.channels;
    m->pcm.assign(info.buffer, info.buffer + info.samples);
    std::free(info.buffer);
    ensure_started();
    return m;
}

std::shared_ptr<Music> music_from_pcm(std::vector<s16> pcm, int channels, int rate) {
    if (pcm.empty() || channels <= 0) return nullptr;
    auto m = std::make_shared<Music>();
    m->channels = channels;
    m->rate = rate;
    m->frames = pcm.size() / channels;
    m->pcm = std::move(pcm);
    m->movie = true;
    ensure_started();
    return m;
}

void music_play(const std::shared_ptr<Music>& m, double start_at) {
    if (!m) return;
    std::lock_guard lock(g_music_mutex);
    m->start_at = start_at;
    if (!m->playing.exchange(true)) g_music.push_back(m);
}
void music_pause(const std::shared_ptr<Music>& m) {
    if (m) m->playing = false;
}
void music_stop(const std::shared_ptr<Music>& m) {
    if (m) m->playing = false;
}
bool music_playing(const std::shared_ptr<Music>& m) { return m && m->playing; }
double music_duration(const std::shared_ptr<Music>& m) { return m ? (double)m->frames / m->rate : 0; }
double music_time(const std::shared_ptr<Music>& m) { return m ? m->pos / m->rate : 0; }
void music_set_time(const std::shared_ptr<Music>& m, double t) {
    if (!m) return;
    std::lock_guard lock(g_music_mutex);
    m->pos = std::clamp(t * m->rate, 0.0, (double)m->frames);
}
void music_set_volume(const std::shared_ptr<Music>& m, float v) {
    if (m) m->volume = v;
}
float music_volume(const std::shared_ptr<Music>& m) { return m ? m->volume : 0; }
void music_set_pan(const std::shared_ptr<Music>& m, float p) {
    if (m) m->pan = std::clamp(p, -1.0f, 1.0f);
}
void music_set_loops(const std::shared_ptr<Music>& m, s64 loops) {
    if (m) m->loops = loops;
}
s64 music_loops(const std::shared_ptr<Music>& m) { return m ? m->loops : 0; }
void music_set_owner(const std::shared_ptr<Music>& m, u64 user) {
    if (m) m->owner = user;
}

}  // namespace audio

#include "audio/video.h"
#include "audio/mixer.h"
#include "foundation/runloop.h"
#include "libc/format.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace video {

struct Movie {
    std::wstring path;
    double duration = 0;
    int width = 0, height = 0;
    std::shared_ptr<audio::Music> sound;  // decoded soundtrack (the playback clock when present)

    std::mutex m;
    std::condition_variable cv;
    bool playing = false, stop_requested = false, restart_requested = false, finished = false, visible = false;
    double clock_base = 0;       // movie time at clock_start
    double clock_start = 0;      // NSDate seconds when (re)started; used without a soundtrack
    std::function<void()> on_finished;
};

namespace {

std::once_flag g_mf_once;
std::mutex g_frame_mutex;
std::shared_ptr<Movie> g_visible;
std::vector<u8> g_frame;
int g_frame_w = 0, g_frame_h = 0;
u64 g_serial = 0;

// Media Foundation is loaded at run time: Windows "N" editions ship without it, and the game
// should still start there (movies are skipped).
struct MediaFoundation {
    HRESULT(WINAPI* Startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* CreateAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* CreateMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* CreateSourceReaderFromURL)(LPCWSTR, IMFAttributes*, IMFSourceReader**) = nullptr;
    bool ok = false;
} g_mf;

void mf_startup() {
    std::call_once(g_mf_once, [] {
        HMODULE plat = LoadLibraryW(L"mfplat.dll"), rw = LoadLibraryW(L"mfreadwrite.dll");
        if (plat && rw) {
            g_mf.Startup = reinterpret_cast<decltype(g_mf.Startup)>(GetProcAddress(plat, "MFStartup"));
            g_mf.CreateAttributes = reinterpret_cast<decltype(g_mf.CreateAttributes)>(GetProcAddress(plat, "MFCreateAttributes"));
            g_mf.CreateMediaType = reinterpret_cast<decltype(g_mf.CreateMediaType)>(GetProcAddress(plat, "MFCreateMediaType"));
            g_mf.CreateSourceReaderFromURL = reinterpret_cast<decltype(g_mf.CreateSourceReaderFromURL)>(
                GetProcAddress(rw, "MFCreateSourceReaderFromURL"));
        }
        g_mf.ok = g_mf.Startup && g_mf.CreateAttributes && g_mf.CreateMediaType && g_mf.CreateSourceReaderFromURL &&
                  SUCCEEDED(g_mf.Startup(MF_VERSION, MFSTARTUP_FULL));
        if (!g_mf.ok)
            LOG_WARN("video: Media Foundation is not available (Windows N edition without the Media Feature Pack?); "
                     "movies will be skipped");
    });
}

IMFSourceReader* make_reader(const std::wstring& path, bool video) {
    mf_startup();
    if (!g_mf.ok) return nullptr;
    IMFAttributes* attrs = nullptr;
    g_mf.CreateAttributes(&attrs, 2);
    if (video) attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* r = nullptr;
    HRESULT hr = g_mf.CreateSourceReaderFromURL(path.c_str(), attrs, &r);
    attrs->Release();
    if (FAILED(hr)) return nullptr;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    DWORD stream = video ? (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM : (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    if (FAILED(r->SetStreamSelection(stream, TRUE))) {
        r->Release();
        return nullptr;
    }
    IMFMediaType* type = nullptr;
    g_mf.CreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, video ? MFMediaType_Video : MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, video ? MFVideoFormat_RGB32 : MFAudioFormat_PCM);
    if (!video) type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    hr = r->SetCurrentMediaType(stream, nullptr, type);
    type->Release();
    if (FAILED(hr)) {
        r->Release();
        return nullptr;
    }
    return r;
}

// Decodes the whole soundtrack into a Music track.
std::shared_ptr<audio::Music> decode_audio(const std::wstring& path) {
    IMFSourceReader* r = make_reader(path, false);
    if (!r) return nullptr;
    IMFMediaType* type = nullptr;
    r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &type);
    UINT32 channels = MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 2);
    UINT32 rate = MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    type->Release();
    std::vector<s16> pcm;
    for (;;) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample))) break;
        if (sample) {
            IMFMediaBuffer* buf = nullptr;
            sample->ConvertToContiguousBuffer(&buf);
            BYTE* data = nullptr;
            DWORD len = 0;
            buf->Lock(&data, nullptr, &len);
            pcm.insert(pcm.end(), reinterpret_cast<s16*>(data), reinterpret_cast<s16*>(data + len));
            buf->Unlock();
            buf->Release();
            sample->Release();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    r->Release();
    return audio::music_from_pcm(std::move(pcm), (int)channels, (int)rate);
}

double movie_clock(Movie& mv) {
    if (mv.sound) return audio::music_time(mv.sound);
    return mv.clock_base + (mv.playing ? ns::now_ref() - mv.clock_start : 0);
}

void publish(Movie* mv, IMFSample* sample) {
    IMFMediaBuffer* buf = nullptr;
    sample->ConvertToContiguousBuffer(&buf);
    BYTE* data = nullptr;
    DWORD len = 0;
    buf->Lock(&data, nullptr, &len);
    int w = mv->width, h = mv->height;
    size_t stride = w ? len / h : 0;
    {
        std::lock_guard lock(g_frame_mutex);
        g_frame.resize((size_t)w * h * 4);
        for (int y = 0; y < h; y++) {
            const u8* src = data + y * stride;  // BGRX, top-down
            u8* dst = &g_frame[(size_t)y * w * 4];
            for (int x = 0; x < w; x++) {
                dst[4 * x + 0] = src[4 * x + 2];
                dst[4 * x + 1] = src[4 * x + 1];
                dst[4 * x + 2] = src[4 * x + 0];
                dst[4 * x + 3] = 255;
            }
        }
        g_frame_w = w;
        g_frame_h = h;
        g_serial++;
    }
    buf->Unlock();
    buf->Release();
}

void decode_loop(Movie* mv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMFSourceReader* r = make_reader(mv->path, true);
    if (!r) {
        LOG_ERROR("video: cannot open %s", libc::wide_to_utf8(mv->path).c_str());
        std::lock_guard lock(mv->m);
        mv->finished = true;
        if (mv->on_finished) mv->on_finished();
        return;
    }
    for (;;) {
        {
            std::unique_lock lock(mv->m);
            mv->cv.wait(lock, [&] { return mv->playing || mv->stop_requested || mv->restart_requested; });
            if (mv->stop_requested) break;
            if (mv->restart_requested) {
                PROPVARIANT pos;
                PropVariantInit(&pos);
                pos.vt = VT_I8;
                pos.hVal.QuadPart = 0;
                r->SetCurrentPosition(GUID_NULL, pos);
                mv->restart_requested = false;
                mv->finished = false;
                mv->clock_base = 0;
                mv->clock_start = ns::now_ref();
                if (mv->sound) audio::music_set_time(mv->sound, 0);
            }
        }
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample))) flags |= MF_SOURCE_READERF_ENDOFSTREAM;
        if (sample) {
            double t = ts / 1e7;
            // Wait until the frame is due (or playback is stopped/paused).
            for (;;) {
                std::unique_lock lock(mv->m);
                if (mv->stop_requested || mv->restart_requested) break;
                double now = movie_clock(*mv);
                if (now >= t - 0.002 && mv->playing) break;
                mv->cv.wait_for(lock, std::chrono::milliseconds(std::clamp((int)((t - now) * 1000), 1, 20)));
            }
            if (mv->visible) publish(mv, sample);
            sample->Release();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            std::function<void()> fn;
            {
                std::unique_lock lock(mv->m);
                mv->finished = true;
                mv->playing = false;
                fn = mv->on_finished;
            }
            LOG_INFO("video: finished %s", libc::wide_to_utf8(mv->path).c_str());
            if (fn) fn();
        }
    }
    r->Release();
}

}  // namespace

std::shared_ptr<Movie> open(const std::string& host_path) {
    mf_startup();
    auto mv = std::make_shared<Movie>();
    mv->path = libc::utf8_to_wide(host_path);
    IMFSourceReader* r = make_reader(mv->path, true);
    if (!r) {
        LOG_ERROR("video: Media Foundation cannot open %s", host_path.c_str());
        return nullptr;
    }
    IMFMediaType* type = nullptr;
    r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    type->Release();
    PROPVARIANT dur;
    PropVariantInit(&dur);
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur)))
        mv->duration = dur.uhVal.QuadPart / 1e7;
    r->Release();
    mv->width = (int)w;
    mv->height = (int)h;
    mv->sound = decode_audio(mv->path);
    LOG_INFO("video: opened %s (%ux%u, %.1f s, %s)", host_path.c_str(), w, h, mv->duration, mv->sound ? "with sound" : "silent");
    std::thread([mv] { decode_loop(mv.get()); }).detach();  // exits (and drops its reference) on stop()
    return mv;
}

void play(const std::shared_ptr<Movie>& mv) {
    if (!mv) return;
    {
        std::lock_guard lock(mv->m);
        if (mv->finished) {
            mv->restart_requested = true;
            mv->finished = false;
        }
        mv->playing = true;
        mv->visible = true;
        mv->clock_start = ns::now_ref();
    }
    if (mv->sound) audio::music_play(mv->sound, 0);
    {
        std::lock_guard lock(g_frame_mutex);
        g_visible = mv;
    }
    mv->cv.notify_all();
}

void pause(const std::shared_ptr<Movie>& mv) {
    if (!mv) return;
    {
        std::lock_guard lock(mv->m);
        mv->clock_base = movie_clock(*mv);
        mv->playing = false;
        mv->visible = false;
    }
    if (mv->sound) audio::music_pause(mv->sound);
    mv->cv.notify_all();
    std::lock_guard lock(g_frame_mutex);
    if (g_visible == mv) {
        g_visible.reset();
        g_frame.clear();
    }
}

void stop(const std::shared_ptr<Movie>& mv) {
    if (!mv) return;
    {
        std::lock_guard lock(mv->m);
        mv->stop_requested = true;
        mv->playing = false;
        mv->visible = false;
    }
    if (mv->sound) audio::music_stop(mv->sound);
    mv->cv.notify_all();
    std::lock_guard lock(g_frame_mutex);
    if (g_visible == mv) {
        g_visible.reset();
        g_frame.clear();
    }
}

void restart(const std::shared_ptr<Movie>& mv) {
    if (!mv) return;
    bool resume;
    {
        std::lock_guard lock(mv->m);
        mv->restart_requested = true;
        resume = mv->visible;
        if (resume) mv->playing = true;
    }
    if (mv->sound) {
        audio::music_set_time(mv->sound, 0);
        if (resume) audio::music_play(mv->sound, 0);
    }
    mv->cv.notify_all();
}

double duration(const std::shared_ptr<Movie>& mv) { return mv ? mv->duration : 0; }
double time(const std::shared_ptr<Movie>& mv) {
    if (!mv) return 0;
    std::lock_guard lock(mv->m);
    return mv->finished ? mv->duration : movie_clock(*mv);
}

void set_on_finished(const std::shared_ptr<Movie>& mv, std::function<void()> fn) {
    if (!mv) return;
    std::lock_guard lock(mv->m);
    mv->on_finished = std::move(fn);
}

bool current_frame(const u8*& rgba, int& w, int& h, u64& serial) {
    g_frame_mutex.lock();
    if (!g_visible || g_frame.empty()) {
        g_frame_mutex.unlock();
        return false;
    }
    rgba = g_frame.data();
    w = g_frame_w;
    h = g_frame_h;
    serial = g_serial;
    return true;  // caller calls release_frame()
}

void release_frame() { g_frame_mutex.unlock(); }

}  // namespace video

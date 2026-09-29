// Movie decoding with Media Foundation (H.264/AAC .m4v), for video.cpp.
#include "audio/video_decoder.h"
#include "libc/format.h"
#include <mutex>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace video {

namespace {

std::once_flag g_mf_once;

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

// Copies a BGRX frame (top-down) into RGBA.
void bgrx_to_rgba(IMFSample* sample, int w, int h, u8* rgba) {
    IMFMediaBuffer* buf = nullptr;
    sample->ConvertToContiguousBuffer(&buf);
    BYTE* data = nullptr;
    DWORD len = 0;
    buf->Lock(&data, nullptr, &len);
    size_t stride = h ? len / h : 0;
    for (int y = 0; y < h; y++) {
        const u8* src = data + y * stride;
        u8* dst = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            dst[4 * x + 0] = src[4 * x + 2];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = src[4 * x + 0];
            dst[4 * x + 3] = 255;
        }
    }
    buf->Unlock();
    buf->Release();
}

struct MfDecoder : Decoder {
    IMFSourceReader* r = nullptr;
    IMFSample* held = nullptr;
    int w = 0, h = 0;

    ~MfDecoder() override {
        if (held) held->Release();
        if (r) r->Release();
    }
    bool next(double& t) override {
        for (;;) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample))) return false;
            if (sample) {
                held = sample;
                t = ts / 1e7;
                return true;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
        }
    }
    void take(u8* rgba) override {
        if (!held) return;
        if (rgba) bgrx_to_rgba(held, w, h, rgba);
        held->Release();
        held = nullptr;
    }
    void rewind() override {
        take(nullptr);
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = 0;
        r->SetCurrentPosition(GUID_NULL, pos);
    }
};

}  // namespace

bool probe(const std::string& path, MovieInfo& info) {
    std::wstring wpath = libc::utf8_to_wide(path);
    IMFSourceReader* r = make_reader(wpath, true);
    if (!r) return false;
    IMFMediaType* type = nullptr;
    r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    type->Release();
    PROPVARIANT dur;
    PropVariantInit(&dur);
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur)))
        info.duration = dur.uhVal.QuadPart / 1e7;
    r->Release();
    info.width = (int)w;
    info.height = (int)h;
    info.sound = decode_audio(wpath);
    return true;
}

std::unique_ptr<Decoder> make_decoder(const std::string& path, const MovieInfo& info) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto d = std::make_unique<MfDecoder>();
    d->r = make_reader(libc::utf8_to_wide(path), true);
    if (!d->r) return nullptr;
    d->w = info.width;
    d->h = info.height;
    return d;
}

bool grab_frame(const std::wstring& path, double seconds, std::vector<u8>& rgba, int& w, int& h) {
    IMFSourceReader* r = make_reader(path, true);
    if (!r) return false;
    UINT32 uw = 0, uh = 0;
    IMFMediaType* type = nullptr;
    if (SUCCEEDED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type))) {
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &uw, &uh);
        type->Release();
    }
    LONGLONG target = (LONGLONG)(seconds * 1e7);
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = target;
    r->SetCurrentPosition(GUID_NULL, pos);  // lands on the key frame before `target`
    bool ok = false;
    for (int i = 0; i < 1000 && !ok && uw && uh; i++) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (sample) sample->Release();
            break;
        }
        if (!sample) continue;
        if (ts + 400000 >= target) {  // within 40 ms
            w = (int)uw;
            h = (int)uh;
            rgba.resize((size_t)w * h * 4);
            bgrx_to_rgba(sample, w, h, rgba.data());
            ok = true;
        }
        sample->Release();
    }
    r->Release();
    return ok;
}

}  // namespace video

// Movie decoding on Android with the NDK media APIs (for audio/video.cpp): the soundtrack is
// decoded up front, video frames by the hardware decoder into byte buffers (the codec describes
// their YUV layout; reading its image surfaces directly is unreliable across vendors) and
// converted to RGBA.
#include "audio/video_decoder.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

namespace video {

namespace {

constexpr int64_t kTimeoutUs = 10000;
constexpr int32_t kColorFormatYUV420Flexible = 0x7F420888, kColorFormatYUV420Planar = 19, kColorFormatYUV420SemiPlanar = 21;

// An extractor over a file, with one track selected (the first whose MIME type starts with `kind`).
struct Source {
    int fd = -1;
    AMediaExtractor* ex = nullptr;
    AMediaFormat* format = nullptr;  // of the selected track
    std::string mime;

    ~Source() {
        if (format) AMediaFormat_delete(format);
        if (ex) AMediaExtractor_delete(ex);
        if (fd >= 0) close(fd);
    }
    bool open(const std::string& path, const char* kind) {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0) return false;
        ex = AMediaExtractor_new();
        if (AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) != AMEDIA_OK) return false;
        size_t n = AMediaExtractor_getTrackCount(ex);
        for (size_t i = 0; i < n; i++) {
            AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
            const char* m = nullptr;
            if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m && strncmp(m, kind, strlen(kind)) == 0) {
                mime = m;
                format = f;
                AMediaExtractor_selectTrack(ex, i);
                return true;
            }
            AMediaFormat_delete(f);
        }
        return false;
    }
    // Queues the next sample into the codec; false once the end of the stream was queued.
    bool feed(AMediaCodec* codec) {
        ssize_t in = AMediaCodec_dequeueInputBuffer(codec, kTimeoutUs);
        if (in < 0) return true;
        size_t cap = 0;
        u8* buf = AMediaCodec_getInputBuffer(codec, in, &cap);
        ssize_t n = AMediaExtractor_readSampleData(ex, buf, cap);
        if (n < 0) {
            AMediaCodec_queueInputBuffer(codec, in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            return false;
        }
        AMediaCodec_queueInputBuffer(codec, in, 0, (size_t)n, AMediaExtractor_getSampleTime(ex), 0);
        AMediaExtractor_advance(ex);
        return true;
    }
};

std::shared_ptr<audio::Music> decode_audio(const std::string& path) {
    Source src;
    if (!src.open(path, "audio/")) return nullptr;
    AMediaCodec* codec = AMediaCodec_createDecoderByType(src.mime.c_str());
    if (!codec) return nullptr;
    if (AMediaCodec_configure(codec, src.format, nullptr, nullptr, 0) != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) {
        AMediaCodec_delete(codec);
        return nullptr;
    }
    int32_t channels = 2, rate = 44100, encoding = 2;  // 2 = 16-bit PCM
    AMediaFormat_getInt32(src.format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
    AMediaFormat_getInt32(src.format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
    std::vector<s16> pcm;
    bool input = true;
    for (int idle = 0; idle < 200;) {
        if (input) input = src.feed(codec);
        AMediaCodecBufferInfo info;
        ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, kTimeoutUs);
        if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(codec);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_PCM_ENCODING, &encoding);
            AMediaFormat_delete(f);
            continue;
        }
        if (out < 0) {
            if (!input) idle++;
            continue;
        }
        idle = 0;
        size_t size = 0;
        const u8* data = AMediaCodec_getOutputBuffer(codec, out, &size) + info.offset;
        if (encoding == 4) {  // float
            for (int32_t i = 0; i + 4 <= info.size; i += 4) {
                float v;
                std::memcpy(&v, data + i, 4);
                pcm.push_back((s16)(std::clamp(v, -1.0f, 1.0f) * 32767));
            }
        } else {
            pcm.insert(pcm.end(), reinterpret_cast<const s16*>(data), reinterpret_cast<const s16*>(data + info.size));
        }
        AMediaCodec_releaseOutputBuffer(codec, out, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
    }
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    if (pcm.empty()) return nullptr;
    return audio::music_from_pcm(std::move(pcm), channels, rate);
}

// Android's MediaImage2 ("image-data" in a flexible-YUV output format): where each plane is.
struct MediaImage2 {
    u32 type, num_planes, width, height, bit_depth, bit_depth_allocated;
    struct Plane {
        u32 offset;
        s32 col_inc, row_inc;
        u32 horiz_subsampling, vert_subsampling;
    } planes[4];
};

// YUV 4:2:0 plane layout of the decoder's output buffers.
struct Layout {
    size_t y = 0, u = 0, v = 0;  // offsets
    int y_row = 0, u_row = 0, v_row = 0;
    int u_col = 1, v_col = 1;  // bytes between horizontally adjacent chroma samples
    int left = 0, top = 0;     // crop origin
};

bool read_layout(AMediaFormat* f, int w, int h, Layout& l) {
    int32_t color = 0, stride = w, slice = h, cl = 0, ct = 0, cr = 0, cb = 0;
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &stride);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice);
    if (AMediaFormat_getRect(f, AMEDIAFORMAT_KEY_DISPLAY_CROP, &cl, &ct, &cr, &cb)) l.left = cl, l.top = ct;
    if (stride <= 0) stride = w;
    if (slice <= 0) slice = h;
    void* blob = nullptr;
    size_t blob_size = 0;
    if (AMediaFormat_getBuffer(f, "image-data", &blob, &blob_size) && blob_size >= sizeof(MediaImage2)) {
        MediaImage2 mi;
        std::memcpy(&mi, blob, sizeof mi);
        if (mi.type == 1 /* YUV */ && mi.num_planes >= 3 && mi.bit_depth == 8) {
            l.y = mi.planes[0].offset, l.u = mi.planes[1].offset, l.v = mi.planes[2].offset;
            l.y_row = mi.planes[0].row_inc, l.u_row = mi.planes[1].row_inc, l.v_row = mi.planes[2].row_inc;
            l.u_col = mi.planes[1].col_inc, l.v_col = mi.planes[2].col_inc;
            return true;
        }
    }
    l.y = 0;
    l.y_row = stride;
    if (color == kColorFormatYUV420SemiPlanar) {  // NV12
        l.u = (size_t)stride * slice, l.v = l.u + 1;
        l.u_row = l.v_row = stride;
        l.u_col = l.v_col = 2;
        return true;
    }
    if (color == kColorFormatYUV420Planar) {  // I420
        l.u = (size_t)stride * slice, l.v = l.u + (size_t)(stride / 2) * (slice / 2);
        l.u_row = l.v_row = stride / 2;
        l.u_col = l.v_col = 1;
        return true;
    }
    LOG_ERROR("video: unsupported decoder output (color format 0x%x)", color);
    return false;
}

u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

// YUV 4:2:0 to RGBA with BT.709 (HD) or BT.601 coefficients, video range.
void to_rgba(const u8* data, size_t size, const Layout& l, int w, int h, u8* rgba) {
    const bool hd = h >= 720;
    const int kr = hd ? 459 : 409, kgu = hd ? 55 : 100, kgv = hd ? 136 : 208, kb = hd ? 541 : 516;
    for (int y = 0; y < h; y++) {
        int sy = y + l.top;
        const u8* row_y = data + l.y + (size_t)sy * l.y_row;
        const u8* row_u = data + l.u + (size_t)(sy / 2) * l.u_row;
        const u8* row_v = data + l.v + (size_t)(sy / 2) * l.v_row;
        size_t end = (size_t)((l.left + w + 1) / 2) * std::max(l.u_col, l.v_col);
        if (row_y + l.left + w > data + size || row_u + end > data + size || row_v + end > data + size) break;  // short buffer
        u8* d = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            int sx = x + l.left, cx = sx / 2;
            int c = (row_y[sx] - 16) * 298, u = row_u[cx * l.u_col] - 128, v = row_v[cx * l.v_col] - 128;
            d[0] = clamp8((c + kr * v + 128) >> 8);
            d[1] = clamp8((c - kgu * u - kgv * v + 128) >> 8);
            d[2] = clamp8((c + kb * u + 128) >> 8);
            d[3] = 255;
            d += 4;
        }
    }
}

struct CodecDecoder : Decoder {
    Source src;
    AMediaCodec* codec = nullptr;
    Layout layout;
    bool have_layout = false;
    int w = 0, h = 0;
    bool input = true, ended = false;
    ssize_t held = -1;
    AMediaCodecBufferInfo held_info{};

    ~CodecDecoder() override {
        if (codec) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
        }
    }

    bool start(const std::string& path, const MovieInfo& info) {
        w = info.width;
        h = info.height;
        if (!src.open(path, "video/")) return false;
        AMediaFormat_setInt32(src.format, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatYUV420Flexible);
        codec = AMediaCodec_createDecoderByType(src.mime.c_str());
        return codec && AMediaCodec_configure(codec, src.format, nullptr, nullptr, 0) == AMEDIA_OK &&
               AMediaCodec_start(codec) == AMEDIA_OK;
    }

    bool next(double& t) override {
        if (ended) return false;
        for (int idle = 0; idle < 300;) {
            if (input) input = src.feed(codec);
            AMediaCodecBufferInfo info;
            ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, kTimeoutUs);
            if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* f = AMediaCodec_getOutputFormat(codec);
                have_layout = read_layout(f, w, h, layout);
                AMediaFormat_delete(f);
                continue;
            }
            if (out >= 0) {
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) ended = true;
                if (info.size <= 0) {
                    AMediaCodec_releaseOutputBuffer(codec, out, false);
                    if (ended) return false;
                    continue;
                }
                held = out;
                held_info = info;
                t = info.presentationTimeUs / 1e6;
                return true;
            }
            if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER && !input) idle++;
        }
        return false;  // the decoder stopped producing frames
    }

    void take(u8* rgba) override {
        if (held < 0) return;
        if (rgba && have_layout) {
            size_t size = 0;
            const u8* data = AMediaCodec_getOutputBuffer(codec, held, &size);
            if (data) to_rgba(data + held_info.offset, (size_t)held_info.size, layout, w, h, rgba);
        }
        AMediaCodec_releaseOutputBuffer(codec, held, false);
        held = -1;
    }

    void rewind() override {
        take(nullptr);
        AMediaCodec_flush(codec);
        AMediaExtractor_seekTo(src.ex, 0, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
        input = true;
        ended = false;
    }
};

}  // namespace

bool probe(const std::string& path, MovieInfo& info) {
    Source src;
    if (!src.open(path, "video/")) return false;
    int32_t w = 0, h = 0;
    int64_t us = 0;
    AMediaFormat_getInt32(src.format, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(src.format, AMEDIAFORMAT_KEY_HEIGHT, &h);
    if (AMediaFormat_getInt64(src.format, AMEDIAFORMAT_KEY_DURATION, &us)) info.duration = us / 1e6;
    info.width = w;
    info.height = h;
    info.sound = decode_audio(path);
    return true;
}

std::unique_ptr<Decoder> make_decoder(const std::string& path, const MovieInfo& info) {
    auto d = std::make_unique<CodecDecoder>();
    if (!d->start(path, info)) return nullptr;
    return d;
}

bool grab_frame(const std::wstring&, double, std::vector<u8>&, int&, int&) { return false; }  // launcher only

}  // namespace video

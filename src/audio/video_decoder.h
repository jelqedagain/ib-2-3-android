// The platform part of movie playback (video.cpp): Media Foundation on Windows (video_mf.cpp),
// MediaCodec on Android (port/android/video.cpp).
#pragma once
#include "audio/mixer.h"
#include "audio/video.h"
#include <memory>
#include <string>

namespace video {

struct MovieInfo {
    int width = 0, height = 0;
    double duration = 0;
    std::shared_ptr<audio::Music> sound;  // the whole soundtrack, decoded (nullptr: silent)
};

// Reads a movie's size and duration and decodes its soundtrack.
bool probe(const std::string& path, MovieInfo& info);

// Decodes the video frames of one movie; used by a single thread.
struct Decoder {
    virtual ~Decoder() = default;
    // Decodes the next frame and holds it; false at the end. `t` is the frame's time in seconds.
    virtual bool next(double& t) = 0;
    // Releases the held frame, converting it first into `rgba` (width * height * 4, top row first) if set.
    virtual void take(u8* rgba) = 0;
    virtual void rewind() = 0;
};
std::unique_ptr<Decoder> make_decoder(const std::string& path, const MovieInfo& info);

}  // namespace video

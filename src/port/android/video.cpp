// Movie playback on Android: not implemented yet (MediaCodec later). The game treats every movie
// as undecodable and skips it after a moment (see audio/av.cpp).
#include "audio/video.h"

namespace video {

std::shared_ptr<Movie> open(const std::string&) { return nullptr; }
void play(const std::shared_ptr<Movie>&) {}
void pause(const std::shared_ptr<Movie>&) {}
void stop(const std::shared_ptr<Movie>&) {}
void restart(const std::shared_ptr<Movie>&) {}
double duration(const std::shared_ptr<Movie>&) { return 0; }
double time(const std::shared_ptr<Movie>&) { return 0; }
void set_on_finished(const std::shared_ptr<Movie>&, std::function<void()>) {}
bool current_frame(const u8*&, int&, int&, u64&) { return false; }
void release_frame() {}
bool grab_frame(const std::wstring&, double, std::vector<u8>&, int&, int&) { return false; }

}  // namespace video

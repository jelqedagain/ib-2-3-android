// Movie playback (H.264/AAC .m4v) with Media Foundation, shown in the game window.
#pragma once
#include "common.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace video {

struct Movie;

std::shared_ptr<Movie> open(const std::string& host_path);
void play(const std::shared_ptr<Movie>& m);
void pause(const std::shared_ptr<Movie>& m);
void stop(const std::shared_ptr<Movie>& m);  // stops and hides
void restart(const std::shared_ptr<Movie>& m);
double duration(const std::shared_ptr<Movie>& m);
double time(const std::shared_ptr<Movie>& m);
// Called once (on the decode thread) when playback reaches the end.
void set_on_finished(const std::shared_ptr<Movie>& m, std::function<void()> fn);

// For the presenter: the frame to show, if a movie is on screen. RGBA, top row first.
// Returns false when no movie is visible; `serial` changes whenever the pixels change.
bool current_frame(const u8*& rgba, int& w, int& h, u64& serial);
void release_frame();  // must follow a successful current_frame()

}  // namespace video

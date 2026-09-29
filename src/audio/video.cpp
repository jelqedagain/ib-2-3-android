// Movie playback: timing, pause/restart and the frame shown by the presenter. The soundtrack is
// played as a Music track and is the clock when present. Decoding is platform code (video_decoder.h).
#include "audio/video.h"
#include "audio/video_decoder.h"
#include "foundation/runloop.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace video {

struct Movie {
    std::string path;
    MovieInfo info;

    std::mutex m;
    std::condition_variable cv;
    bool playing = false, stop_requested = false, restart_requested = false, finished = false, visible = false;
    double clock_base = 0;   // movie time at clock_start
    double clock_start = 0;  // NSDate seconds when (re)started; used without a soundtrack
    std::function<void()> on_finished;
};

namespace {

std::mutex g_frame_mutex;
std::shared_ptr<Movie> g_visible;
std::vector<u8> g_frame;
int g_frame_w = 0, g_frame_h = 0;
u64 g_serial = 0;

double movie_clock(Movie& mv) {
    if (mv.info.sound) return audio::music_time(mv.info.sound);
    return mv.clock_base + (mv.playing ? ns::now_ref() - mv.clock_start : 0);
}

void finish(Movie* mv) {
    std::function<void()> fn;
    {
        std::unique_lock lock(mv->m);
        mv->finished = true;
        mv->playing = false;
        fn = mv->on_finished;
    }
    LOG_INFO("video: finished %s", mv->path.c_str());
    if (fn) fn();
}

void decode_loop(Movie* mv) {
    std::unique_ptr<Decoder> dec = make_decoder(mv->path, mv->info);
    if (!dec) {
        LOG_ERROR("video: cannot decode %s", mv->path.c_str());
        finish(mv);
        return;
    }
    for (;;) {
        {
            std::unique_lock lock(mv->m);
            mv->cv.wait(lock, [&] { return mv->playing || mv->stop_requested || mv->restart_requested; });
            if (mv->stop_requested) break;
            if (mv->restart_requested) {
                dec->rewind();
                mv->restart_requested = false;
                mv->finished = false;
                mv->clock_base = 0;
                mv->clock_start = ns::now_ref();
                if (mv->info.sound) audio::music_set_time(mv->info.sound, 0);
            }
        }
        double t = 0;
        if (!dec->next(t)) {
            finish(mv);
            continue;
        }
        // Wait until the frame is due (or playback is stopped/paused).
        for (;;) {
            std::unique_lock lock(mv->m);
            if (mv->stop_requested || mv->restart_requested) break;
            double now = movie_clock(*mv);
            if (now >= t - 0.002 && mv->playing) break;
            mv->cv.wait_for(lock, std::chrono::milliseconds(std::clamp((int)((t - now) * 1000), 1, 20)));
        }
        if (mv->visible) {
            std::lock_guard lock(g_frame_mutex);
            g_frame.resize((size_t)mv->info.width * mv->info.height * 4);
            dec->take(g_frame.data());
            g_frame_w = mv->info.width;
            g_frame_h = mv->info.height;
            g_serial++;
        } else {
            dec->take(nullptr);
        }
    }
}

}  // namespace

std::shared_ptr<Movie> open(const std::string& host_path) {
    auto mv = std::make_shared<Movie>();
    mv->path = host_path;
    if (!probe(host_path, mv->info) || mv->info.width <= 0 || mv->info.height <= 0) {
        LOG_ERROR("video: cannot open %s", host_path.c_str());
        return nullptr;
    }
    LOG_INFO("video: opened %s (%dx%d, %.1f s, %s)", host_path.c_str(), mv->info.width, mv->info.height, mv->info.duration,
             mv->info.sound ? "with sound" : "silent");
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
    if (mv->info.sound) audio::music_play(mv->info.sound, 0);
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
    if (mv->info.sound) audio::music_pause(mv->info.sound);
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
    if (mv->info.sound) audio::music_stop(mv->info.sound);
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
    if (mv->info.sound) {
        audio::music_set_time(mv->info.sound, 0);
        if (resume) audio::music_play(mv->info.sound, 0);
    }
    mv->cv.notify_all();
}

double duration(const std::shared_ptr<Movie>& mv) { return mv ? mv->info.duration : 0; }
double time(const std::shared_ptr<Movie>& mv) {
    if (!mv) return 0;
    std::lock_guard lock(mv->m);
    return mv->finished ? mv->info.duration : movie_clock(*mv);
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

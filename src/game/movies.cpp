// Startup movies. -[FMovieHelper init] starts "Startup" (the Unreal/ChAIR logos) at launch and
// records it as the first startup movie; when the engine later runs InitiateStartupSequence, the
// sequence continues after it only if it is still playing. A fast device always gets there in
// time. Loading here can take longer than the 16 s movie, and then the list restarted with
// "Startup", playing the logos twice. Drop the finished first movie so the sequence continues
// with StartupLoop, as it does on an iPhone.
#include "foundation/foundation.h"
#include "game/game.h"
#include "hook.h"
#include "objc/runtime.h"

namespace game {

namespace {

GuestAddr g_initiate_original = 0;

void on_initiate_startup_sequence(cpu::Thread& t) {
    objc::id self = t.x(0);
    bool playing = objc::send(self, "bIsPlaying") & 0xff;
    objc::id movies = objc::send(self, "StartupMovies");
    if (!playing && movies && objc::send(movies, "count") > 0 &&
        ns::utf8(objc::send(movies, "objectAtIndex:", {0})) == "Startup") {
        LOG_INFO("startup movie already finished; continuing with the rest of the startup sequence");
        objc::send(movies, "removeObjectAtIndex:", {0});
    }
    t.jump(g_initiate_original);
}

// IB3's Origins recap (about 2.5 minutes at the start of a new game, and Options, Play Origins) could
// not be skipped. A tap during a movie skips it if FMovieHelper's SkippableMovies has its name; the
// recap is added to it.
GuestAddr g_play_original = 0;

void on_play_movie(cpu::Thread& t) {
    objc::id self = t.x(0), name = t.x(2);
    std::string movie = name ? ns::utf8(name) : "";
    objc::id skippable = objc::send(self, "SkippableMovies");
    LOG_INFO("movies: PlayMovie %s (%llu skippable)", movie.c_str(),
             (unsigned long long)(skippable ? objc::send(skippable, "count") : 0));
    if (skippable && movie.find("Origins") != std::string::npos && !(objc::send(skippable, "containsObject:", {name}) & 0xff)) {
        objc::send(skippable, "addObject:", {name});
        if (movie.size() > 2 && movie[1] == ',')  // "s,Name" / "l,Name": the game compares the name without the play-mode prefix
            objc::send(skippable, "addObject:", {objc::send(name, "substringFromIndex:", {2})});
        LOG_INFO("movies: %s can be skipped with a tap", movie.c_str());
    }
    t.jump(g_play_original);
}

}  // namespace

void install_startup_movie_fix() {
    objc::Class helper = objc::class_named("FMovieHelper");
    GuestAddr imp = helper ? objc::lookup_imp(helper, objc::sel("InitiateStartupSequence")) : 0;
    if (!imp) {
        LOG_WARN("FMovieHelper InitiateStartupSequence not found");
        return;
    }
    g_initiate_original = hook::install(imp, "-[FMovieHelper InitiateStartupSequence]", on_initiate_startup_sequence);
    if (is_ib2()) return;
    if (GuestAddr play = objc::lookup_imp(helper, objc::sel("PlayMovie:")))
        g_play_original = hook::install(play, "-[FMovieHelper PlayMovie:]", on_play_movie);
}

}  // namespace game

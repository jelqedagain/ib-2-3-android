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

}  // namespace

void install_startup_movie_fix() {
    objc::Class helper = objc::class_named("FMovieHelper");
    GuestAddr imp = helper ? objc::lookup_imp(helper, objc::sel("InitiateStartupSequence")) : 0;
    if (!imp) {
        LOG_WARN("FMovieHelper InitiateStartupSequence not found");
        return;
    }
    g_initiate_original = hook::install(imp, "-[FMovieHelper InitiateStartupSequence]", on_initiate_startup_sequence);
}

}  // namespace game

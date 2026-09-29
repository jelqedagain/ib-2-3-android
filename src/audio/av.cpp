// AVFoundation / CoreMedia / AudioToolbox.
//
// Movies currently "play" instantly (they report completion right away) and audio output is silent;
// real playback replaces these once the game renders.
#include "foundation/foundation.h"
#include "foundation/runloop.h"
#include "objc/internal.h"
#include "audio/mixer.h"
#include "audio/video.h"
#include "libc/vfs.h"
#include <atomic>

namespace audio {

using namespace ns;
using objc::Class;
using objc::id;
using objc::SEL;

namespace {

// CMTime: value (s64), timescale (s32), flags (u32), epoch (s64) — 24 bytes, returned via x8.
struct CMTime {
    s64 value;
    s32 timescale;
    u32 flags;
    s64 epoch;
};
CMTime cm_seconds(double s) { return {(s64)(s * 600), 600, 1, 0}; }
void ret_cmtime(cpu::Thread& t, CMTime v) { std::memcpy(gptr<void>(t.x(8)), &v, sizeof v); }

struct PlayerData : objc::HostData {
    id item = 0;
    double rate = 0;
    double started = 0;
    bool finished = false;
};
struct ItemData : objc::HostData {
    id asset = 0;
};
struct AssetData : objc::HostData {
    id url = 0;
    std::shared_ptr<video::Movie> movie;
    ~AssetData() override { video::stop(movie); }
};

std::shared_ptr<video::Movie> movie_of_item(id item) {
    auto* d = objc::get<ItemData>(item);
    if (!d || !d->asset) return nullptr;
    auto* a = objc::get<AssetData>(d->asset);
    return a ? a->movie : nullptr;
}
std::shared_ptr<video::Movie> movie_of_player(id player);
struct MusicData : objc::HostData {
    id url = 0, delegate = 0;
    std::shared_ptr<Music> music;
    ~MusicData() override { music_stop(music); }
};

constexpr double kFakeMovieSeconds = 0.1;

// AudioToolbox: an opaque graph whose nodes are small integers.
struct Graph {
    int next_node = 1;
    bool running = false;
    std::vector<std::pair<u32, GuestAddr>> callbacks;  // bus -> render callback
};

}  // namespace

void install() {
    using hle::fn;
    using objc::class_method;
    using objc::method;

    // ---------------- CoreMedia ----------------
    static CMTime s_zero{0, 1, 1, 0};
    hle::data("_kCMTimeZero", gaddr(&s_zero));
    fn("_CMTimeGetSeconds", [](const CMTime* t) -> double {
        if (!t || !(t->flags & 1) || !t->timescale) return 0;
        return (double)t->value / t->timescale;
    });

    // ---------------- AVAudioSession ----------------
    Class AS = objc::host_class("AVAudioSession");
    class_method(AS, "sharedInstance", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    for (const char* s : {"setCategory:error:", "setActive:error:", "setMode:error:", "setPreferredSampleRate:error:",
                          "setPreferredIOBufferDuration:error:", "overrideOutputAudioPort:error:"})
        method(AS, s, [](id, SEL, u64, u64* err) {
            if (err) *err = 0;
            return true;
        });
    method(AS, "setCategory:withOptions:error:", [](id, SEL, id, u64, u64* err) {
        if (err) *err = 0;
        return true;
    });
    method(AS, "setDelegate:", [](id, SEL, id) {});
    method(AS, "setPreferredHardwareSampleRate:error:", [](id, SEL, double, u64* err) {
        if (err) *err = 0;
        return true;
    });
    method(AS, "currentHardwareSampleRate", [](id, SEL) { return 44100.0; });
    method(AS, "preferredHardwareSampleRate", [](id, SEL) { return 44100.0; });
    method(AS, "category", [](id, SEL) { return str("AVAudioSessionCategoryAmbient"); });
    method(AS, "isOtherAudioPlaying", [](id, SEL) { return false; });
    method(AS, "otherAudioPlaying", [](id, SEL) { return false; });
    method(AS, "secondaryAudioShouldBeSilencedHint", [](id, SEL) { return false; });
    method(AS, "sampleRate", [](id, SEL) { return 44100.0; });
    method(AS, "preferredSampleRate", [](id, SEL) { return 44100.0; });
    method(AS, "IOBufferDuration", [](id, SEL) { return 0.023; });
    method(AS, "outputVolume", [](id, SEL) { return 1.0f; });
    method(AS, "outputNumberOfChannels", [](id, SEL) -> s64 { return 2; });
    method(AS, "requestRecordPermission:", [](id, SEL, GuestAddr b) {
        if (b) objc::call_block(b, {0});
    });

    // ---------------- AVAudioPlayer (music) ----------------
    Class AP = objc::host_class("AVAudioPlayer");
    g_on_music_finished = [](u64 player) {
        objc::retain(player);
        post_to_main([player] {
            id delegate = objc::ensure<MusicData>(player).delegate;
            if (delegate && objc::responds_to(delegate, objc::sel("audioPlayerDidFinishPlaying:successfully:")))
                objc::send(delegate, "audioPlayerDidFinishPlaying:successfully:", {player, 1});
            objc::release(player);
        });
    };
    method(AP, "initWithContentsOfURL:error:", [](id self, SEL, id url, u64* err) -> id {
        if (err) *err = 0;
        auto& d = objc::ensure<MusicData>(self);
        d.url = objc::retain(url);
        std::string path = utf8(objc::send(url, "path"));
        d.music = music_load(vfs::to_host(path.c_str()));
        if (!d.music) {
            objc::release(self);
            return 0;
        }
        music_set_owner(d.music, self);
        LOG_DEBUG("AVAudioPlayer %s (%.1f s)", path.c_str(), music_duration(d.music));
        return self;
    });
    method(AP, "initWithData:error:", [](id self, SEL, id, u64* err) -> id {
        LOG_WARN("AVAudioPlayer initWithData: not supported");
        if (err) *err = 0;
        objc::release(self);
        return 0;
    });
    method(AP, "url", [](id self, SEL) { return objc::ensure<MusicData>(self).url; });
    method(AP, "prepareToPlay", [](id, SEL) { return true; });
    method(AP, "play", [](id self, SEL) {
        music_play(objc::ensure<MusicData>(self).music, 0);
        return true;
    });
    method(AP, "playAtTime:", [](id self, SEL, double t) {
        music_play(objc::ensure<MusicData>(self).music, t);
        return true;
    });
    method(AP, "pause", [](id self, SEL) { music_pause(objc::ensure<MusicData>(self).music); });
    method(AP, "stop", [](id self, SEL) { music_stop(objc::ensure<MusicData>(self).music); });
    method(AP, "isPlaying", [](id self, SEL) { return music_playing(objc::ensure<MusicData>(self).music); });
    method(AP, "setVolume:", [](id self, SEL, float v) { music_set_volume(objc::ensure<MusicData>(self).music, v); });
    method(AP, "volume", [](id self, SEL) { return music_volume(objc::ensure<MusicData>(self).music); });
    method(AP, "setPan:", [](id self, SEL, float p) { music_set_pan(objc::ensure<MusicData>(self).music, p); });
    method(AP, "setNumberOfLoops:", [](id self, SEL, s64 n) { music_set_loops(objc::ensure<MusicData>(self).music, n); });
    method(AP, "numberOfLoops", [](id self, SEL) -> s64 { return music_loops(objc::ensure<MusicData>(self).music); });
    method(AP, "setDelegate:", [](id self, SEL, id d) { objc::ensure<MusicData>(self).delegate = d; });
    method(AP, "delegate", [](id self, SEL) { return objc::ensure<MusicData>(self).delegate; });
    method(AP, "duration", [](id self, SEL) { return music_duration(objc::ensure<MusicData>(self).music); });
    method(AP, "currentTime", [](id self, SEL) { return music_time(objc::ensure<MusicData>(self).music); });
    method(AP, "setCurrentTime:", [](id self, SEL, double t) { music_set_time(objc::ensure<MusicData>(self).music, t); });
    method(AP, "deviceCurrentTime", [](id, SEL) { return now_ref(); });
    method(AP, "numberOfChannels", [](id, SEL) -> u64 { return 2; });
    method(AP, "setMeteringEnabled:", [](id, SEL, bool) {});
    method(AP, "setEnableRate:", [](id, SEL, bool) {});
    method(AP, "setRate:", [](id, SEL, float) {});

    // ---------------- AVURLAsset / AVPlayerItem / AVPlayer / AVPlayerLayer (video) ----------------
    Class ASSET = objc::host_class("AVURLAsset", "AVAsset");
    auto open_asset = [](id a, id url) {
        auto& d = objc::ensure<AssetData>(a);
        d.url = objc::retain(url);
        std::string path = utf8(objc::send(url, "path"));
        d.movie = video::open(vfs::to_host(path.c_str()));
        if (logging::enabled(logging::Level::Debug))
            LOG_DEBUG("movie %s opened from:\n%s", path.c_str(), cpu::current().backtrace().c_str());
    };
    static decltype(open_asset) s_open_asset = open_asset;
    class_method(ASSET, "URLAssetWithURL:options:", [](Class c, SEL, id url, id) {
        id a = objc::alloc(c);
        s_open_asset(a, url);
        return objc::autorelease(a);
    });
    method(ASSET, "initWithURL:options:", [](id self, SEL, id url, id) {
        s_open_asset(self, url);
        return self;
    });
    objc::add_method(ASSET, "duration", [](cpu::Thread& t) {
        auto* d = objc::get<AssetData>(t.x(0));
        ret_cmtime(t, cm_seconds(d && d->movie ? video::duration(d->movie) : kFakeMovieSeconds));
    });
    method(ASSET, "URL", [](id self, SEL) { return objc::ensure<AssetData>(self).url; });
    method(ASSET, "tracksWithMediaType:", [](id, SEL, id) { return array({}); });
    method(ASSET, "loadValuesAsynchronouslyForKeys:completionHandler:", [](id, SEL, id, GuestAddr b) {
        if (!b) return;
        GuestAddr bb = objc::block_copy(b);
        post_to_main([bb] {
            objc::call_block(bb);
            objc::block_release(bb);
        });
    });
    method(ASSET, "statusOfValueForKey:error:", [](id, SEL, id, u64* err) -> s64 {
        if (err) *err = 0;
        return 2;  // loaded
    });

    Class ITEM = objc::host_class("AVPlayerItem");
    class_method(ITEM, "playerItemWithAsset:", [](Class c, SEL, id asset) {
        id i = objc::alloc(c);
        objc::ensure<ItemData>(i).asset = objc::retain(asset);
        return objc::autorelease(i);
    });
    class_method(ITEM, "playerItemWithURL:", [](Class c, SEL, id url) {
        id i = objc::alloc(c);
        id asset = objc::send(objc::class_named("AVURLAsset"), "URLAssetWithURL:options:", {url, 0});
        objc::ensure<ItemData>(i).asset = objc::retain(asset);
        return objc::autorelease(i);
    });
    method(ITEM, "asset", [](id self, SEL) { return objc::ensure<ItemData>(self).asset; });
    objc::add_method(ITEM, "duration", [](cpu::Thread& t) {
        auto m = movie_of_item(t.x(0));
        ret_cmtime(t, cm_seconds(m ? video::duration(m) : kFakeMovieSeconds));
    });
    objc::add_method(ITEM, "currentTime", [](cpu::Thread& t) {
        auto m = movie_of_item(t.x(0));
        ret_cmtime(t, cm_seconds(m ? video::time(m) : kFakeMovieSeconds));
    });
    method(ITEM, "status", [](id, SEL) -> s64 { return 1; });  // ReadyToPlay
    method(ITEM, "isPlaybackLikelyToKeepUp", [](id, SEL) { return true; });
    // seekToTime: takes a CMTime (24 bytes) by reference; only rewinding (looping movies) is used.
    method(ITEM, "seekToTime:", [](id self, SEL, const CMTime* t) {
        if (auto m = movie_of_item(self); m && (!t || t->value == 0)) video::restart(m);
    });
    // FMovieHelper waits for KVO on "status" to become ReadyToPlay before calling -play.
    method(ITEM, "addObserver:forKeyPath:options:context:", [](id self, SEL, id observer, id key_path, u64, u64 context) {
        if (utf8(key_path) != "status") return;
        objc::retain(self);
        objc::retain(observer);
        objc::retain(key_path);
        post_to_main([self, observer, key_path, context] {
            id change = dict({{str("NSKeyValueChangeNewKey"), number_int(1)}});
            objc::send(observer, "observeValueForKeyPath:ofObject:change:context:", {key_path, self, change, context});
            objc::release(key_path);
            objc::release(observer);
            objc::release(self);
        });
    });
    method(ITEM, "removeObserver:forKeyPath:", [](id, SEL, id, id) {});

    Class PLAYER = objc::host_class("AVPlayer");
    class_method(PLAYER, "playerWithPlayerItem:", [](Class c, SEL, id item) {
        id p = objc::alloc(c);
        objc::ensure<PlayerData>(p).item = objc::retain(item);
        return objc::autorelease(p);
    });
    method(PLAYER, "initWithPlayerItem:", [](id self, SEL, id item) {
        objc::ensure<PlayerData>(self).item = objc::retain(item);
        return self;
    });
    method(PLAYER, "currentItem", [](id self, SEL) { return objc::ensure<PlayerData>(self).item; });
    method(PLAYER, "play", [](id self, SEL) {
        auto& d = objc::ensure<PlayerData>(self);
        d.rate = 1;
        id item = d.item;
        auto post_end = [item] {
            objc::retain(item);
            post_to_main([item] {
                id center = objc::send(objc::class_named("NSNotificationCenter"), "defaultCenter");
                objc::send(center, "postNotificationName:object:", {str("AVPlayerItemDidPlayToEndTimeNotification"), item});
                objc::release(item);
            });
        };
        if (auto m = movie_of_item(item)) {
            video::set_on_finished(m, post_end);
            video::play(m);
            return;
        }
        // No decodable movie: report the end right away.
        auto* tm = new Timer;
        tm->fire_at = now_ref() + kFakeMovieSeconds;
        tm->fn = post_end;
        RunLoop::main().add_timer(tm);
    });
    method(PLAYER, "pause", [](id self, SEL) {
        auto& d = objc::ensure<PlayerData>(self);
        d.rate = 0;
        if (auto m = movie_of_item(d.item)) video::pause(m);
    });
    method(PLAYER, "rate", [](id self, SEL) { return (float)objc::ensure<PlayerData>(self).rate; });
    method(PLAYER, "setRate:", [](id self, SEL, float r) {
        objc::ensure<PlayerData>(self).rate = r;
        if (r == 0) objc::send(self, "pause");
        else objc::send(self, "play");
    });
    objc::add_method(PLAYER, "currentTime", [](cpu::Thread& t) {
        auto m = movie_of_item(objc::ensure<PlayerData>(t.x(0)).item);
        ret_cmtime(t, cm_seconds(m ? video::time(m) : kFakeMovieSeconds));
    });
    method(PLAYER, "seekToTime:", [](id self, SEL, const CMTime* t) {
        if (auto m = movie_of_item(objc::ensure<PlayerData>(self).item); m && (!t || t->value == 0)) video::restart(m);
    });
    method(PLAYER, "setActionAtItemEnd:", [](id, SEL, s64) {});
    method(PLAYER, "setVolume:", [](id, SEL, float) {});
    method(PLAYER, "setMuted:", [](id, SEL, bool) {});
    method(PLAYER, "replaceCurrentItemWithPlayerItem:", [](id self, SEL, id item) { objc::ensure<PlayerData>(self).item = objc::retain(item); });
    method(PLAYER, "addPeriodicTimeObserverForInterval:queue:usingBlock:", [](id, SEL) { return objc::autorelease(objc::alloc(objc::class_named("NSObject"))); });
    method(PLAYER, "removeTimeObserver:", [](id, SEL, id) {});
    method(PLAYER, "status", [](id, SEL) -> s64 { return 1; });

    Class PL = objc::host_class("AVPlayerLayer", "CALayer");
    class_method(PL, "playerLayerWithPlayer:", [](Class c, SEL, id) { return objc::autorelease(objc::send(objc::alloc(c), "init")); });
    method(PL, "setPlayer:", [](id, SEL, id) {});
    method(PL, "setVideoGravity:", [](id, SEL, id) {});
    method(PL, "isReadyForDisplay", [](id, SEL) { return true; });

    // ---------------- AudioToolbox (silent for now) ----------------
    fn("_AudioSessionInitialize", [](u64, u64, u64, u64) { return 0; });
    fn("_AudioSessionAddPropertyListener", [](u32, u64, u64) { return 0; });
    fn("_AudioSessionGetProperty", [](u32 id_, u32* size, void* data) -> s32 {
        switch (id_) {
        case 0x6f746872:  // 'othr' other audio is playing
        case 0x63686e67:
            *static_cast<u32*>(data) = 0;
            return 0;
        case 0x68777372:  // 'hwsr' hardware sample rate
            *static_cast<double*>(data) = 44100.0;
            return 0;
        case 0x6f636867:  // 'ochg' output channels
            *static_cast<u32*>(data) = 2;
            return 0;
        case 0x63627264:  // 'cbrd' current buffer duration
            *static_cast<float*>(data) = 0.023f;
            return 0;
        default:
            LOG_DEBUG("AudioSessionGetProperty('%c%c%c%c')", (id_ >> 24) & 0xff, (id_ >> 16) & 0xff, (id_ >> 8) & 0xff, id_ & 0xff);
            if (size && *size >= 4) std::memset(data, 0, *size);
            return 0;
        }
    });
    fn("_NewAUGraph", [](u64* out) {
        *out = gaddr(new Graph);
        return 0;
    });
    fn("_AUGraphAddNode", [](Graph* g, const u32* desc, s32* node) {
        *node = g->next_node++;
        auto fourcc = [](u32 v) { return std::string{(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v}; };
        LOG_INFO("AUGraphAddNode %d: type '%s' subtype '%s'", *node, fourcc(desc[0]).c_str(), fourcc(desc[1]).c_str());
        return 0;
    });
    fn("_AUGraphNodeInfo", [](Graph*, s32 node, void* desc, u64* unit) {
        if (unit) *unit = 0x1000 + node;
        return 0;
    });
    fn("_AUGraphSetNodeInputCallback", [](Graph* g, s32, u32 bus, const u64* cb) {
        g->callbacks.push_back({bus, cb ? cb[0] : 0});
        if (cb) mixer_set_callback(bus, cb[0], cb[1]);
        return 0;
    });
    for (const char* s : {"_AUGraphOpen", "_AUGraphInitialize", "_AUGraphConnectNodeInput", "_CAShow"}) fn(s, []() { return 0; });
    fn("_AUGraphStart", [](Graph* g) {
        if (g) g->running = true;
        LOG_INFO("AUGraphStart (%zu mixer inputs)", g ? g->callbacks.size() : 0);
        mixer_set_running(true);
        return 0;
    });
    fn("_AUGraphStop", [](Graph* g) {
        if (g) g->running = false;
        mixer_set_running(false);
        return 0;
    });
    fn("_AudioUnitSetProperty", [](u64 unit, u32 prop, u32 scope, u32 elem, const void* data, u32 size) {
        if (prop == 8 && size >= 40 && scope == 1) {  // StreamFormat on a mixer input
            double rate;
            std::memcpy(&rate, data, 8);
            const u32* f = reinterpret_cast<const u32*>(static_cast<const u8*>(data) + 8);
            LOG_DEBUG("mixer input %u format: %.0f Hz, flags 0x%x, %u ch, %u bits", elem, rate, f[1], f[5], f[6]);
            mixer_set_input_rate(elem, rate);
        } else if (prop == 2 && size == 8) {  // SampleRate
            double rate;
            std::memcpy(&rate, data, 8);
            LOG_INFO("audio unit 0x%llx sample rate (scope %u) = %.0f", (unsigned long long)unit, scope, rate);
            if (scope == 1) mixer_set_input_rate(elem, rate);
        }
        return 0;
    });
    fn("_AudioUnitSetParameter", [](u64 unit, u32 param, u32 scope, u32 elem, float value, u32) {
        if (scope == 1) mixer_set_param(elem, param, value);  // 3D mixer input bus parameters
        return 0;
    });
}

}  // namespace audio

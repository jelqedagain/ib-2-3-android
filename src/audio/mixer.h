// Audio output (WASAPI) with an emulation of Apple's 3D Mixer audio unit and AVAudioPlayer music.
#pragma once
#include "common.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace audio {

constexpr int kMaxBuses = 64;

// 3D Mixer (kAudioUnitSubType_AU3DMixerEmbedded) input buses: the guest supplies PCM through
// render callbacks; parameters use Apple's IDs (0 azimuth, 2 distance, 3 gain dB, 4 rate, 5 enable).
void mixer_set_callback(u32 bus, GuestAddr proc, GuestAddr refcon);
void mixer_set_param(u32 bus, u32 param, float value);
void mixer_set_running(bool running);
void mixer_set_input_rate(u32 bus, double hz);

// Decoded music track played by AVAudioPlayer.
struct Music;
std::shared_ptr<Music> music_load(const std::string& host_path);
std::shared_ptr<Music> music_from_pcm(std::vector<s16> pcm, int channels, int rate);
void music_play(const std::shared_ptr<Music>& m, double start_at_ref /* 0 = now */);
void music_pause(const std::shared_ptr<Music>& m);
void music_stop(const std::shared_ptr<Music>& m);
bool music_playing(const std::shared_ptr<Music>& m);
double music_duration(const std::shared_ptr<Music>& m);
double music_time(const std::shared_ptr<Music>& m);
void music_set_time(const std::shared_ptr<Music>& m, double seconds);
void music_set_volume(const std::shared_ptr<Music>& m, float v);
float music_volume(const std::shared_ptr<Music>& m);
void music_set_pan(const std::shared_ptr<Music>& m, float pan);
void music_set_loops(const std::shared_ptr<Music>& m, s64 loops);
s64 music_loops(const std::shared_ptr<Music>& m);
// Called (on the audio thread) when a track finishes naturally; `user` is the value passed to music_set_owner.
void music_set_owner(const std::shared_ptr<Music>& m, u64 user);
extern std::function<void(u64 user)> g_on_music_finished;

// If set, audio is written to this WAV file instead of the speakers (test runs).
extern std::string g_wav_path;

// Master volume for everything (0..1).
void set_master_volume(float v);
// User volume settings (0..1): music tracks, and the game's sound effects and speech.
void set_volumes(float music, float effects);

}  // namespace audio

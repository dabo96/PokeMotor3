#include "AudioManager.h"
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <cmath>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace audio {

// ─────────────────────────────────────────────────────────────────────────────
// MusicTrack
// ─────────────────────────────────────────────────────────────────────────────

void AudioManager::MusicTrack::Close() {
    if (stream) {
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
    if (vorbis) {
        stb_vorbis_close(vorbis);
        vorbis = nullptr;
    }
    active = false;
    paused = false;
    reachedEnd = false;
    currentVolume = 1.0f;
    targetVolume = 1.0f;
    fadeSpeed = 0.0f;
    path.clear();
    channels = 0;
    sampleRate = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Singleton & lifecycle
// ─────────────────────────────────────────────────────────────────────────────

AudioManager& AudioManager::Instance() {
    static AudioManager instance;
    return instance;
}

void AudioManager::Init() {
    if (mInitialized) return;

    mDeviceID = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!mDeviceID) {
        SDL_Log("[Audio] Failed to open audio device: %s", SDL_GetError());
        return;
    }

    mInitialized = true;
    SDL_Log("[Audio] AudioManager initialized (device ID: %u)", mDeviceID);
}

void AudioManager::Shutdown() {
    if (!mInitialized) return;

    StopAll();
    StopMusic(0.0f);
    mClips.clear();

    if (mDeviceID) {
        SDL_CloseAudioDevice(mDeviceID);
        mDeviceID = 0;
    }

    mInitialized = false;
    SDL_Log("[Audio] AudioManager shut down");
}

// ─────────────────────────────────────────────────────────────────────────────
// Clip management
// ─────────────────────────────────────────────────────────────────────────────

bool AudioManager::LoadClip(const std::string& name, const std::string& path) {
    if (mClips.count(name)) return true;

    auto clip = std::make_unique<AudioClip>();
    if (!clip->Load(path)) {
        SDL_Log("[Audio] Failed to load clip '%s' from %s", name.c_str(), path.c_str());
        return false;
    }

    SDL_Log("[Audio] Loaded clip '%s' (%u bytes)", name.c_str(), clip->GetDataLen());
    mClips[name] = std::move(clip);
    return true;
}

AudioClip* AudioManager::GetClip(const std::string& name) {
    auto it = mClips.find(name);
    return it != mClips.end() ? it->second.get() : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// SFX playback
// ─────────────────────────────────────────────────────────────────────────────

SoundHandle AudioManager::Play(const std::string& clipName, float volume, bool loop) {
    if (!mInitialized) return INVALID_SOUND;

    auto* clip = GetClip(clipName);
    if (!clip) {
        SDL_Log("[Audio] Clip '%s' not found", clipName.c_str());
        return INVALID_SOUND;
    }

    EnforceSFXLimit();

    SDL_AudioStream* stream = SDL_CreateAudioStream(&clip->GetSpec(), nullptr);
    if (!stream) {
        SDL_Log("[Audio] Failed to create audio stream: %s", SDL_GetError());
        return INVALID_SOUND;
    }

    if (!SDL_BindAudioStream(mDeviceID, stream)) {
        SDL_DestroyAudioStream(stream);
        return INVALID_SOUND;
    }

    if (!SDL_PutAudioStreamData(stream, clip->GetData(), clip->GetDataLen())) {
        SDL_DestroyAudioStream(stream);
        return INVALID_SOUND;
    }

    SDL_SetAudioStreamGain(stream, volume * mSFXVolume * mMasterVolume);

    SoundHandle handle = mNextHandle++;
    mActiveSounds.push_back({handle, stream, clipName, volume, loop, false});
    return handle;
}

void AudioManager::Stop(SoundHandle handle) {
    for (auto& sound : mActiveSounds) {
        if (sound.handle == handle) {
            if (sound.stream) {
                SDL_DestroyAudioStream(sound.stream);
                sound.stream = nullptr;
            }
            sound.finished = true;
            break;
        }
    }
}

void AudioManager::StopAll() {
    for (auto& sound : mActiveSounds) {
        if (sound.stream) {
            SDL_DestroyAudioStream(sound.stream);
            sound.stream = nullptr;
        }
    }
    mActiveSounds.clear();
}

void AudioManager::EnforceSFXLimit() {
    while (static_cast<int>(mActiveSounds.size()) >= mMaxSFXChannels) {
        // Evict oldest non-looping sound first
        auto it = std::find_if(mActiveSounds.begin(), mActiveSounds.end(),
            [](const ActiveSound& s) { return !s.loop && !s.finished; });
        if (it == mActiveSounds.end())
            it = mActiveSounds.begin();
        if (it->stream)
            SDL_DestroyAudioStream(it->stream);
        mActiveSounds.erase(it);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Music playback (OGG streaming)
// ─────────────────────────────────────────────────────────────────────────────

bool AudioManager::StartMusicTrack(MusicTrack& track, const std::string& path) {
    track.Close();

    int error = 0;
    track.vorbis = stb_vorbis_open_filename(path.c_str(), &error, nullptr);
    if (!track.vorbis) {
        SDL_Log("[Audio] Failed to open OGG '%s' (error %d)", path.c_str(), error);
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info(track.vorbis);
    track.channels = info.channels;
    track.sampleRate = info.sample_rate;
    track.path = path;

    SDL_AudioSpec srcSpec{};
    srcSpec.format = SDL_AUDIO_S16;
    srcSpec.channels = track.channels;
    srcSpec.freq = track.sampleRate;

    track.stream = SDL_CreateAudioStream(&srcSpec, nullptr);
    if (!track.stream) {
        SDL_Log("[Audio] Failed to create music stream: %s", SDL_GetError());
        track.Close();
        return false;
    }

    if (!SDL_BindAudioStream(mDeviceID, track.stream)) {
        SDL_Log("[Audio] Failed to bind music stream: %s", SDL_GetError());
        track.Close();
        return false;
    }

    track.active = true;
    track.looping = true;
    track.reachedEnd = false;

    // Pre-fill buffer
    for (int i = 0; i < 4; ++i)
        StreamMusicChunk(track);

    SDL_Log("[Audio] Music started: %s (%d ch, %d Hz)", path.c_str(), track.channels, track.sampleRate);
    return true;
}

void AudioManager::StopMusicTrack(MusicTrack& track) {
    track.Close();
}

void AudioManager::StreamMusicChunk(MusicTrack& track) {
    if (!track.active || !track.vorbis || !track.stream || track.paused) return;

    short buffer[STREAM_CHUNK_SAMPLES * 2]; // max stereo
    int maxSamples = STREAM_CHUNK_SAMPLES;
    int got = stb_vorbis_get_samples_short_interleaved(
        track.vorbis, track.channels, buffer, maxSamples * track.channels);

    if (got > 0) {
        SDL_PutAudioStreamData(track.stream, buffer,
            got * track.channels * static_cast<int>(sizeof(short)));
    }

    if (got < maxSamples) {
        if (track.looping) {
            stb_vorbis_seek_start(track.vorbis);
        } else {
            track.reachedEnd = true;
        }
    }
}

void AudioManager::PlayMusic(const std::string& path, float fadeInSec) {
    if (!mInitialized) return;

    auto& track = mMusicTracks[mActiveMusicIndex];

    // Already playing this path
    if (track.active && track.path == path) return;

    // Stop any crossfade remnant in the other slot
    mMusicTracks[1 - mActiveMusicIndex].Close();

    StopMusicTrack(track);
    if (!StartMusicTrack(track, path)) return;

    if (fadeInSec > 0.0f) {
        track.currentVolume = 0.0f;
        track.targetVolume = 1.0f;
        track.fadeSpeed = 1.0f / fadeInSec;
    } else {
        track.currentVolume = 1.0f;
        track.targetVolume = 1.0f;
        track.fadeSpeed = 0.0f;
    }

    ApplyMusicGain(track);
}

void AudioManager::StopMusic(float fadeOutSec) {
    for (auto& track : mMusicTracks) {
        if (!track.active) continue;
        if (fadeOutSec > 0.0f) {
            track.targetVolume = 0.0f;
            track.fadeSpeed = 1.0f / fadeOutSec;
        } else {
            track.Close();
        }
    }
}

void AudioManager::CrossfadeTo(const std::string& path, float durationSec) {
    if (!mInitialized) return;

    if (durationSec <= 0.0f) {
        PlayMusic(path, 0.0f);
        return;
    }

    auto& current = mMusicTracks[mActiveMusicIndex];

    // Same track already playing
    if (current.active && current.path == path) return;

    // Fade out current
    if (current.active) {
        current.targetVolume = 0.0f;
        current.fadeSpeed = 1.0f / durationSec;
    }

    // Start new track in the other slot
    int newIdx = 1 - mActiveMusicIndex;
    auto& newTrack = mMusicTracks[newIdx];
    newTrack.Close();

    if (!StartMusicTrack(newTrack, path)) return;

    newTrack.currentVolume = 0.0f;
    newTrack.targetVolume = 1.0f;
    newTrack.fadeSpeed = 1.0f / durationSec;
    ApplyMusicGain(newTrack);

    mActiveMusicIndex = newIdx;
}

void AudioManager::PauseMusic() {
    for (auto& track : mMusicTracks) {
        if (track.active && track.stream && !track.paused) {
            SDL_UnbindAudioStream(track.stream);
            track.paused = true;
        }
    }
}

void AudioManager::ResumeMusic() {
    for (auto& track : mMusicTracks) {
        if (track.active && track.stream && track.paused) {
            SDL_BindAudioStream(mDeviceID, track.stream);
            track.paused = false;
        }
    }
}

bool AudioManager::IsMusicPlaying() const {
    for (auto& track : mMusicTracks) {
        if (track.active && !track.paused) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Volume control
// ─────────────────────────────────────────────────────────────────────────────

void AudioManager::SetVolume(SoundHandle handle, float volume) {
    for (auto& sound : mActiveSounds) {
        if (sound.handle == handle && sound.stream) {
            sound.volume = volume;
            SDL_SetAudioStreamGain(sound.stream, volume * mSFXVolume * mMasterVolume);
            break;
        }
    }
}

void AudioManager::SetMasterVolume(float volume) {
    mMasterVolume = volume;
    for (auto& sound : mActiveSounds) {
        if (sound.stream)
            SDL_SetAudioStreamGain(sound.stream, sound.volume * mSFXVolume * mMasterVolume);
    }
    for (auto& track : mMusicTracks) {
        if (track.active) ApplyMusicGain(track);
    }
}

void AudioManager::SetMusicVolume(float volume) {
    mMusicVolume = volume;
    for (auto& track : mMusicTracks) {
        if (track.active) ApplyMusicGain(track);
    }
}

void AudioManager::SetSFXVolume(float volume) {
    mSFXVolume = volume;
    for (auto& sound : mActiveSounds) {
        if (sound.stream)
            SDL_SetAudioStreamGain(sound.stream, sound.volume * mSFXVolume * mMasterVolume);
    }
}

void AudioManager::ApplyMusicGain(MusicTrack& track) {
    if (track.stream)
        SDL_SetAudioStreamGain(track.stream, track.currentVolume * mMusicVolume * mMasterVolume);
}

// ─────────────────────────────────────────────────────────────────────────────
// Update
// ─────────────────────────────────────────────────────────────────────────────

void AudioManager::UpdateMusicFade(MusicTrack& track, float dt) {
    if (track.fadeSpeed <= 0.0f) return;

    if (track.currentVolume < track.targetVolume) {
        track.currentVolume = std::min(track.currentVolume + track.fadeSpeed * dt, track.targetVolume);
    } else if (track.currentVolume > track.targetVolume) {
        track.currentVolume = std::max(track.currentVolume - track.fadeSpeed * dt, track.targetVolume);
    }

    if (track.currentVolume == track.targetVolume)
        track.fadeSpeed = 0.0f;

    ApplyMusicGain(track);
}

void AudioManager::Update(float deltaTime) {
    if (!mInitialized) return;

    // --- Music ---
    for (int i = 0; i < 2; ++i) {
        auto& track = mMusicTracks[i];
        if (!track.active) continue;

        UpdateMusicFade(track, deltaTime);

        // Fade-out complete → close
        if (track.targetVolume <= 0.0f && track.currentVolume <= 0.0f) {
            track.Close();
            continue;
        }

        // Stream more data if needed
        if (!track.paused && track.vorbis) {
            int queued = SDL_GetAudioStreamQueued(track.stream);
            int threshold = track.sampleRate * track.channels * static_cast<int>(sizeof(short));
            while (queued < threshold && !track.reachedEnd) {
                int before = queued;
                StreamMusicChunk(track);
                queued = SDL_GetAudioStreamQueued(track.stream);
                if (queued == before) break;
            }
        }

        // Non-looping track finished
        if (track.reachedEnd) {
            int queued = SDL_GetAudioStreamQueued(track.stream);
            if (queued <= 0)
                track.Close();
        }
    }

    // --- SFX ---
    for (auto& sound : mActiveSounds) {
        if (sound.finished || !sound.stream) continue;

        int queued = SDL_GetAudioStreamQueued(sound.stream);
        if (queued <= 0) {
            if (sound.loop) {
                auto* clip = GetClip(sound.clipName);
                if (clip)
                    SDL_PutAudioStreamData(sound.stream, clip->GetData(), clip->GetDataLen());
            } else {
                SDL_DestroyAudioStream(sound.stream);
                sound.stream = nullptr;
                sound.finished = true;
            }
        }
    }

    mActiveSounds.erase(
        std::remove_if(mActiveSounds.begin(), mActiveSounds.end(),
            [](const ActiveSound& s) { return s.finished; }),
        mActiveSounds.end());
}

} // namespace audio

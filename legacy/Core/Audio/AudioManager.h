#pragma once
#include "AudioClip.h"
#include <SDL3/SDL_audio.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

struct stb_vorbis;

namespace audio {

using SoundHandle = uint32_t;
constexpr SoundHandle INVALID_SOUND = 0;

class AudioManager {
public:
    static AudioManager& Instance();

    void Init();
    void Shutdown();

    // Clip management — loads and caches WAV/OGG files (full decode for SFX)
    bool LoadClip(const std::string& name, const std::string& path);
    AudioClip* GetClip(const std::string& name);

    // SFX playback
    SoundHandle Play(const std::string& clipName, float volume = 1.0f, bool loop = false);
    void Stop(SoundHandle handle);
    void StopAll();

    // Music playback (OGG streaming from disk)
    void PlayMusic(const std::string& path, float fadeInSec = 0.0f);
    void StopMusic(float fadeOutSec = 0.0f);
    void CrossfadeTo(const std::string& path, float durationSec = 1.0f);
    void PauseMusic();
    void ResumeMusic();
    bool IsMusicPlaying() const;

    // Volume control
    void SetVolume(SoundHandle handle, float volume);
    void SetMasterVolume(float volume);
    float GetMasterVolume() const { return mMasterVolume; }
    void SetMusicVolume(float volume);
    float GetMusicVolume() const { return mMusicVolume; }
    void SetSFXVolume(float volume);
    float GetSFXVolume() const { return mSFXVolume; }

    // SFX channel limit
    void SetMaxSFXChannels(int max) { mMaxSFXChannels = max; }
    int GetMaxSFXChannels() const { return mMaxSFXChannels; }

    // Per-frame update (handles streaming, fading, cleanup)
    void Update(float deltaTime);

    bool IsInitialized() const { return mInitialized; }

private:
    AudioManager() = default;
    ~AudioManager() = default;

    struct ActiveSound {
        SoundHandle handle = INVALID_SOUND;
        SDL_AudioStream* stream = nullptr;
        std::string clipName;
        float volume = 1.0f;
        bool loop = false;
        bool finished = false;
    };

    struct MusicTrack {
        stb_vorbis* vorbis = nullptr;
        SDL_AudioStream* stream = nullptr;
        float currentVolume = 1.0f;
        float targetVolume = 1.0f;
        float fadeSpeed = 0.0f;        // volume units per second
        bool active = false;
        bool paused = false;
        bool looping = true;
        bool reachedEnd = false;        // stb_vorbis hit EOF
        std::string path;
        int channels = 0;
        int sampleRate = 0;

        void Close();
    };

    static constexpr int STREAM_CHUNK_SAMPLES = 4096;

    bool mInitialized = false;
    float mMasterVolume = 1.0f;
    float mMusicVolume = 1.0f;
    float mSFXVolume = 1.0f;
    SDL_AudioDeviceID mDeviceID = 0;
    SoundHandle mNextHandle = 1;
    int mMaxSFXChannels = 16;

    std::unordered_map<std::string, std::unique_ptr<AudioClip>> mClips;
    std::vector<ActiveSound> mActiveSounds;

    MusicTrack mMusicTracks[2];
    int mActiveMusicIndex = 0;

    bool StartMusicTrack(MusicTrack& track, const std::string& path);
    void StopMusicTrack(MusicTrack& track);
    void StreamMusicChunk(MusicTrack& track);
    void UpdateMusicFade(MusicTrack& track, float dt);
    void ApplyMusicGain(MusicTrack& track);
    void EnforceSFXLimit();
};

} // namespace audio

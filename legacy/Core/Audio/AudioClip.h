#pragma once
#include <SDL3/SDL_audio.h>
#include <string>
#include <cstdint>

namespace audio {

class AudioClip {
public:
    AudioClip() = default;
    ~AudioClip() { Unload(); }

    // Non-copyable, movable
    AudioClip(const AudioClip&) = delete;
    AudioClip& operator=(const AudioClip&) = delete;
    AudioClip(AudioClip&& other) noexcept { *this = std::move(other); }
    AudioClip& operator=(AudioClip&& other) noexcept {
        if (this != &other) {
            Unload();
            mData = other.mData;
            mDataLen = other.mDataLen;
            mSpec = other.mSpec;
            mPath = std::move(other.mPath);
            other.mData = nullptr;
            other.mDataLen = 0;
        }
        return *this;
    }

    // Loads WAV or OGG (detected by extension)
    bool Load(const std::string& path);

    void Unload();

    bool IsLoaded() const { return mData != nullptr; }
    const SDL_AudioSpec& GetSpec() const { return mSpec; }
    const uint8_t* GetData() const { return mData; }
    uint32_t GetDataLen() const { return mDataLen; }
    const std::string& GetPath() const { return mPath; }

private:
    uint8_t* mData = nullptr;
    uint32_t mDataLen = 0;
    SDL_AudioSpec mSpec{};
    std::string mPath;

    bool LoadWAV(const std::string& path);
    bool LoadOGG(const std::string& path);
};

} // namespace audio

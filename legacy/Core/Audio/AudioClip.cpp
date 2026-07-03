#include "AudioClip.h"
#include <SDL3/SDL_log.h>
#include <algorithm>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace audio {

bool AudioClip::Load(const std::string& path) {
    Unload();

    // Detect format by extension
    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    if (ext == ".ogg")
        return LoadOGG(path);
    else
        return LoadWAV(path);
}

void AudioClip::Unload() {
    if (mData) {
        SDL_free(mData);
        mData = nullptr;
        mDataLen = 0;
    }
}

bool AudioClip::LoadWAV(const std::string& path) {
    if (!SDL_LoadWAV(path.c_str(), &mSpec, &mData, &mDataLen)) {
        SDL_Log("Failed to load WAV: %s", path.c_str());
        return false;
    }
    mPath = path;
    return true;
}

bool AudioClip::LoadOGG(const std::string& path) {
    int channels = 0, sampleRate = 0;
    short* decoded = nullptr;
    int sampleCount = stb_vorbis_decode_filename(path.c_str(), &channels, &sampleRate, &decoded);

    if (sampleCount <= 0 || !decoded) {
        SDL_Log("Failed to decode OGG: %s", path.c_str());
        return false;
    }

    // Store as signed 16-bit PCM
    uint32_t dataLen = static_cast<uint32_t>(sampleCount * channels * sizeof(short));
    mData = static_cast<uint8_t*>(SDL_malloc(dataLen));
    if (!mData) {
        free(decoded);
        return false;
    }
    SDL_memcpy(mData, decoded, dataLen);
    mDataLen = dataLen;
    free(decoded);

    mSpec.format = SDL_AUDIO_S16;
    mSpec.channels = channels;
    mSpec.freq = sampleRate;

    mPath = path;
    return true;
}

} // namespace audio

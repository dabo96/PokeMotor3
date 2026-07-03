#include "ConfigManager.h"
#include <fstream>
#include <iostream>

namespace persistence {

ConfigManager& ConfigManager::Instance()
{
    static ConfigManager instance;
    return instance;
}

void ConfigManager::Init(const std::string& basePath)
{
    mConfigPath = std::filesystem::path(basePath) / "config.json";
    LoadFromDisk();
    std::cout << "[Config] Initialized: " << mConfigPath.string() << std::endl;
}

void ConfigManager::Save(const std::string& key, const json& value)
{
    mData[key] = value;
    SaveToDisk();
}

json ConfigManager::Load(const std::string& key, const json& defaultValue)
{
    auto it = mData.find(key);
    if (it != mData.end()) {
        return *it;
    }
    return defaultValue;
}

bool ConfigManager::Has(const std::string& key) const
{
    return mData.contains(key);
}

void ConfigManager::Remove(const std::string& key)
{
    mData.erase(key);
    SaveToDisk();
}

void ConfigManager::LoadFromDisk()
{
    try {
        std::ifstream file(mConfigPath);
        if (file.is_open()) {
            mData = json::parse(file);
            std::cout << "[Config] Loaded " << mData.size() << " keys" << std::endl;
        } else {
            mData = json::object();
        }
    } catch (const std::exception& e) {
        std::cerr << "[Config] Load error: " << e.what() << std::endl;
        mData = json::object();
    }
    mLoaded = true;
}

void ConfigManager::SaveToDisk()
{
    try {
        std::filesystem::create_directories(mConfigPath.parent_path());

        std::ofstream file(mConfigPath);
        if (file.is_open()) {
            file << mData.dump(2);
        } else {
            std::cerr << "[Config] Failed to write: " << mConfigPath.string() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Config] Save error: " << e.what() << std::endl;
    }
}

} // namespace persistence

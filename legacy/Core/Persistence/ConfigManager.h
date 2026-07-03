#pragma once

#include <string>
#include <filesystem>
#include <json.hpp>

using json = nlohmann::json;

namespace persistence {

class ConfigManager {
public:
    static ConfigManager& Instance();

    // Must be called before use — sets path for config.json
    void Init(const std::string& basePath);

    // Save a key-value pair (persists immediately)
    void Save(const std::string& key, const json& value);

    // Load a value by key, returns defaultValue if not found
    json Load(const std::string& key, const json& defaultValue = nullptr);

    // Check if a key exists
    bool Has(const std::string& key) const;

    // Remove a key
    void Remove(const std::string& key);

private:
    ConfigManager() = default;
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    void LoadFromDisk();
    void SaveToDisk();

    json mData = json::object();
    std::filesystem::path mConfigPath;
    bool mLoaded = false;
};

} // namespace persistence

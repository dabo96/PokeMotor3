#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <json.hpp>

using json = nlohmann::json;

namespace persistence {

class FileSystemManager {
public:
    static FileSystemManager& Instance();

    // Base directory for all save data (next to executable)
    void SetBasePath(const std::string& basePath);

    // Generic file save/load (path relative to base)
    bool Save(const std::string& relativePath, const json& data);
    json Load(const std::string& relativePath);

    // Save slots (stored in Saves/<slot>/)
    bool SaveSlot(int slot, const json& data);
    json LoadSlot(int slot);
    bool SlotExists(int slot) const;
    bool DeleteSlot(int slot);
    std::vector<int> ListSlots() const;

    // Utility
    bool Exists(const std::string& relativePath) const;
    bool Delete(const std::string& relativePath);

private:
    FileSystemManager() = default;
    ~FileSystemManager() = default;
    FileSystemManager(const FileSystemManager&) = delete;
    FileSystemManager& operator=(const FileSystemManager&) = delete;

    std::filesystem::path ResolvePath(const std::string& relativePath) const;
    std::filesystem::path SlotPath(int slot) const;

    std::filesystem::path mBasePath;
};

} // namespace persistence

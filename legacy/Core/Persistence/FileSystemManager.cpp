#include "FileSystemManager.h"
#include <fstream>
#include <iostream>

namespace persistence {

FileSystemManager& FileSystemManager::Instance()
{
    static FileSystemManager instance;
    return instance;
}

void FileSystemManager::SetBasePath(const std::string& basePath)
{
    mBasePath = basePath;
    std::cout << "[FileSystem] Base path: " << mBasePath.string() << std::endl;
}

// ── Generic save/load ────────────────────────────────────────────────────────

bool FileSystemManager::Save(const std::string& relativePath, const json& data)
{
    auto fullPath = ResolvePath(relativePath);

    try {
        std::filesystem::create_directories(fullPath.parent_path());

        std::ofstream file(fullPath);
        if (!file.is_open()) {
            std::cerr << "[FileSystem] Failed to open for writing: " << fullPath.string() << std::endl;
            return false;
        }

        file << data.dump(2);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[FileSystem] Save error: " << e.what() << std::endl;
        return false;
    }
}

json FileSystemManager::Load(const std::string& relativePath)
{
    auto fullPath = ResolvePath(relativePath);

    try {
        std::ifstream file(fullPath);
        if (!file.is_open()) {
            return nullptr;
        }

        return json::parse(file);
    } catch (const std::exception& e) {
        std::cerr << "[FileSystem] Load error: " << e.what() << std::endl;
        return nullptr;
    }
}

// ── Save slots ───────────────────────────────────────────────────────────────

bool FileSystemManager::SaveSlot(int slot, const json& data)
{
    auto path = SlotPath(slot) / "save.json";

    try {
        std::filesystem::create_directories(path.parent_path());

        std::ofstream file(path);
        if (!file.is_open()) {
            std::cerr << "[FileSystem] Failed to open slot " << slot << " for writing" << std::endl;
            return false;
        }

        file << data.dump(2);
        std::cout << "[FileSystem] Saved slot " << slot << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[FileSystem] SaveSlot error: " << e.what() << std::endl;
        return false;
    }
}

json FileSystemManager::LoadSlot(int slot)
{
    auto path = SlotPath(slot) / "save.json";

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return nullptr;
        }

        return json::parse(file);
    } catch (const std::exception& e) {
        std::cerr << "[FileSystem] LoadSlot error: " << e.what() << std::endl;
        return nullptr;
    }
}

bool FileSystemManager::SlotExists(int slot) const
{
    return std::filesystem::exists(SlotPath(slot) / "save.json");
}

bool FileSystemManager::DeleteSlot(int slot)
{
    try {
        auto path = SlotPath(slot);
        if (std::filesystem::exists(path)) {
            std::filesystem::remove_all(path);
            std::cout << "[FileSystem] Deleted slot " << slot << std::endl;
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[FileSystem] DeleteSlot error: " << e.what() << std::endl;
        return false;
    }
}

std::vector<int> FileSystemManager::ListSlots() const
{
    std::vector<int> slots;
    auto savesDir = mBasePath / "Saves";

    if (!std::filesystem::exists(savesDir)) return slots;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(savesDir)) {
            if (!entry.is_directory()) continue;

            auto name = entry.path().filename().string();
            // Slot directories are named "Slot_1", "Slot_2", etc.
            if (name.rfind("Slot_", 0) == 0) {
                try {
                    int slot = std::stoi(name.substr(5));
                    if (std::filesystem::exists(entry.path() / "save.json")) {
                        slots.push_back(slot);
                    }
                } catch (...) {}
            }
        }
    } catch (...) {}

    std::sort(slots.begin(), slots.end());
    return slots;
}

// ── Utility ──────────────────────────────────────────────────────────────────

bool FileSystemManager::Exists(const std::string& relativePath) const
{
    return std::filesystem::exists(ResolvePath(relativePath));
}

bool FileSystemManager::Delete(const std::string& relativePath)
{
    try {
        auto fullPath = ResolvePath(relativePath);
        if (std::filesystem::exists(fullPath)) {
            std::filesystem::remove(fullPath);
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[FileSystem] Delete error: " << e.what() << std::endl;
        return false;
    }
}

// ── Private helpers ──────────────────────────────────────────────────────────

std::filesystem::path FileSystemManager::ResolvePath(const std::string& relativePath) const
{
    return mBasePath / relativePath;
}

std::filesystem::path FileSystemManager::SlotPath(int slot) const
{
    return mBasePath / "Saves" / ("Slot_" + std::to_string(slot));
}

} // namespace persistence

// Editor/AssetBrowser.cpp — escaneo recursivo de Assets/ con std::filesystem.
#include "Editor/AssetBrowser.h"

#include "Core/Log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace pk {

namespace fs = std::filesystem;

AssetKind AssetBrowser::kindForExtension(std::string ext) {
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" ||
        ext == "tga" || ext == "gif" || ext == "psd")                    return AssetKind::Image;
    if (ext == "gltf" || ext == "glb" || ext == "obj" || ext == "fbx" ||
        ext == "bin")                                                    return AssetKind::Model;
    if (ext == "json")                                                   return AssetKind::Data;
    if (ext == "lua")                                                    return AssetKind::Script;
    if (ext == "ttf" || ext == "otf")                                    return AssetKind::Font;
    if (ext == "wav" || ext == "ogg" || ext == "mp3" || ext == "flac")   return AssetKind::Audio;
    return AssetKind::Other;
}

void AssetBrowser::refresh(const std::string& absRoot, const std::string& relRoot) {
    m_root = AssetNode{};
    m_root.name     = relRoot;     // título mostrado ("Assets")
    m_root.path     = relRoot;     // ruta relativa raíz (para selección/drag-drop)
    m_root.kind     = AssetKind::Folder;
    m_root.isFolder = true;

    std::error_code ec;
    if (!fs::is_directory(absRoot, ec)) {
        LOG_WARN("AssetBrowser: no se encontró '%s'.", absRoot.c_str());
        return;
    }
    scan(absRoot, relRoot, m_root);
}

void AssetBrowser::scan(const std::string& absDir, const std::string& relDir, AssetNode& node) {
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (const auto& e : fs::directory_iterator(absDir, ec)) entries.push_back(e);
    if (ec) return;

    // Carpetas antes que archivos; alfabético (sin distinguir caja) dentro de cada grupo.
    std::sort(entries.begin(), entries.end(),
        [](const fs::directory_entry& a, const fs::directory_entry& b) {
            std::error_code e1, e2;
            const bool da = a.is_directory(e1), db = b.is_directory(e2);
            if (da != db) return da;
            std::string an = a.path().filename().string(), bn = b.path().filename().string();
            std::transform(an.begin(), an.end(), an.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::transform(bn.begin(), bn.end(), bn.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return an < bn;
        });

    for (const auto& e : entries) {
        AssetNode child;
        child.name = e.path().filename().string();
        child.path = (fs::path(relDir) / child.name).generic_string();   // RELATIVA (drag-drop)
        if (e.is_directory(ec)) {
            child.isFolder = true;
            child.kind     = AssetKind::Folder;
            // Escanea el directorio absoluto real, pero sigue almacenando rutas relativas.
            scan((fs::path(absDir) / child.name).generic_string(), child.path, child);
        } else {
            child.kind = kindForExtension(e.path().extension().string());
        }
        node.children.push_back(std::move(child));
    }
}

}  // namespace pk

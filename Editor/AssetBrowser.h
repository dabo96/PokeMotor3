// Editor/AssetBrowser.h — escanea la carpeta Assets/ en disco y la expone como un
// árbol (carpetas + archivos clasificados por tipo) para el navegador del editor.
// A diferencia del enfoque anterior (listar solo las texturas ya cargadas en
// memoria), esto refleja lo que REALMENTE hay en disco. Fase 1 del navegador de
// assets (ver asset-browser-plan).
#pragma once

#include <string>
#include <vector>

namespace pk {

// Categoría de un archivo, derivada de su extensión (decide el icono y, en fases
// siguientes, si se le genera miniatura).
enum class AssetKind { Folder, Image, Model, Data, Script, Font, Audio, Other };

struct AssetNode {
    std::string name;                  // nombre a mostrar (archivo o carpeta)
    std::string path;                  // ruta relativa al exe ("Assets/Models/Sprites/001.png")
    AssetKind   kind = AssetKind::Other;
    bool        isFolder = false;
    std::vector<AssetNode> children;   // contenido (solo en carpetas)
};

class AssetBrowser {
public:
    // (Re)escanea el directorio ABSOLUTO 'absRoot', pero almacena las rutas de los nodos
    // RELATIVAS bajo 'relRoot' (p.ej. "Assets/...") para que el drag-drop genere rutas
    // portables que resuelve Project (proyecto → fallback del motor). Reconstruye el árbol.
    void refresh(const std::string& absRoot, const std::string& relRoot = "Assets");
    const AssetNode& root() const { return m_root; }

    static AssetKind kindForExtension(std::string ext);   // ext con o sin punto, cualquier caja

private:
    void scan(const std::string& absDir, const std::string& relDir, AssetNode& node);
    AssetNode m_root;
};

}  // namespace pk

// Core/Project.cpp — implementación de la raíz de proyecto + resolución de rutas.
#include "Core/Project.h"

#include "Core/Log.h"

#include <SDL3/SDL.h>
#include <json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace pk {

namespace fs = std::filesystem;

Project& Project::instance() {
    static Project p;
    return p;
}

void Project::init() {
    if (m_inited) return;
    m_inited = true;

    // Raíz de fallback = assets que TRAE el motor. En Debug, el árbol de fuentes (permite
    // editar/hot-reload de scripts y datos del motor); en Release, junto al exe.
#ifdef POKEMOTOR_SOURCE_DIR
    m_engineAssets = POKEMOTOR_SOURCE_DIR;
#else
    if (const char* base = SDL_GetBasePath()) m_engineAssets = base;
#endif
    if (!m_engineAssets.empty())
        m_engineAssets = fs::path(m_engineAssets).lexically_normal().string();

    // Directorio del ejecutable: es donde el post-build deja los recursos de la APP
    // (shaders, atlas de fuentes). Se guarda siempre —también en Debug, donde el fallback
    // de contenido apunta al árbol de fuentes— para resolveAppFile.
    if (const char* base = SDL_GetBasePath())
        m_exeDir = fs::path(base).lexically_normal().string();

    // Config de USUARIO (recientes) en la carpeta de preferencias del SO (no es contenido
    // del proyecto y NO se hardcodea ninguna ruta de proyecto).
    if (const char* pref = SDL_GetPrefPath("PokeMotor", "PokeMotor")) m_prefDir = pref;
    loadRecents();

    // NO se abre ni crea ningún proyecto aquí: la raíz queda vacía hasta que la pantalla de
    // bienvenida llame a newProject/openProject. La demo carga por fallback al motor.
    LOG_INFO("Project: plantillas en '%s/Templates'; %zu recientes. Sin proyecto activo "
             "(esperando bienvenida).", m_engineAssets.c_str(), m_recents.size());
}

void Project::loadRecents() {
    m_recents.clear();
    if (m_prefDir.empty()) return;
    std::ifstream f(fs::path(m_prefDir) / "recents.json");
    if (!f) return;
    try {
        nlohmann::json j; f >> j;
        if (j.is_array())
            for (const auto& e : j)
                if (e.is_string()) m_recents.push_back(e.get<std::string>());
    } catch (...) {}
}

void Project::saveRecents() const {
    if (m_prefDir.empty()) return;
    nlohmann::json j = nlohmann::json::array();
    for (const auto& r : m_recents) j.push_back(r);
    std::ofstream f(fs::path(m_prefDir) / "recents.json");
    if (f) f << j.dump(2);
}

void Project::addRecent(const std::string& absPkprojPath) {
    if (absPkprojPath.empty()) return;
    const std::string p = fs::path(absPkprojPath).lexically_normal().string();
    m_recents.erase(std::remove(m_recents.begin(), m_recents.end(), p), m_recents.end());  // de-dup
    m_recents.insert(m_recents.begin(), p);                                                // más nuevo primero
    if (m_recents.size() > 8) m_recents.resize(8);
    saveRecents();
}

void Project::setRoot(const std::string& dir) {
    m_root = fs::path(dir).lexically_normal().string();
}

// Contenido del PROYECTO y solo del proyecto: sin proyecto abierto —o si el archivo no
// está— se devuelve la ruta tal cual y el que carga informa de que falta. El motor ya no
// presta sus assets: lo que puebla un proyecto son las plantillas, al crearlo.
std::string Project::resolveRead(const std::string& rel) const {
    if (rel.empty()) return rel;
    const fs::path p(rel);
    if (p.is_absolute()) return rel;
    if (!m_root.empty()) return (fs::path(m_root) / p).lexically_normal().string();
    return rel;
}

// Recurso de la aplicación (no contenido del proyecto): junto al exe primero, y si no
// está, bajo la raíz del motor. Devuelve absoluta para que resolveRead no la reescriba.
std::string Project::resolveAppFile(const std::string& rel) const {
    if (rel.empty()) return rel;
    const fs::path p(rel);
    if (p.is_absolute()) return rel;
    std::error_code ec;
    if (!m_exeDir.empty()) {
        const fs::path cand = fs::path(m_exeDir) / p;
        if (fs::exists(cand, ec)) return cand.lexically_normal().string();
    }
    if (!m_engineAssets.empty()) {
        const fs::path cand = fs::path(m_engineAssets) / p;
        if (fs::exists(cand, ec)) return cand.lexically_normal().string();
    }
    return rel;   // no encontrado: relativa al cwd (comportamiento previo)
}

std::string Project::resolveWrite(const std::string& rel) const {
    if (rel.empty()) return rel;
    const fs::path p(rel);
    if (p.is_absolute()) return rel;
    fs::path full = (m_root.empty() ? fs::path(m_engineAssets) : fs::path(m_root)) / p;
    full = full.lexically_normal();
    std::error_code ec;
    fs::create_directories(full.parent_path(), ec);   // asegura las carpetas padre
    return full.string();
}

// --- Fase 2: archivo de proyecto (.pkproj) ---

bool Project::newProject(const std::string& pkprojPath, const std::string& name,
                         const std::string& templateName) {
    const fs::path pk(pkprojPath);
    const fs::path dir = pk.parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    // Esqueleto de carpetas de contenido del proyecto.
    for (const char* sub : { "Assets/Data", "Assets/Scripts", "Assets/Textures", "Assets/Models/Sprites" })
        fs::create_directories(dir / sub, ec);

    setRoot(dir.string());
    m_name       = name.empty() ? pk.stem().string() : name;
    m_pkprojPath = pk.lexically_normal().string();
    m_startScene.clear();

    // Contenido inicial: Base (los .lua que el editor adjunta al crear jugador/cámara) y
    // encima la plantilla elegida. Si ésta trae una escena, queda como escena inicial.
    copyTemplate("Base");
    // "Empty" es la plantilla SIN carpeta: el proyecto se queda con el esqueleto + Base.
    if (!templateName.empty() && templateName != "Base" && templateName != "Empty" &&
        !copyTemplate(templateName))
        LOG_WARN("Proyecto: la plantilla '%s' no existe; se crea vacío.", templateName.c_str());
    if (fs::exists(fs::path(m_root) / "Assets/Data/scene.json", ec))
        m_startScene = "Assets/Data/scene.json";
    LOG_INFO("Proyecto: plantilla '%s' aplicada en %s", templateName.c_str(), m_root.c_str());

    if (!save()) return false;
    addRecent(m_pkprojPath);
    LOG_INFO("Proyecto creado: '%s' en %s", m_name.c_str(), m_root.c_str());
    return true;
}

bool Project::openProject(const std::string& pkprojPath) {
    std::ifstream f(pkprojPath);
    if (!f) { LOG_WARN("Project: no se pudo abrir '%s'", pkprojPath.c_str()); return false; }
    try {
        nlohmann::json j; f >> j;
        m_name       = j.value("name", fs::path(pkprojPath).stem().string());
        m_startScene = j.value("startScene", std::string{});
    } catch (const std::exception& e) {
        LOG_ERROR("Project: .pkproj inválido (%s)", e.what());
        return false;
    }
    setRoot(fs::path(pkprojPath).parent_path().string());
    m_pkprojPath = fs::path(pkprojPath).lexically_normal().string();
    addRecent(m_pkprojPath);
    // Fontanería mínima: los .lua que el editor adjunta al crear un jugador o una cámara
    // deben existir en TODO proyecto. Se copian si faltan (skip_existing nunca pisa los del
    // usuario); esto también repara los proyectos que antes los tomaban prestados del motor.
    copyTemplate("Base");
    LOG_INFO("Proyecto abierto: '%s' (%s)", m_name.c_str(), m_root.c_str());
    return true;
}

// Vuelca "Templates/<name>/" sobre la raíz del proyecto. skip_existing: una plantilla
// nunca pisa lo que el usuario ya tenga (y Base, que va primera, gana a las demás).
bool Project::copyTemplate(const std::string& name) const {
    if (m_root.empty() || name.empty()) return false;
    std::error_code ec;
    const fs::path src(resolveAppFile("Templates/" + name));
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) return false;
    fs::copy(src, fs::path(m_root),
             fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec) { LOG_WARN("Proyecto: fallo copiando la plantilla '%s' (%s)", name.c_str(), ec.message().c_str()); return false; }
    return true;
}

// Las plantillas son CARPETAS, no una lista en el código: añadir una es crear su carpeta.
std::vector<std::string> Project::templateNames() const {
    std::vector<std::string> out{ "Empty" };   // sintética: proyecto en blanco (solo Base)
    std::error_code ec;
    const fs::path dir(resolveAppFile("Templates"));
    if (!fs::exists(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory()) continue;
        const std::string n = e.path().filename().string();
        if (n == "Base") continue;      // se aplica siempre, no se elige
        out.push_back(n);
    }
    std::sort(out.begin() + 1, out.end());   // "Empty" siempre primera
    return out;
}

std::string Project::importAsset(const std::string& srcAbsPath, const std::string& destRelDir) const {
    if (m_root.empty() || srcAbsPath.empty()) return {};
    std::error_code ec;
    const fs::path src(srcAbsPath);
    // ¿Ya está dentro del proyecto? Devuelve su ruta relativa sin copiar.
    const fs::path    rel  = fs::relative(src, m_root, ec);
    const std::string rels = ec ? std::string{} : rel.generic_string();
    if (!rels.empty() && rels.rfind("..", 0) != 0) return rels;
    // Copia a <raíz>/<destRelDir>/<archivo>.
    const fs::path dstDir = fs::path(m_root) / destRelDir;
    fs::create_directories(dstDir, ec);
    const fs::path dst = dstDir / src.filename();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { LOG_WARN("Project: no se pudo importar '%s' (%s)", srcAbsPath.c_str(), ec.message().c_str()); return {}; }
    LOG_INFO("Proyecto: '%s' importado a %s", src.filename().string().c_str(), destRelDir.c_str());
    return (fs::path(destRelDir) / src.filename()).generic_string();
}

void Project::noteSceneSaved(const std::string& absScenePath) {
    if (m_root.empty() || absScenePath.empty()) return;
    std::error_code ec;
    const fs::path    rel  = fs::relative(absScenePath, m_root, ec);
    const std::string rels = ec ? std::string{} : rel.generic_string();
    // Bajo la raíz → ruta relativa (portable); fuera → absoluta (resolveRead la devuelve tal cual).
    const std::string s = (!rels.empty() && rels.rfind("..", 0) != 0) ? rels : absScenePath;
    if (s == m_startScene) return;   // sin cambios: no reescribir el .pkproj
    m_startScene = s;
    save();
}

bool Project::save() const {
    if (m_pkprojPath.empty()) return false;
    nlohmann::json j;
    j["name"] = m_name;
    if (!m_startScene.empty()) j["startScene"] = m_startScene;
    std::ofstream f(m_pkprojPath);
    if (!f) { LOG_WARN("Project: no se pudo escribir '%s'", m_pkprojPath.c_str()); return false; }
    f << j.dump(2);
    return true;
}

}  // namespace pk

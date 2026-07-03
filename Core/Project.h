// Core/Project.h — raíz del PROYECTO activo (modelo de motor real). Un proyecto es una
// carpeta con un archivo ".pkproj" en la raíz; todo el CONTENIDO (escenas, tilesets +
// su PNG, sprites, scripts de gameplay, datos como species/moves/animaciones) se guarda
// con rutas RELATIVAS y se resuelve contra esa raíz. Si el proyecto no tiene un asset,
// se cae a los assets que TRAE el motor (fallback), así la demo funciona en cualquier
// proyecto nuevo sin copiar nada a mano.
//
// Los recursos del MOTOR (shaders en "Shaders/SPV/", fuentes en "Assets/Fonts/") NO
// pasan por aquí: viven junto al exe y se resuelven por el cwd (ver anchorCwdToExe).
#pragma once

#include <string>
#include <vector>

namespace pk {

class Project {
public:
    static Project& instance();

    // Fija la raíz de fallback (assets del motor) y carga la lista de proyectos recientes.
    // NO abre ni crea ningún proyecto: la raíz queda vacía hasta newProject/openProject
    // (la pantalla de bienvenida los invoca). Llamar una vez al arrancar, tras anclar el cwd.
    void init();

    bool isOpen() const { return !m_root.empty(); }   // ¿hay un proyecto activo?

    // Resolución de rutas de contenido. Las rutas guardadas son relativas (p.ej.
    // "Assets/Data/scene.json"). Una ruta ABSOLUTA se devuelve tal cual.
    //  - resolveRead:  raíz del proyecto si el archivo existe ahí; si no, el fallback
    //                  del motor. Para CARGAR.
    //  - resolveWrite: SIEMPRE bajo la raíz del proyecto (crea las carpetas padre). Para GUARDAR.
    std::string resolveRead(const std::string& rel) const;
    std::string resolveWrite(const std::string& rel) const;

    void               setRoot(const std::string& dir);
    const std::string& root() const { return m_root; }
    const std::string& name() const { return m_name; }

    // --- Fase 2: archivo de proyecto (.pkproj) ---
    // newProject: crea la carpeta + esqueleto Assets/ + escribe <pkprojPath> y fija la raíz.
    // openProject: fija la raíz = carpeta de <pkprojPath> y lee nombre/escena inicial.
    bool        newProject(const std::string& pkprojPath, const std::string& name);
    bool        openProject(const std::string& pkprojPath);
    bool        save() const;                 // reescribe el .pkproj de la raíz actual
    const std::string& startScene() const { return m_startScene; }
    void               setStartScene(const std::string& rel) { m_startScene = rel; }
    // Registra la escena recién guardada como "escena inicial" del proyecto (relativa a la
    // raíz si está bajo ella) y reescribe el .pkproj. Así al reabrir el proyecto se recarga.
    void        noteSceneSaved(const std::string& absScenePath);

    // Copia los assets de contenido del motor (Models/Sprites, Textures, Data, Scripts) a la
    // raíz del proyecto (sin pisar los que ya existan). Para arrancar un proyecto con material.
    void        seedEngineAssets() const;
    // Importa un archivo externo al proyecto: lo copia a <raíz>/<destRelDir>/ y devuelve su
    // ruta RELATIVA ("Assets/Textures/x.png"). Si ya está bajo la raíz, no copia y la devuelve.
    // Cadena vacía si falla.
    std::string importAsset(const std::string& srcAbsPath, const std::string& destRelDir) const;

    // --- Proyectos recientes (config de USUARIO en SDL_GetPrefPath, no contenido) ---
    const std::vector<std::string>& recentProjects() const { return m_recents; }
    void addRecent(const std::string& absPkprojPath);   // de-dup al frente; persiste

private:
    Project() = default;
    void loadRecents();   // lee <prefpath>/recents.json
    void saveRecents() const;

    std::string m_root;          // carpeta del proyecto activo (absoluta); vacía = sin proyecto
    std::string m_engineAssets;  // raíz de fallback = assets del motor (absoluta)
    std::string m_name = "Default";
    std::string m_pkprojPath;    // ruta del .pkproj activo (para reescribirlo en save)
    std::string m_startScene;    // escena a cargar al abrir (relativa), opcional
    std::string m_prefDir;       // carpeta de config de usuario (SDL_GetPrefPath)
    std::vector<std::string> m_recents;   // .pkproj recientes (más nuevo primero)
    bool        m_inited = false;
};

}  // namespace pk

// Core/Project.h — raíz del PROYECTO activo (modelo de motor real). Un proyecto es una
// carpeta con un archivo ".pkproj" en la raíz; todo el CONTENIDO (escenas, tilesets +
// su PNG, sprites, scripts de gameplay, datos como species/moves/animaciones) se guarda
// con rutas RELATIVAS y se resuelve contra esa raíz. Si el proyecto no tiene un asset,
// se cae a los assets que TRAE el motor (fallback), así la demo funciona en cualquier
// proyecto nuevo sin copiar nada a mano.
//
// Los recursos del MOTOR (shaders en "Shaders/SPV/", fuentes en "Assets/Fonts/") NO
// pasan por aquí: viven junto al exe y se resuelven por el cwd (ver anchorCwdToExe).
//
// El motor NO trae contenido: lo que arranca un proyecto son las PLANTILLAS de
// "Templates/" (Base = los scripts que el editor adjunta al crear jugador/cámara; Demo =
// el mundo de ejemplo). Un proyecto sin un asset NO cae a ninguna parte: falta y punto.
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
    //  - resolveRead:  contra la raíz del proyecto. Para CARGAR. Sin proyecto abierto (o
    //                  si el archivo no está) devuelve la ruta tal cual: el motor ya no
    //                  presta contenido propio (eso lo hacen las plantillas, al crear).
    //  - resolveWrite: SIEMPRE bajo la raíz del proyecto (crea las carpetas padre). Para GUARDAR.
    std::string resolveRead(const std::string& rel) const;
    std::string resolveWrite(const std::string& rel) const;

    // Recursos que TRAE la aplicación (shaders, atlas de fuentes…), NO contenido del
    // proyecto del usuario: el post-build de CMake los deja junto al exe, así que se
    // buscan ahí primero. Devuelve una ruta ABSOLUTA, que resolveRead respeta tal cual —
    // necesario porque el fallback de contenido apunta al árbol de FUENTES en Debug
    // (POKEMOTOR_SOURCE_DIR), donde estos archivos no existen.
    std::string resolveAppFile(const std::string& rel) const;
    const std::string& exeDir() const { return m_exeDir; }

    void               setRoot(const std::string& dir);
    const std::string& root() const { return m_root; }
    const std::string& name() const { return m_name; }

    // --- Fase 2: archivo de proyecto (.pkproj) ---
    // newProject: crea la carpeta + esqueleto Assets/, vuelca la PLANTILLA elegida
    // ("Empty" o "Demo"), escribe <pkprojPath> y fija la raíz.
    // openProject: fija la raíz = carpeta de <pkprojPath> y lee nombre/escena inicial.
    bool        newProject(const std::string& pkprojPath, const std::string& name,
                           const std::string& templateName = std::string("Empty"));
    bool        openProject(const std::string& pkprojPath);
    bool        save() const;                 // reescribe el .pkproj de la raíz actual
    const std::string& startScene() const { return m_startScene; }
    void               setStartScene(const std::string& rel) { m_startScene = rel; }
    // Registra la escena recién guardada como "escena inicial" del proyecto (relativa a la
    // raíz si está bajo ella) y reescribe el .pkproj. Así al reabrir el proyecto se recarga.
    void        noteSceneSaved(const std::string& absScenePath);

    // Vuelca "Templates/<name>/" sobre la raíz del proyecto (sin pisar lo que ya exista).
    // Devuelve false si esa plantilla no existe. "Base" se copia SIEMPRE antes que la
    // elegida: trae los .lua que el editor adjunta al crear un jugador o una cámara.
    bool        copyTemplate(const std::string& name) const;
    // Nombres de plantilla disponibles (carpetas de "Templates/", menos Base). Para el
    // selector de la pantalla de bienvenida: la lista no está hardcodeada en la UI.
    std::vector<std::string> templateNames() const;
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
    std::string m_exeDir;        // directorio del ejecutable (absoluta); recursos de la app
    std::string m_name = "Default";
    std::string m_pkprojPath;    // ruta del .pkproj activo (para reescribirlo en save)
    std::string m_startScene;    // escena a cargar al abrir (relativa), opcional
    std::string m_prefDir;       // carpeta de config de usuario (SDL_GetPrefPath)
    std::vector<std::string> m_recents;   // .pkproj recientes (más nuevo primero)
    bool        m_inited = false;
};

}  // namespace pk

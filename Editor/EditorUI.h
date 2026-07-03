// Editor/EditorUI.h — UI del motor (FluentUI en modo Vulkan compartido).
// Diseño: MotorGrafico_UIEditor.md. Orquesta los paneles y se dibuja como último
// pass sobre la swapchain. Todo bajo #ifdef ENGINE_EDITOR (fuera de release).
#pragma once

#include "Editor/AssetBrowser.h"

#include <vulkan/vulkan.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace pk {

class VulkanContext;
class Renderer;
class AssetManager;
class EventBus;
class SceneManager;
class ScriptSystem;
struct Selection;

class EditorUI {
public:
    bool init(VulkanContext& ctx, SDL_Window* window, Renderer* renderer, AssetManager* assets);
    void shutdown();

    void setSelection(Selection* s) { m_selection = s; }   // entidad activa del editor
    void setEventBus(EventBus* bus) { m_eventBus = bus; }  // para emitir AssetDroppedEvent
    void setSceneManager(SceneManager* sm) { m_sceneMgr = sm; }  // jerarquía/inspector sobre la escena
    void setScriptSystem(ScriptSystem* ss) { m_scriptSys = ss; }  // exports del script en el inspector

    void beginInputFrame();                  // limpia flancos (1x por frame, ANTES de los eventos)
    void processEvent(const SDL_Event& e);   // alimenta el input de FluentUI
    void beginFrame(float dt);               // NewFrame + construye los paneles
    void render(VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D extent);  // graba draws
    bool wantsInput() const;                 // FluentUI quiere ratón/teclado (sobre cualquier panel/barra)
    bool consumeTileEditorRequest();         // true una vez si el usuario pidió abrir el editor de tiles
    bool consumeQuitRequest();               // true una vez si el usuario pidió salir (menú Archivo)

    // Pantalla de bienvenida (selector de proyecto). El Engine la activa al arrancar y la
    // apaga al montar el juego.
    void setWelcomeMode(bool w) { m_welcome = w; }
    // Si el usuario eligió crear/abrir un proyecto, devuelve true una vez y rellena la ruta
    // del .pkproj y si es nuevo (Save dialog) o existente (Open/reciente). El Engine lo aplica.
    bool consumeProjectChoice(std::string& outPath, bool& outIsNew);

private:
    void  buildPanels(float dt, int w, int h);
    void  buildWelcomeScreen(int w, int h);   // selector de proyecto (Nuevo/Abrir/recientes)
    void* thumbnailFor(VkImageView view);   // registra/cachea la textura para Image()
    void  drawFolderTree(const AssetNode& node);          // columna izq: solo carpetas
    void  drawAssetCards(const AssetNode& folder, float gridW);  // columna der: tarjetas

    // Menú Archivo: la op se pide desde el menú (su diálogo puede invocar el callback
    // en OTRO hilo) y se ejecuta en beginFrame —hilo principal, antes de dibujar— para
    // no cambiar la escena activa a mitad de frame.
    enum class SceneOp { None, New, Open, Save, SaveAs, NewProject, OpenProject };
    void applySceneOp(SceneOp op, const std::string& path);
    void openSaveDialog();   // diálogo "Guardar como" → encola SaveAs con la ruta elegida
    void refreshAfterProjectChange();   // tras Nuevo/Abrir proyecto: recarga tileset/assets, resetea scripts/selección

    SDL_Window*   m_window      = nullptr;
    Renderer*     m_renderer    = nullptr;
    AssetManager* m_assets      = nullptr;
    Selection*    m_selection   = nullptr;   // selección compartida (la posee el Engine)
    EventBus*     m_eventBus    = nullptr;   // para publicar el drop de assets al viewport
    SceneManager* m_sceneMgr    = nullptr;   // escena activa (jerarquía/inspector)
    ScriptSystem* m_scriptSys   = nullptr;   // exports del script seleccionado
    void*         m_uictx       = nullptr;   // FluentUI::UIContext* de este editor
    VkDevice      m_device      = VK_NULL_HANDLE;
    bool          m_initialized = false;
    bool          m_requestTileEditor = false;
    bool          m_requestQuit       = false;
    float         m_fps         = 0.0f;

    // Bienvenida: estado + elección pendiente (el callback del diálogo puede venir de otro
    // hilo; se protege con m_pendingMutex, igual que las ops de escena).
    bool          m_welcome            = false;
    bool          m_pendingProject     = false;
    bool          m_pendingProjectIsNew = false;
    std::string   m_pendingProjectPath;

    // Navegador de assets.
    std::unordered_map<VkImageView, void*> m_thumb;   // handles de miniatura (FluentUI)
    std::vector<std::string>               m_pendingLoads;  // rutas a cargar (hilo principal)
    std::mutex                             m_pendingMutex;
    SceneOp                                m_pendingSceneOp = SceneOp::None;   // acción del menú Archivo
    std::string                            m_pendingScenePath;                 // ruta del diálogo (Open/SaveAs)
    int                                    m_bottomTab = 0; // 0 = Consola, 1 = Assets
    AssetBrowser                           m_assetBrowser;        // escaneo de Assets/ en disco
    bool                                   m_assetsScanned = false;
    std::unordered_map<std::string, bool>  m_folderOpen;          // expansión por ruta de carpeta
    std::string                            m_selectedFolder = "Assets";  // carpeta mostrada en el grid
    std::string                            m_dragSourcePath;             // tarjeta que originó el drag
};

}  // namespace pk

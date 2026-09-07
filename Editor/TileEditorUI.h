// Editor/TileEditorUI.h — editor de tiles que vive en la 2ª ventana (ToolWindow).
// Edita el mismo mapa que el overworld, que ahora vive en el TileMapComponent de la
// escena ACTIVA (no en overworld.json): paleta de tipos, lienzo con pinceles (pintar /
// rectángulo / balde / borrador) y guardar/recargar (vuelca/relee el componente).
// Dibuja el lienzo con el renderer del contexto FluentUI actual.
#pragma once

#include "Core/Math.h"
#include "Game/TileMap.h"
#include "Math/Rect.h"   // FluentUI::Rect: rect del pane que reciben las superficies

#include <mutex>
#include <string>
#include <unordered_set>

struct SDL_Window;

namespace pk {

class AssetManager;
class EventBus;
class Scene;
class SceneManager;
struct TileMapComponent;

class TileEditor {
public:
    void build(int width, int height);   // construye la UI + lienzo (contexto FluentUI actual)

    // El motor inyecta el bus para notificar (MapSavedEvent) al guardar (el overworld
    // recarga en caliente) y el SceneManager para leer/escribir el TileMapComponent de
    // la escena ACTIVA (el mapa vive en la escena, no en overworld.json).
    void setEventBus(EventBus* bus) { m_bus = bus; }
    void setSceneManager(SceneManager* sm) { m_scenes = sm; }
    // AssetManager para cargar el tileset PNG y pintar la paleta/lienzo con el arte real
    // (si no hay tileset, se cae a color plano por tipo).
    void setAssetManager(AssetManager* am) { m_assets = am; }
    // Ventana de la 2ª ventana OS (la fija el Engine al abrirla): parenta el diálogo
    // nativo de "Cambiar imagen del tileset".
    void setWindow(SDL_Window* w) { m_window = w; }

    // Invalida el handle cacheado del atlas para FluentUI. DEBE llamarse cuando la 2ª
    // ventana se (re)abre: su backend FluentUI se recrea desde cero en cada apertura
    // (CreateStandaloneContext), así que el VulkanTexture* registrado en el backend
    // anterior queda colgante (su descriptor pool se destruyó en DestroyStandaloneContext).
    // El VkImageView del tileset SÍ sobrevive (lo posee el AssetManager), por eso el
    // chequeo "view != m_atlasView" no detecta el cambio y hay que forzar el re-registro.
    void resetAtlasCache() { m_atlasUi = nullptr; m_atlasView = nullptr; }

private:
    enum class Tool { Paint, Rect, Fill, Erase };
    enum class Screen { Map, Tileset };   // pintar el mapa vs configurar el tileset

    TileMapComponent* mapComp();   // componente-mapa de la escena activa (o nullptr)
    void ensureLoaded();
    void buildMapScreen(int width, int height);      // pantalla de pintura del mapa
    void buildTilesetScreen(int width, int height);  // pantalla de configuración del tileset
    // Superficies dibujadas a mano (rejillas con hit-test propio). Reciben el RECT donde
    // pintar —el que les da el Splitter— en vez de deducirlo de constantes de layout.
    void drawPalette(const FluentUI::Rect& r);
    void drawCanvas(const FluentUI::Rect& r);
    // Pantalla de tileset: lista de tipos (izquierda) + atlas recortado (derecha).
    void drawTypeList(const FluentUI::Rect& r);
    void drawAtlasPanel(const FluentUI::Rect& r);
    // Rect libre que queda en el pane actual del Splitter, desde el cursor hasta abajo.
    FluentUI::Rect remainingPane(FluentUI::Vec2 paneOrigin, FluentUI::Vec2 paneSize) const;
    void saveTileset();         // escribe tileset.json + avisa al overworld para reconstruir
    void addNewType();          // añade un tipo nuevo y lo selecciona
    void addNewGroup();         // añade un tipo nuevo dentro de un grupo nuevo y lo selecciona
    void removeSelectedType();  // borra el tipo seleccionado y remapea la copia de trabajo del mapa
    void changeImageDialog();   // abre el diálogo nativo para elegir la imagen del tileset
    void applyPendingImage();   // aplica (en el hilo principal) la imagen elegida en el diálogo
    // Handle del atlas del tileset envuelto para FluentUI (o nullptr si no hay tileset);
    // carga perezosa + registro (cacheado).
    void* atlasHandle();
    void save();
    void reload();
    void resizeMap();            // aplica m_mapWBuf×m_mapHBuf a la copia de trabajo del mapa
    void syncSizeBuffers();      // vuelca el tamaño actual del mapa a los buffers de texto
    void floodFill(int x, int y, TileType from, TileType to);

    EventBus*     m_bus    = nullptr;   // notifica al overworld al guardar (no propio)
    SceneManager* m_scenes = nullptr;   // escena activa (donde vive el TileMapComponent)
    AssetManager* m_assets = nullptr;   // carga el tileset PNG (no propio)
    SDL_Window*   m_window = nullptr;    // para parentar el diálogo de imagen (no propio)
    void*         m_atlasUi   = nullptr;   // textura del atlas envuelta para FluentUI (cacheada)
    void*         m_atlasView = nullptr;   // VkImageView crudo con el que se registró (detecta cambios)

    // Imagen elegida en el diálogo nativo: el callback puede venir de OTRO hilo, así que
    // sólo deja la ruta aquí y la aplicamos en build() (hilo principal). Ver applyPendingImage.
    std::mutex  m_imgMutex;
    std::string m_pendingImage;
    bool        m_hasPendingImage = false;
    TileMap m_map;
    bool         m_loaded    = false;
    const Scene* m_lastScene = nullptr;   // detecta cambio de escena/proyecto para recargar m_map
    int     m_activeType = 1;            // 1 = Grass (tipo a pintar en el mapa)
    int     m_selType    = 0;            // tipo seleccionado en la pantalla de tileset
    Screen  m_screen     = Screen::Map;  // pantalla activa
    Tool    m_tool       = Tool::Paint;
    bool    m_rectActive = false;
    IVec2   m_rectStart{ 0, 0 };
    IVec2   m_rectEnd{ 0, 0 };
    double  m_mapW = 0.0;          // campos del control "Redimensionar" (NumberBox trabaja en double)
    double  m_mapH = 0.0;
    // Ratios de los Splitters de cada pantalla (paleta|lienzo y tipos|atlas). Son estado
    // editable: el divisor se arrastra igual que en la ventana principal.
    float   m_ratioPalette = 0.22f;
    float   m_ratioTypes   = 0.34f;
    bool    m_mapSizeOpen  = true;   // Expander "Mapa" (tamaño) desplegado
    bool    m_typeInspOpen = true;   // Expander "Tipo seleccionado" desplegado
    // Scroll (con la rueda) de las listas de tipos: config (drawTypeList) y paleta (drawPalette).
    float   m_typeScroll    = 0.0f;
    float   m_paletteScroll = 0.0f;
    // Grupos plegados (dropdown): compartido por ambas listas; su cabecera oculta los miembros.
    std::unordered_set<std::string> m_collapsedGroups;
    char    m_status[96] = { 0 };
};

}  // namespace pk

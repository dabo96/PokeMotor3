#pragma once
// ─── Brief 32: widget Card (data-driven) + one-shots de conveniencia ─────────
//
// UN SOLO contenedor Card parametrizado por CardConfig (4 estilos ×
// {estática, clicable, seleccionable}) + 3 one-shots de azúcar. Las "variantes"
// son combinaciones de config, NO widgets separados. La Card es una fachada
// semántica sobre primitivas que YA existen: resuelve un FluentMaterial
// (WidgetRole::Card), dibuja fondo/borde/sombra con las primitivas SDF del
// Renderer (que consumen reveal) y abre un scope de layout para el contenido.
//
// NO existe un ExpandableCard: eso es el Expander (brief 14) con estilo de card.

#include <string>
#include <functional>
#include <cstdint>
#include "Math/Vec2.h"

namespace FluentUI {

enum class CardStyle {
    Elevated,   ///< sombra key+ambient (default Fluent); flota sobre el fondo.
    Outlined,   ///< sin sombra, solo borde (listas densas).
    Filled,     ///< fondo tintado sutil, sin borde.
    Acrylic,    ///< fondo translúcido con blur (brief 06).
};

struct CardConfig {
    CardStyle style      = CardStyle::Elevated;
    bool      clickable  = false;   ///< toda la card actúa como botón.
    bool      selectable = false;   ///< mantiene estado on/off con marca de selección.
    float     radius     = 0.0f;    ///< 0 = CornerTokens::Medium.
    float     padding    = 0.0f;    ///< 0 = padding por defecto (S(16)).
    float     elevationZ = -1.0f;   ///< <0 = Elevation::Z::Card; permite override.
    Vec2      size       = Vec2(0, 0); ///< (0,0) = auto (ancho del layout, alto del contenido).
    bool      reveal     = false;   ///< opt-in: resaltado del borde por proximidad del cursor
                                    ///< (si clickable/selectable). Desactivado por defecto — el
                                    ///< feedback de hover es el FONDO, no el borde.
    bool      fitContent = false;   ///< ancho = ancho del contenido + padding (shrink-to-fit,
                                    ///< ignora size.x). Usar con contenido de ancho natural
                                    ///< (labels/botones); NO con LabelWrapped (dependencia circular).
};

// ── Primitiva (contrato Begin/End; el cuerpo se construye SIEMPRE) ────────────
// @return para una card clicable: true el frame en que se activó (click válido,
//         excluyendo clicks sobre interactivos internos). Para una seleccionable:
//         el estado de selección. Estática: siempre false.
// NOTA (immediate-mode): la activación con auto-exclusión solo se conoce tras
// dibujar el contenido, así que BeginCard devuelve el valor finalizado el frame
// ANTERIOR; la fachada Card(...) devuelve el de ESTE frame. Usa la fachada si
// necesitas la activación al instante.
bool BeginCard(const std::string& id, const CardConfig& cfg = {}, bool* selected = nullptr);
void EndCard();

// ── Fachada callback (brief 31; la forma legible, activación same-frame) ──────
bool Card(const std::string& id, const CardConfig& cfg, std::function<void()> content);
inline bool Card(const std::string& id, std::function<void()> content) {
    return Card(id, CardConfig{}, std::move(content));
}

// ── One-shots de conveniencia (el azúcar del 80%) ────────────────────────────

// Título + descripción + icono opcional. Clicable opcional (tiles de dashboard).
bool InfoCard(const std::string& id, const std::string& title,
              const std::string& description, uint32_t icon = 0, bool clickable = false);

// Patrón "SettingsCard" del Community Toolkit: icono + título + subtítulo a la
// izquierda, control arbitrario a la derecha (ToggleSwitch, ComboBox, flecha…).
// La `action` queda auto-excluida de la activación de la card.
void SettingsCard(const std::string& id, uint32_t icon,
                  const std::string& title, const std::string& subtitle,
                  std::function<void()> action);

// Media arriba (imagen/thumbnail) + contenido debajo. Patrón galería/producto.
bool MediaCard(const std::string& id, void* imageTexture, Vec2 imageSize,
               std::function<void()> content, bool clickable = false);

} // namespace FluentUI

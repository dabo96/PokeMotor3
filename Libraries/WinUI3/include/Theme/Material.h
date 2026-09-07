#pragma once
// Brief 07 — data-driven Fluent material model.
//
// A FluentMaterial captures everything the SDF rounded-rect pipeline needs to
// draw a widget surface (fill, border, corner, elevation, reveal, acrylic). It is
// derived from design tokens + the active Style via ResolveMaterial(role, state),
// replacing the ad-hoc per-widget color/elevation maths scattered across src/UI/*.
//
// The existing Style API is NOT broken: materials are COMPOSED from the active
// Style's sub-styles (ButtonStyle/PanelStyle) + the token scales below, so themes
// (GetDefaultFluentStyle / GetDarkFluentStyle / GetHighContrastStyle /
// CreateCustomFluentStyle) keep working unchanged.

#include "Math/Color.h"
#include "Math/Vec2.h"
#include "Theme/Style.h"
#include "core/Elevation.h"
#include "core/RenderBackend.h" // SDFInstance
#include <string>

namespace FluentUI {

// Everything the SDF shader needs for one widget surface. POD (trivially
// copyable / serializable).
struct FluentMaterial {
    Color fill;                       // base fill
    Color border;                     // border / outline color (top of the elevation border)
    // Brief 34 Parte E: color del borde inferior (bisel de elevación Win11). Por
    // defecto == border ⇒ borde plano. ResolveMaterial lo separa para botones/cards.
    Color borderBottom;               // bottom border color (flat when == border)
    float borderWidth = 0.0f;         // px (0 = no border)
    float radius = 4.0f;              // corner radius (px) — a corner token
    float elevationZ = 0.0f;          // Elevation::Z::* → soft drop shadow
    float revealIntensity = 0.0f;     // 0..1 cursor reveal on the edge (brief 04)
    bool  acrylic = false;            // if true the fill uses the brief-06 pipeline
    float tintOpacity = 0.15f;        // acrylic tint blend
    float luminosityOpacity = 0.85f;  // acrylic luminosity blend
};

// ── Tokens ──────────────────────────────────────────────────────────────────
// Corner radius scale (centralizes the magic radii spread across src/UI/*).
namespace CornerTokens {
    constexpr float Small  = 4.0f;
    constexpr float Medium = 8.0f;
    constexpr float Large  = 12.0f;
}
// Stroke scale.
namespace StrokeTokens {
    constexpr float Thin = 1.0f; // standard 1px logical border
}

// Brief 34 Parte B — familias de tokens de color WinUI 3. Los valores neutros y de
// señal se leen del header generado WinUITokens.h (Light()/Dark()); la familia de
// ACENTO no está en ese archivo (SystemAccentColor* vive fuera) y sigue viniendo del
// Style — la sustituirá la paleta perceptual de la Parte D. Semi-transparentes: se
// COMPONEN sobre el fondo (por eso un control deja translucir el acrylic de detrás).
struct ColorTokens {
    // ── Text ──────────────────────────────────────────────────────────────────
    Color textPrimary, textSecondary, textTertiary, textDisabled;
    Color textOnAccentPrimary, textOnAccentDisabled;
    // ── Fill (control) ────────────────────────────────────────────────────────
    Color controlFillDefault, controlFillSecondary, controlFillTertiary;
    Color controlFillDisabled, controlFillInputActive;
    Color subtleFillSecondary, subtleFillTertiary;   // hover / press de fondo sutil
    // ── Background ────────────────────────────────────────────────────────────
    Color cardBackgroundDefault, cardBackgroundSecondary;
    Color layerFillDefault;
    Color solidBackgroundBase, solidBackgroundSecondary, solidBackgroundTertiary;
    Color smokeFill;                                  // scrim de diálogos
    // ── Stroke ────────────────────────────────────────────────────────────────
    Color controlStrokeDefault, controlStrokeSecondary;
    Color cardStroke, dividerStroke, surfaceStroke;
    Color focusStrokeOuter, focusStrokeInner;
    // ── Accent (del Style; Parte D) ──────────────────────────────────────────
    Color accent, accentHover, accentPressed;
    Color accentFillDisabled;   // AccentFillColorDisabled (token WinUI, por tema)
    // ── System (señal) + sus fondos (los pide el InfoBar, brief 15) ───────────
    Color success, successBackground;
    Color caution, cautionBackground;
    Color critical, criticalBackground;
    Color attentionBackground;
};

namespace Tokens {
    ColorTokens Light();
    ColorTokens Dark();
    // Tokens for the active theme: light/dark base + accent (and its hover/pressed)
    // derived from the Style's accentColor, exactly like CreateCustomFluentStyle.
    ColorTokens FromStyle(const Style& theme);
}

// ── Roles / states ──────────────────────────────────────────────────────────
enum class WidgetRole {
    ButtonPrimary,
    ButtonSecondary,
    Card,
    Surface,
    Input,
    Flyout,
    Dialog,
    MenuItem,
};

enum class WidgetState { Rest, Hover, Pressed, Disabled, Focused };

// ── Resolution ──────────────────────────────────────────────────────────────
// Headline API: resolve a role+state against the active theme.
FluentMaterial ResolveMaterial(WidgetRole role, WidgetState state, const Style& theme);

// Brief 34 Parte E: aplica el bisel de elevación Win11 (borde 1px con degradado
// vertical superior→inferior desde tokens de trazo) a un material. Idempotente.
//
// `baseFill` (opcional): fill sobre el que se APLANAN los trazos alpha. Por defecto
// se usa m.fill; los widgets cuyo fill cambia con el estado (botones) deben pasar el
// fill EN REPOSO para que el contorno sea idéntico en rest/hover/pressed — el único
// cambio visible al pasar el ratón debe ser el FONDO, nunca el borde.
void ApplyElevationBorder(FluentMaterial& m, const Style& theme,
                          const Color* baseFill = nullptr);

// Lower-level builders used by the pilot widgets so they honor the EFFECTIVE
// sub-style (which may be pushed independently via the style stacks) and keep
// exact visual parity with the pre-refactor code.
FluentMaterial ResolveButtonMaterial(const ButtonStyle& bs, WidgetState state);
FluentMaterial ResolvePanelMaterial(const PanelStyle& ps, WidgetState state);

// Translate a material to a single SDF rounded-rect instance (fill + border +
// reveal, mode 0). Provided as the building block the briefs 02/03 did by hand;
// the pilots draw via the Renderer primitives (which build instances internally)
// to preserve batching and reveal-consume semantics.
SDFInstance MakeInstance(const Vec2& pos, const Vec2& size,
                         const FluentMaterial& m, float dpiScale);

// ── Serialization (brief 07, optional / data-driven) ─────────────────────────
// FluentMaterial is POD, so we expose a minimal JSON round-trip. No JSON library
// dependency (matching LayoutSerializer's no-dep philosophy): the object is flat,
// colors are written as "#RRGGBBAA" hex strings (hand-editable), the rest as
// numbers/bools. Lets themes / per-widget styles live in data files.
//
// MaterialFromJson is tolerant: unknown keys are ignored and absent keys keep
// the FluentMaterial default, so files survive forward/backward field changes.
std::string    MaterialToJson(const FluentMaterial& m);
FluentMaterial MaterialFromJson(const std::string& json);

// File helpers (return false on I/O failure; parse errors fall back to defaults).
bool SaveMaterial(const std::string& filepath, const FluentMaterial& m);
bool LoadMaterial(const std::string& filepath, FluentMaterial& out);

} // namespace FluentUI

// Brief 07 — data-driven Fluent material model (implementation).
#include "Theme/Material.h"
#include "Theme/FluentTheme.h"
#include "Theme/WinUITokens.h" // brief 34 Parte B: fuente de los tokens neutros/señal
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace FluentUI {

// Brief 34 Parte B — proyecta un WinUITokens (Light/Dark) sobre las familias de
// ColorTokens. La familia de acento NO sale de aquí (no está en el archivo del SDK):
// la rellena FromStyle desde el Style. Ningún hex a mano.
static ColorTokens FromWinUI(const WinUITokens& w) {
    ColorTokens t{};
    // Text
    t.textPrimary          = w.TextFillColorPrimary;
    t.textSecondary        = w.TextFillColorSecondary;
    t.textTertiary         = w.TextFillColorTertiary;
    t.textDisabled         = w.TextFillColorDisabled;
    t.textOnAccentPrimary  = w.TextOnAccentFillColorPrimary;
    t.textOnAccentDisabled = w.TextOnAccentFillColorDisabled;
    // Fill (control)
    t.controlFillDefault     = w.ControlFillColorDefault;
    t.controlFillSecondary   = w.ControlFillColorSecondary;
    t.controlFillTertiary    = w.ControlFillColorTertiary;
    t.controlFillDisabled    = w.ControlFillColorDisabled;
    t.controlFillInputActive = w.ControlFillColorInputActive;
    t.subtleFillSecondary    = w.SubtleFillColorSecondary;
    t.subtleFillTertiary     = w.SubtleFillColorTertiary;
    // Background
    t.cardBackgroundDefault   = w.CardBackgroundFillColorDefault;
    t.cardBackgroundSecondary = w.CardBackgroundFillColorSecondary;
    t.layerFillDefault        = w.LayerFillColorDefault;
    t.solidBackgroundBase      = w.SolidBackgroundFillColorBase;
    t.solidBackgroundSecondary = w.SolidBackgroundFillColorSecondary;
    t.solidBackgroundTertiary  = w.SolidBackgroundFillColorTertiary;
    t.smokeFill               = w.SmokeFillColorDefault;
    // Stroke
    t.controlStrokeDefault   = w.ControlStrokeColorDefault;
    t.controlStrokeSecondary = w.ControlStrokeColorSecondary;
    t.cardStroke             = w.CardStrokeColorDefault;
    t.dividerStroke          = w.DividerStrokeColorDefault;
    t.surfaceStroke          = w.SurfaceStrokeColorDefault;
    t.focusStrokeOuter       = w.FocusStrokeColorOuter;
    t.focusStrokeInner       = w.FocusStrokeColorInner;
    // System (señal) + fondos
    t.success            = w.SystemFillColorSuccess;
    t.successBackground  = w.SystemFillColorSuccessBackground;
    t.caution            = w.SystemFillColorCaution;
    t.cautionBackground  = w.SystemFillColorCautionBackground;
    t.critical           = w.SystemFillColorCritical;
    t.criticalBackground = w.SystemFillColorCriticalBackground;
    t.attentionBackground = w.SystemFillColorAttentionBackground;
    // Accent disabled: token WinUI real (por tema); el resto de la familia de acento
    // la rellenan Light()/Dark()/FromStyle desde el Style (Parte D).
    t.accentFillDisabled = w.AccentFillColorDisabled;
    return t;
}

ColorTokens Tokens::Light() {
    ColorTokens t = FromWinUI(WinUITokens::Light());
    // Acento por defecto (Parte D lo reemplazará por una paleta perceptual).
    t.accent        = FluentColors::Accent;
    t.accentHover   = FluentColors::AccentHover;
    t.accentPressed = FluentColors::AccentPressed;
    return t;
}

ColorTokens Tokens::Dark() {
    ColorTokens t = FromWinUI(WinUITokens::Dark());
    t.accent        = FluentColors::Accent;
    t.accentHover   = FluentColors::AccentHover;
    t.accentPressed = FluentColors::AccentPressed;
    return t;
}

ColorTokens Tokens::FromStyle(const Style& theme) {
    ColorTokens t = theme.isDarkTheme ? Dark() : Light();
    // theme.accentColor ya es el tono de acento RESUELTO por tema (Parte D:
    // Light2 en oscuro / Dark1 en claro). Hover/pressed = ese tono a 0.9/0.8 de
    // opacidad, igual que AccentFillColorSecondary/Tertiary de WinUI (compone
    // sobre el fondo, robusto en acentos extremos — ya no suma/multiplica RGB).
    const Color a = theme.accentColor;
    t.accent        = a;
    t.accentHover   = Color(a.r, a.g, a.b, 0.9f);
    t.accentPressed = Color(a.r, a.g, a.b, 0.8f);
    return t;
}

// Pick the per-state color out of a ColorState (matches the widgets' getTargetColor).
static Color PickState(const ColorState& cs, WidgetState state) {
    switch (state) {
        case WidgetState::Disabled: return cs.disabled;
        case WidgetState::Pressed:  return cs.pressed;
        case WidgetState::Hover:    return cs.hover;
        case WidgetState::Rest:
        case WidgetState::Focused:
        default:                    return cs.normal;
    }
}

FluentMaterial ResolveButtonMaterial(const ButtonStyle& bs, WidgetState state) {
    FluentMaterial m;
    m.fill        = PickState(bs.background, state);
    // El contorno NO reacciona al puntero: hover/pressed/focused reutilizan el borde
    // EN REPOSO. El feedback de interacción es el FONDO (fill), no el borde — un borde
    // que cambia de color al pasar el ratón se lee como un "flash" y no es Fluent.
    // Disabled sí conserva el suyo (los temas lo ponen transparente/atenuado a propósito).
    m.border      = PickState(bs.border, state == WidgetState::Disabled
                                             ? WidgetState::Disabled
                                             : WidgetState::Rest);
    m.borderBottom = m.border; // plano por defecto (brief 34 Parte E)
    m.borderWidth = bs.borderWidth;
    m.radius      = bs.cornerRadius;
    // State → z, identical to the previous inline mapping in Button:
    //   rest = ButtonRest(0), hover = ButtonHover, pressed = ButtonPressed, disabled = 0.
    m.elevationZ = (state == WidgetState::Disabled) ? 0.0f
                 : (state == WidgetState::Pressed)  ? Elevation::Z::ButtonPressed
                 : (state == WidgetState::Hover)    ? Elevation::Z::ButtonHover
                                                    : Elevation::Z::ButtonRest;
    // Reveal (brief 04) DESACTIVADO en botones: iluminaba la banda del borde hacia
    // blanco por proximidad del cursor (radio 120px), o sea otra forma de "el borde
    // cambia al hacer hover". El resaltado del cursor queda disponible en el shader
    // para quien lo quiera (Card::reveal), pero no se aplica por defecto.
    m.revealIntensity = 0.0f;
    return m;
}

FluentMaterial ResolvePanelMaterial(const PanelStyle& ps, WidgetState state) {
    (void)state; // panels have a single resting appearance in the current Style
    FluentMaterial m;
    m.fill        = ps.background;
    m.border      = ps.borderColor;
    m.borderBottom = m.border; // plano por defecto (brief 34 Parte E)
    m.borderWidth = ps.borderWidth;
    m.radius      = ps.cornerRadius;
    m.elevationZ  = Elevation::Z::Card; // a card/panel floats at z=Card
    m.revealIntensity = 0.0f;
    m.acrylic     = ps.useAcrylic;
    m.tintOpacity = std::clamp(0.10f + 0.10f * ps.acrylicOpacity, 0.0f, 1.0f);
    m.luminosityOpacity = 0.85f;
    return m;
}

// Compone un color alpha (fg) sobre una base opaca (bg) → color OPACO equivalente.
static Color FlattenOver(const Color& fg, const Color& bg) {
    const float a = fg.a;
    return Color(bg.r * (1.0f - a) + fg.r * a,
                 bg.g * (1.0f - a) + fg.g * a,
                 bg.b * (1.0f - a) + fg.b * a, 1.0f);
}

void ApplyElevationBorder(FluentMaterial& m, const Style& theme,
                          const Color* baseFill) {
    // Brief 34 Parte F: en HighContrast no hay bisel ni reveal. El borde plano y
    // opaco lo pone el propio estilo HC (windowText, 2px); aquí solo lo respetamos.
    if (theme.isHighContrast) {
        m.borderBottom = m.border; // plano (sin degradado)
        m.revealIntensity = 0.0f;  // sin reveal en HC
        return;
    }
    const ColorTokens t = Tokens::FromStyle(theme);
    if (m.borderWidth <= 0.0f) m.borderWidth = StrokeTokens::Thin; // 1px lógico
    // Bisel de elevación Win11 (trazo con degradado vertical). Los trazos WinUI
    // difieren en ALPHA (secondary más opaco que default), no en RGB; y el shader
    // interpola RGB con alpha común. Por eso APLANAMOS cada trazo sobre el fill: el
    // resultado son dos colores opacos que difieren en RGB y sí producen gradiente.
    // Oscuro: realce (secondary) arriba, base (default) abajo. Claro: al revés, para
    // que la sombra quede abajo. Ambos trazos salen de tokens WinUI.
    const Color topStroke = theme.isDarkTheme ? t.controlStrokeSecondary : t.controlStrokeDefault;
    const Color botStroke = theme.isDarkTheme ? t.controlStrokeDefault   : t.controlStrokeSecondary;
    // El aplanado usa `baseFill` cuando el llamante lo aporta (fill en reposo) para
    // que el contorno NO se aclare/oscurezca al cambiar de estado; si no, m.fill.
    const Color base = baseFill ? *baseFill : m.fill;
    m.border       = FlattenOver(topStroke, base);
    m.borderBottom = FlattenOver(botStroke, base);
}

FluentMaterial ResolveMaterial(WidgetRole role, WidgetState state, const Style& theme) {
    switch (role) {
        case WidgetRole::ButtonPrimary:
        case WidgetRole::ButtonSecondary:
        case WidgetRole::MenuItem:
            return ResolveButtonMaterial(theme.button, state);
        case WidgetRole::Card: {
            FluentMaterial m = ResolvePanelMaterial(theme.panel, state);
            ApplyElevationBorder(m, theme); // brief 34 Parte E: bisel en cards (outlined)
            return m;
        }
        case WidgetRole::Surface:
        case WidgetRole::Input:
            return ResolvePanelMaterial(theme.panel, state);
        case WidgetRole::Flyout:
        case WidgetRole::Dialog: {
            FluentMaterial m = ResolvePanelMaterial(theme.panel, state);
            m.elevationZ = (role == WidgetRole::Dialog) ? Elevation::Z::Dialog
                                                        : Elevation::Z::Flyout;
            return m;
        }
        default:
            return ResolvePanelMaterial(theme.panel, state);
    }
}

SDFInstance MakeInstance(const Vec2& pos, const Vec2& size,
                         const FluentMaterial& m, float dpiScale) {
    SDFInstance s{};
    s.cx = pos.x + size.x * 0.5f;
    s.cy = pos.y + size.y * 0.5f;
    s.hx = size.x * 0.5f;
    s.hy = size.y * 0.5f;
    s.radius = std::clamp(m.radius, 0.0f, std::min(s.hx, s.hy));
    s.softness = std::max(1.0f, dpiScale);
    s.mode = 0.0f;
    s.fillR = m.fill.r; s.fillG = m.fill.g; s.fillB = m.fill.b; s.fillA = m.fill.a;
    if (m.borderWidth > 0.0f) {
        s.borderWidth = std::max(1.0f, dpiScale) * m.borderWidth;
        s.borderR = m.border.r; s.borderG = m.border.g; s.borderB = m.border.b; s.borderA = m.border.a;
        // Brief 34 Parte E: color inferior del bisel (alpha común = borderA). Si es
        // igual al superior, el shader lo trata como borde plano (retrocompatible).
        s.borderR2 = m.borderBottom.r; s.borderG2 = m.borderBottom.g; s.borderB2 = m.borderBottom.b;
    }
    s.revealIntensity = m.revealIntensity;
    return s;
}

// ── Serialization ────────────────────────────────────────────────────────────
namespace {

// Color → "#RRGGBBAA". (Color::ToUint32 packs ABGR, so we format channels by hand.)
std::string ColorToHex(const Color& c) {
    auto byte = [](float v) {
        return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    char buf[10];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X",
                  byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    return std::string(buf);
}

// Tolerant scalar finder: locate "key" and return the text after its ':' up to
// the next ',' or '}'. Returns false if the key is absent.
bool FindValue(const std::string& json, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' ||
                               json[i] == '\n' || json[i] == '\r')) ++i;
    size_t end = i;
    while (end < json.size() && json[end] != ',' && json[end] != '}' &&
           json[end] != '\n' && json[end] != '\r') ++end;
    out = json.substr(i, end - i);
    // trim trailing whitespace
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return !out.empty();
}

void ParseFloat(const std::string& json, const char* key, float& dst) {
    std::string v;
    if (FindValue(json, key, v)) dst = std::strtof(v.c_str(), nullptr);
}

void ParseBool(const std::string& json, const char* key, bool& dst) {
    std::string v;
    if (FindValue(json, key, v)) dst = (v.find("true") != std::string::npos);
}

void ParseColor(const std::string& json, const char* key, Color& dst) {
    std::string v;
    if (!FindValue(json, key, v)) return;
    size_t q1 = v.find('"');
    size_t q2 = (q1 == std::string::npos) ? std::string::npos : v.find('"', q1 + 1);
    if (q1 != std::string::npos && q2 != std::string::npos) {
        dst = Color::FromHex(v.substr(q1, q2 - q1 + 1).c_str());
    }
}

} // namespace

std::string MaterialToJson(const FluentMaterial& m) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"fill\": \""              << ColorToHex(m.fill)   << "\",\n";
    os << "  \"border\": \""            << ColorToHex(m.border) << "\",\n";
    os << "  \"borderBottom\": \""      << ColorToHex(m.borderBottom) << "\",\n";
    os << "  \"borderWidth\": "         << m.borderWidth        << ",\n";
    os << "  \"radius\": "              << m.radius             << ",\n";
    os << "  \"elevationZ\": "          << m.elevationZ         << ",\n";
    os << "  \"revealIntensity\": "     << m.revealIntensity    << ",\n";
    os << "  \"acrylic\": "             << (m.acrylic ? "true" : "false") << ",\n";
    os << "  \"tintOpacity\": "         << m.tintOpacity        << ",\n";
    os << "  \"luminosityOpacity\": "   << m.luminosityOpacity  << "\n";
    os << "}\n";
    return os.str();
}

FluentMaterial MaterialFromJson(const std::string& json) {
    FluentMaterial m; // start from defaults; absent keys keep them
    ParseColor(json, "fill",   m.fill);
    ParseColor(json, "border", m.border);
    // Brief 34 Parte E: borderBottom por defecto = border (plano); si el JSON lo trae,
    // lo sobreescribe. JSON antiguo sin la clave ⇒ borde plano, como antes.
    m.borderBottom = m.border;
    ParseColor(json, "borderBottom", m.borderBottom);
    ParseFloat(json, "borderWidth",       m.borderWidth);
    ParseFloat(json, "radius",            m.radius);
    ParseFloat(json, "elevationZ",        m.elevationZ);
    ParseFloat(json, "revealIntensity",   m.revealIntensity);
    ParseBool (json, "acrylic",           m.acrylic);
    ParseFloat(json, "tintOpacity",       m.tintOpacity);
    ParseFloat(json, "luminosityOpacity", m.luminosityOpacity);
    return m;
}

bool SaveMaterial(const std::string& filepath, const FluentMaterial& m) {
    std::ofstream f(filepath, std::ios::binary);
    if (!f) return false;
    f << MaterialToJson(m);
    return static_cast<bool>(f);
}

bool LoadMaterial(const std::string& filepath, FluentMaterial& out) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = MaterialFromJson(ss.str());
    return true;
}

} // namespace FluentUI

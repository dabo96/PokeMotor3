#pragma once
#include "Math/Color.h"
#include "Style.h"
#include "Theme/WinUITokens.h" // brief 34 Parte B: fachada sobre tokens WinUI 3
#include <algorithm>

namespace FluentUI {

    // Colores oficiales del Fluent Design System.
    //
    // Brief 34 Parte B — FluentColors es ahora una FACHADA DELGADA sobre los tokens
    // generados (WinUITokens.h). Los miembros neutros (fondos, superficies, texto,
    // bordes) leen del token WinUI equivalente en vez de valores calibrados a ojo;
    // las calibraciones manuales de contraste se han retirado. Excepciones que NO
    // salen del archivo del SDK y se conservan:
    //   · Familia de ACENTO (Accent*, presets AccentBlue..Teal, GetAccent*): el
    //     SystemAccentColor no vive en este archivo — lo sustituye la paleta
    //     perceptual de la Parte D.
    //   · Señal (Error/Success/Warning/Info): son constantes ÚNICAS agnósticas de
    //     tema; su versión theme-aware (con fondo) ya vive en ColorTokens (familia
    //     System) para migrar los widgets a ella en una fase posterior.
    namespace FluentColors {
        namespace detail {
            // Se evalúan en compile-time (Color y las fábricas son constexpr): sin
            // coste en runtime ni problemas de orden de inicialización estática.
            inline constexpr WinUITokens kL = WinUITokens::Light();
            inline constexpr WinUITokens kD = WinUITokens::Dark();
        }

        // Colores base (Light theme) — fondos de página/panel opacos → variantes Solid.
        inline constexpr Color Background     = detail::kL.SolidBackgroundFillColorBase;
        inline constexpr Color Surface        = detail::kL.SolidBackgroundFillColorSecondary;
        inline constexpr Color SurfaceAlt     = detail::kL.SolidBackgroundFillColorTertiary;
        inline constexpr Color SurfaceElevated = detail::kL.SolidBackgroundFillColorQuarternary;

        // Colores base (Dark theme)
        inline constexpr Color BackgroundDark      = detail::kD.SolidBackgroundFillColorBase;
        inline constexpr Color SurfaceDark         = detail::kD.SolidBackgroundFillColorSecondary;
        inline constexpr Color SurfaceAltDark      = detail::kD.SolidBackgroundFillColorTertiary;
        inline constexpr Color SurfaceElevatedDark = detail::kD.SolidBackgroundFillColorQuarternary;

        // Colores de acento predefinidos (Parte D los reemplazará; sin cambio aquí)
        inline const Color AccentBlue = Color(0.0f, 0.47f, 0.84f, 1.0f);
        inline const Color AccentGreen = Color(0.16f, 0.69f, 0.32f, 1.0f);
        inline const Color AccentPurple = Color(0.70f, 0.40f, 0.90f, 1.0f);
        inline const Color AccentOrange = Color(1.0f, 0.58f, 0.0f, 1.0f);
        inline const Color AccentPink = Color(0.94f, 0.20f, 0.55f, 1.0f);
        inline const Color AccentTeal = Color(0.0f, 0.68f, 0.70f, 1.0f);

        // Colores de acento (Fluent Blue por defecto)
        inline const Color Accent = AccentBlue;
        inline const Color AccentHover = Color(0.0f, 0.55f, 0.92f, 1.0f);
        inline const Color AccentPressed = Color(0.0f, 0.38f, 0.70f, 1.0f);

        // Colores de texto (tokens WinUI: alpha sobre el fondo, componen correctamente)
        inline constexpr Color TextPrimary       = detail::kL.TextFillColorPrimary;
        inline constexpr Color TextSecondary     = detail::kL.TextFillColorSecondary;
        inline constexpr Color TextTertiary      = detail::kL.TextFillColorTertiary;
        inline constexpr Color TextPrimaryDark   = detail::kD.TextFillColorPrimary;
        inline constexpr Color TextSecondaryDark = detail::kD.TextFillColorSecondary;
        inline constexpr Color TextTertiaryDark  = detail::kD.TextFillColorTertiary;

        // Borders — trazos WinUI (más sutiles que el gris opaco anterior; look Fluent)
        inline constexpr Color Border      = detail::kL.ControlStrokeColorDefault;
        inline constexpr Color BorderDark  = detail::kD.ControlStrokeColorDefault;
        inline constexpr Color BorderLight = detail::kL.CardStrokeColorDefault;

        // Bordes de contenedor — trazo "secondary" (algo más marcado, sigue WinUI)
        inline constexpr Color ContainerBorderDark  = detail::kD.ControlStrokeColorSecondary;
        inline constexpr Color ContainerBorderLight = detail::kL.ControlStrokeColorSecondary;

        // Fondos de contenedor
        inline constexpr Color ContainerBackgroundDark  = detail::kD.SolidBackgroundFillColorSecondary;
        inline constexpr Color ContainerBackgroundLight = detail::kL.SolidBackgroundFillColorSecondary;

        // Estados (señal agnóstica de tema — ver nota de cabecera; theme-aware en ColorTokens)
        inline const Color Disabled = Color(0.7f, 0.7f, 0.7f, 1.0f);
        inline const Color Error = Color(0.95f, 0.26f, 0.21f, 1.0f);
        inline const Color Success = Color(0.16f, 0.69f, 0.32f, 1.0f);
        inline const Color Warning = Color(1.0f, 0.58f, 0.0f, 1.0f);
        inline const Color Info = Color(0.0f, 0.47f, 0.84f, 1.0f);

        // LEGACY (brief 34 Parte D): variación de acento por suma/multiplicación en
        // RGB. Se degrada con acentos muy claros/oscuros (clipping). Ya NO se usa en
        // la construcción de temas — sustituido por MakeAccentPalette (OKLCH) +
        // AccentRolesForTheme. Se conservan solo por compatibilidad de API.
        inline Color GetAccentHover(const Color& accent) {
            float lift = 0.08f;
            return Color(std::min(1.0f, accent.r + lift),
                        std::min(1.0f, accent.g + lift),
                        std::min(1.0f, accent.b + lift), accent.a);
        }

        inline Color GetAccentPressed(const Color& accent) {
            return Color(accent.r * 0.85f, accent.g * 0.85f, accent.b * 0.85f, accent.a);
        }
    } // namespace FluentColors

    // ── Brief 34 Parte D — paleta de acento perceptual ───────────────────────────
    // Siete tonos generados en OKLCH (luminosidad perceptual, cromaticidad/tono
    // estables) a partir de un acento base. Reemplaza el ajuste RGB ingenuo, que se
    // degradaba en los extremos. Índices análogos a SystemAccentColor{Dark,Light}N.
    struct AccentPalette {
        Color dark3, dark2, dark1, base, light1, light2, light3;
    };

    // Genera los 7 tonos desde el acento base (preserva alpha del base).
    AccentPalette MakeAccentPalette(const Color& base);

    // Rol de acento por estado, ya remapeado por tema. WinUI usa el tono CLARO del
    // acento en tema oscuro (Light2) y el OSCURO en tema claro (Dark1); hover/pressed
    // son ese tono a 0.9/0.8 de opacidad (AccentFillColorSecondary/Tertiary).
    struct AccentRoles { Color rest, hover, pressed; };
    AccentRoles AccentRolesForTheme(const AccentPalette& palette, bool darkTheme);

    // Tema Fluent predeterminado
    Style GetDefaultFluentStyle();
    Style GetDarkFluentStyle();
    
    // Crear tema personalizado con color de acento
    Style CreateCustomFluentStyle(const Color& accentColor, bool darkTheme = true);

    // High contrast theme (Phase 6: Accessibility)
    Style GetHighContrastStyle();

    // PokeMotor editor theme — matches the HTML/CSS reference design
    Style GetEditorDarkStyle();

} // namespace FluentUI

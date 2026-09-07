#include "Theme/FluentTheme.h"
#include "Theme/Material.h" // brief 34 Parte B/D: ColorTokens + Tokens::Light/Dark
#include "core/SystemColors.h" // brief 34 Parte F: HighContrast del SO

namespace FluentUI {

    namespace {
        // Brief 34 Parte C/D — compone un token alpha (fg) sobre una base opaca (bg)
        // y devuelve el color OPACO equivalente. Permite derivar superficies del
        // sistema de tokens sin volverlas translúcidas (ni multiplicadores a ojo).
        Color Flatten(const Color& fg, const Color& bg) {
            const float a = fg.a;
            return Color(bg.r * (1.0f - a) + fg.r * a,
                         bg.g * (1.0f - a) + fg.g * a,
                         bg.b * (1.0f - a) + fg.b * a,
                         1.0f);
        }

        // ── OKLab / OKLCH (Björn Ottosson) — base de la paleta de acento (Parte D).
        // Trabajar la luminosidad en un espacio perceptual evita el clipping y la
        // degradación de la variación RGB ingenua con acentos claros/oscuros.
        inline float srgb2lin(float c) {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        inline float lin2srgb(float c) {
            return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }
        struct OKLab { float L, a, b; };
        OKLab RgbToOklab(const Color& c) {
            const float r = srgb2lin(c.r), g = srgb2lin(c.g), bl = srgb2lin(c.b);
            const float l = 0.4122214708f*r + 0.5363325363f*g + 0.0514459929f*bl;
            const float m = 0.2119034982f*r + 0.6806995451f*g + 0.1073969566f*bl;
            const float s = 0.0883024619f*r + 0.2817188376f*g + 0.6299787005f*bl;
            const float l_ = std::cbrt(l), m_ = std::cbrt(m), s_ = std::cbrt(s);
            return { 0.2104542553f*l_ + 0.7936177850f*m_ - 0.0040720468f*s_,
                     1.9779984951f*l_ - 2.4285922050f*m_ + 0.4505937099f*s_,
                     0.0259040371f*l_ + 0.7827717662f*m_ - 0.8086757660f*s_ };
        }
        Color OklabToRgb(const OKLab& c, float alpha) {
            const float l_ = c.L + 0.3963377774f*c.a + 0.2158037573f*c.b;
            const float m_ = c.L - 0.1055613458f*c.a - 0.0638541728f*c.b;
            const float s_ = c.L - 0.0894841775f*c.a - 1.2914855480f*c.b;
            const float l = l_*l_*l_, m = m_*m_*m_, s = s_*s_*s_;
            const float r  =  4.0767416621f*l - 3.3077115913f*m + 0.2309699292f*s;
            const float g  = -1.2684380046f*l + 2.6097574011f*m - 0.3413193965f*s;
            const float bl = -0.0041960863f*l - 0.7034186147f*m + 1.7076147010f*s;
            auto c01 = [](float v){ return std::clamp(v, 0.0f, 1.0f); };
            return Color(c01(lin2srgb(c01(r))), c01(lin2srgb(c01(g))), c01(lin2srgb(c01(bl))), alpha);
        }
        // Desplaza la luminosidad OKLab hacia blanco (t>0) o negro (t<0) por fracción
        // |t| (tono y cromaticidad estables). Preserva el alpha del color base.
        Color ShiftLightness(const Color& base, float t) {
            OKLab o = RgbToOklab(base);
            const float target = t >= 0.0f ? 1.0f : 0.0f;
            o.L = o.L + (target - o.L) * std::fabs(t);
            return OklabToRgb(o, base.a);
        }

        TextStyle MakeTextStyle(float size, FontWeight weight, const Color& color, float lineHeight = 0.0f)
        {
            TextStyle ts;
            ts.fontSize = size;
            ts.weight = weight;
            ts.color = color;
            ts.lineHeight = lineHeight;
            return ts;
        }

        ButtonStyle MakeButtonStyle(const Color& base, const Color& hover, const Color& pressed, const Color& disabledBg, const Color& disabledForeground, const Color& textColor)
        {
            ButtonStyle style;
            style.background.normal = base;
            style.background.hover = hover;
            style.background.pressed = pressed;
            // Brief 34 Parte C/D: disabled = AccentFillColorDisabled (token WinUI real),
            // no la derivación base·0.4 alpha.
            style.background.disabled = disabledBg;

            style.foreground.normal = textColor;
            style.foreground.hover = textColor;
            style.foreground.pressed = textColor;
            style.foreground.disabled = disabledForeground;

            style.border.normal = Color(base.r, base.g, base.b, 0.0f);
            style.border.hover = Color(base.r, base.g, base.b, 0.0f);
            style.border.pressed = Color(base.r, base.g, base.b, 0.0f);
            style.border.disabled = Color(0.0f, 0.0f, 0.0f, 0.0f);

            style.cornerRadius = 6.0f;
            style.padding = Vec2(20.0f, 10.0f);
            style.borderWidth = 0.0f;
            style.shadowOpacity = 0.25f;
            style.shadowOffsetY = 2.0f;
            style.text = MakeTextStyle(16.0f, FontWeight::SemiBold, textColor);
            return style;
        }

        PanelStyle MakePanelStyle(const ColorTokens& t, const AccentRoles& accent)
        {
            PanelStyle panel;
            // Brief 34 Parte B/4: superficie del panel derivada de tokens WinUI, sin
            // los multiplicadores a ojo. La capa (SolidBackgroundFillColorTertiary)
            // queda por encima del fondo base de la ventana: en oscuro es más clara
            // (como Windows Settings), en claro un leve realce — sin calibrar a mano.
            panel.background = t.solidBackgroundTertiary;
            // Cabecera: velo sutil sobre el propio panel, aplanado a opaco.
            panel.headerBackground = Flatten(t.subtleFillSecondary, panel.background);
            panel.borderColor = t.controlStrokeSecondary; // trazo WinUI (borde 0 abajo)
            panel.headerText = MakeTextStyle(16.0f, FontWeight::SemiBold, t.textPrimary);
            // Acento resuelto por tema (Parte D): tono + hover/pressed por opacidad,
            // disabled desde el token AccentFillColorDisabled.
            panel.titleButton.normal = accent.rest;
            panel.titleButton.hover = accent.hover;
            panel.titleButton.pressed = accent.pressed;
            panel.titleButton.disabled = t.accentFillDisabled;

            panel.borderWidth = 0.0f;  // Sin borde visible - solo contraste de fondo
            panel.cornerRadius = 10.0f;
            // Sombra ambiental difusa: opacidad baja + offset pequeño + blur
            // amplio. Un pico alto concentraba el negro en el borde inferior y se
            // leía como una banda dura; bajarlo lo convierte en un halo sutil.
            panel.shadowOpacity = 0.22f;
            panel.shadowOffsetY = 2.0f;
            panel.shadowBlur = 10.0f;
            panel.padding = Vec2(16.0f, 14.0f);
            panel.useAcrylic = false; // Deshabilitar acrylic por defecto para evitar transparencias no deseadas
            panel.acrylicOpacity = 0.85f;
            return panel;
        }

        SeparatorStyle MakeSeparatorStyle(const ColorTokens& t)
        {
            SeparatorStyle separator;
            // Brief 34 Parte 4: divisor = DividerStrokeColorDefault (token WinUI),
            // en vez del gris/blanco al 15% calibrado a mano.
            separator.color = t.dividerStroke;
            separator.thickness = 1.0f;
            separator.padding = 12.0f;
            return separator;
        }

        LabelStyle MakeLabelStyle(const TextStyle& baseText, const ColorTokens& t)
        {
            LabelStyle label;
            label.text = baseText;
            // Brief 34 Parte C/4: color de texto deshabilitado = TextFillColorDisabled
            // (token WinUI explícito), no la derivación color*0.45 alpha.
            label.disabledColor = t.textDisabled;
            return label;
        }

        Style BuildStyle(bool darkTheme)
        {
            Style style;
            style.isDarkTheme = darkTheme;

            // Brief 34 Parte B/4: paleta del tema desde las familias de tokens WinUI.
            const ColorTokens t = darkTheme ? Tokens::Dark() : Tokens::Light();

            style.backgroundColor = t.solidBackgroundBase;
            style.spacing = 10.0f;
            style.padding = 14.0f;

            const Color textPrimary = t.textPrimary;
            const Color textSecondary = t.textSecondary;
            const Color textTertiary = t.textTertiary;

            // Tamaños de fuente según Fluent Design System
            style.typography.caption = MakeTextStyle(13.0f, FontWeight::Regular, textSecondary, 18.0f);
            style.typography.body = MakeTextStyle(14.0f, FontWeight::Regular, textPrimary, 20.0f);
            style.typography.bodyStrong = MakeTextStyle(14.0f, FontWeight::SemiBold, textPrimary, 20.0f);
            style.typography.subtitle = MakeTextStyle(18.0f, FontWeight::Regular, textPrimary, 24.0f);
            style.typography.subtitleStrong = MakeTextStyle(18.0f, FontWeight::SemiBold, textPrimary, 24.0f);
            style.typography.title = MakeTextStyle(20.0f, FontWeight::SemiBold, textPrimary, 28.0f);
            style.typography.titleLarge = MakeTextStyle(28.0f, FontWeight::SemiBold, textPrimary, 36.0f);
            style.typography.display = MakeTextStyle(42.0f, FontWeight::Bold, textPrimary, 52.0f);

            // Brief 34 Parte D: acento por defecto → paleta OKLCH + remapeo por tema.
            const AccentRoles accent = AccentRolesForTheme(MakeAccentPalette(FluentColors::Accent), darkTheme);
            style.accentColor = accent.rest; // tono resuelto que usan sliders/checks/etc.

            // Botón de acento: fills desde los roles; texto y disabled desde tokens
            // *OnAccent* (por tema: negro sobre acento claro / blanco sobre oscuro).
            ButtonStyle buttonStyle = MakeButtonStyle(
                accent.rest, accent.hover, accent.pressed,
                t.accentFillDisabled, t.textOnAccentDisabled, t.textOnAccentPrimary);
            style.button = buttonStyle;

            style.label = MakeLabelStyle(style.typography.body, t);
            style.panel = MakePanelStyle(t, accent);
            style.separator = MakeSeparatorStyle(t);

            // Brief 11: themed drop-shadow tint. Light mode uses a solid black; dark
            // mode keeps black but at a lower alpha so shadows read as a soft halo
            // instead of a hard black smudge over already-dark surfaces.
            style.shadowColor = darkTheme ? Color(0.0f, 0.0f, 0.0f, 0.55f)
                                          : Color(0.0f, 0.0f, 0.0f, 1.0f);

            return style;
        }
    } // namespace

    // Brief 34 Parte D — 7 tonos perceptuales desde el acento base. Los desplazamientos
    // de luminosidad OKLab (±0.16/0.32/0.50 hacia blanco/negro) dan pasos perceptuales
    // uniformes que no se degradan con acentos extremos (a diferencia del RGB ±0.08 / ×0.85).
    AccentPalette MakeAccentPalette(const Color& base) {
        return AccentPalette{
            ShiftLightness(base, -0.50f), // dark3
            ShiftLightness(base, -0.32f), // dark2
            ShiftLightness(base, -0.16f), // dark1
            base,                         // base
            ShiftLightness(base,  0.16f), // light1
            ShiftLightness(base,  0.32f), // light2
            ShiftLightness(base,  0.50f), // light3
        };
    }

    // Remapeo rol→tono por tema (mapeo WinUI: Default = Light2 en oscuro / Dark1 en
    // claro). Hover/pressed = ese tono a 0.9/0.8 de opacidad (compone sobre el fondo).
    AccentRoles AccentRolesForTheme(const AccentPalette& p, bool darkTheme) {
        const Color rest = darkTheme ? p.light2 : p.dark1;
        return AccentRoles{
            rest,
            Color(rest.r, rest.g, rest.b, 0.9f),
            Color(rest.r, rest.g, rest.b, 0.8f),
        };
    }

    Style GetDefaultFluentStyle() {
        return BuildStyle(false);
    }

    Style GetDarkFluentStyle() {
        return BuildStyle(true);
    }

    Style CreateCustomFluentStyle(const Color& accentColor, bool darkTheme) {
        Style style = BuildStyle(darkTheme);
        const ColorTokens t = darkTheme ? Tokens::Dark() : Tokens::Light();

        // Brief 34 Parte D: paleta perceptual desde el acento dado + remapeo por tema.
        // Robusto en extremos: en oscuro el tono resuelto es claro (Light2) y el texto
        // OnAccent es oscuro; en claro, al revés — legible en ambos sin heurística de
        // luminancia. La firma pública se mantiene.
        const AccentRoles accent = AccentRolesForTheme(MakeAccentPalette(accentColor), darkTheme);

        ButtonStyle buttonStyle = MakeButtonStyle(
            accent.rest, accent.hover, accent.pressed,
            t.accentFillDisabled, t.textOnAccentDisabled, t.textOnAccentPrimary);
        // Botones casi planos (estilo Fluent): apenas un velo de sombra para dar
        // un mínimo de elevación, sin la banda oscura inferior que se veía pesada.
        buttonStyle.shadowOpacity = 0.10f;
        buttonStyle.shadowOffsetY = 1.0f;
        buttonStyle.shadowBlur = 6.0f;
        buttonStyle.cornerRadius = 6.0f;
        style.button = buttonStyle;

        // Brand accent (tono resuelto) usado por sliders, checkboxes, progress bars,
        // radios, plots, date pickers, etc. (ctx->style.accentColor).
        style.accentColor = accent.rest;

        // Acento en la barra de título del panel.
        style.panel.titleButton.normal = accent.rest;
        style.panel.titleButton.hover = accent.hover;
        style.panel.titleButton.pressed = accent.pressed;
        style.panel.titleButton.disabled = t.accentFillDisabled;

        return style;
    }

    // Brief 34 Parte F — HighContrast leído del SO. En HC WinUI NO inventa colores:
    // mapea todo a los 8 SystemColor* (GetSysColor). Sin translúcidos, sin acrylic,
    // sin reveal, sin sombras; bordes de 2px. Fuera de Windows usa el fallback de
    // SystemColors (negro/blanco/amarillo), de modo que el build/CI no requiere Win32.
    Style GetHighContrastStyle() {
        const SystemColors sys = SystemColors::Query();
        Style style;
        // El tema base (claro/oscuro) se deduce del fondo real del SO.
        style.isDarkTheme = sys.window.Luminance() < 0.5f;
        style.isHighContrast = true; // desactiva bisel/reveal/acrylic aguas abajo

        style.backgroundColor = sys.window;
        style.accentColor = sys.highlight; // sliders/checks/etc. usan el highlight del SO
        style.spacing = 8.0f;
        style.padding = 12.0f;
        style.shadowColor = Color(0.0f, 0.0f, 0.0f, 0.0f); // sin sombras en HC

        auto makeText = [&](float size, FontWeight w = FontWeight::Regular) -> TextStyle {
            return {size, 0.0f, w, sys.windowText};
        };
        style.typography.caption        = makeText(13.0f);
        style.typography.body           = makeText(15.0f);
        style.typography.bodyStrong     = makeText(15.0f, FontWeight::Bold);
        style.typography.subtitle       = makeText(20.0f);
        style.typography.subtitleStrong = makeText(20.0f, FontWeight::Bold);
        style.typography.title          = makeText(24.0f, FontWeight::Bold);
        style.typography.titleLarge     = makeText(32.0f, FontWeight::Bold);
        style.typography.display        = makeText(48.0f, FontWeight::Bold);

        // Botón: cara/texto del SO; hover/pressed usan el highlight; borde 2px.
        style.button.background = {sys.btnFace, sys.highlight, sys.highlight, sys.btnFace};
        style.button.foreground = {sys.btnText, sys.highlightText, sys.highlightText, sys.grayText};
        style.button.border     = {sys.windowText, sys.highlight, sys.highlight, sys.grayText};
        style.button.padding = Vec2(18.0f, 12.0f);
        style.button.cornerRadius = 4.0f;
        style.button.borderWidth = 2.0f;
        style.button.shadowOpacity = 0.0f;
        style.button.text = makeText(15.0f, FontWeight::Bold);

        style.label.text = makeText(15.0f);
        style.label.disabledColor = sys.grayText;

        // Panel: bordes claros de 2px, sin acrylic ni sombra.
        style.panel.background = sys.window;
        style.panel.headerBackground = sys.btnFace;
        style.panel.borderColor = sys.windowText;
        style.panel.borderWidth = 2.0f;
        style.panel.cornerRadius = 4.0f;
        style.panel.shadowOpacity = 0.0f;
        style.panel.headerText = makeText(15.0f, FontWeight::Bold);
        style.panel.titleButton = {sys.highlight, sys.highlightText, sys.highlightText, sys.grayText};
        style.panel.padding = Vec2(12.0f, 12.0f);
        style.panel.useAcrylic = false;

        style.separator.color = sys.windowText;
        style.separator.thickness = 2.0f;
        style.separator.padding = 8.0f;

        return style;
    }

    Style GetEditorDarkStyle() {
        Style style;
        style.isDarkTheme = true;

        // Increased contrast dark theme — clear visual hierarchy:
        // menubar (darkest) → toolbar → viewport bg → panels (lightest dark)
        Color bg0 = Color::FromHex("#141414");   // viewport/menubar — nearly black
        Color bg1 = Color::FromHex("#1e1e1e");   // toolbar
        Color bg2 = Color::FromHex("#252525");   // panels
        Color bg3 = Color::FromHex("#2e2e2e");   // panel headers, button bg
        Color borderColor = Color::FromHex("#3a3a3a");  // more visible borders
        Color borderSoft = Color::FromHex("#333333");
        Color textMain = Color::FromHex("#e8e8e8");  // slightly brighter
        Color textDim = Color::FromHex("#9a9a9a");
        Color textMuted = Color::FromHex("#6b6b6b");
        Color accent = Color::FromHex("#3b82f6");
        Color accentHover = Color::FromHex("#2563eb");
        Color accentPressed = Color::FromHex("#1d4ed8");

        style.backgroundColor = bg0;
        style.spacing = 10.0f;
        style.padding = 14.0f;
        style.accentColor = accent;
        // sliderFillColor se deja en nullopt → el slider usa accentColor (brand
        // fill Fluent 2). Brief 34 Parte C: ya no se usa el sentinela alpha=0.

        // Typography aligned to Fluent 2 fontSizeBase tokens:
        //   100=10, 200=12 (caption), 300=14 (body), 400=16, 500=20, 600=24
        style.typography.caption    = MakeTextStyle(12.0f, FontWeight::Regular, textDim, 18.0f);
        style.typography.body       = MakeTextStyle(14.0f, FontWeight::Regular, textMain, 20.0f);
        style.typography.bodyStrong = MakeTextStyle(14.0f, FontWeight::SemiBold, textMain, 20.0f);
        style.typography.subtitle   = MakeTextStyle(16.0f, FontWeight::Regular, textMain, 22.0f);
        style.typography.subtitleStrong = MakeTextStyle(16.0f, FontWeight::SemiBold, textMain, 22.0f);
        style.typography.title      = MakeTextStyle(20.0f, FontWeight::SemiBold, textMain, 28.0f);
        style.typography.titleLarge = MakeTextStyle(24.0f, FontWeight::SemiBold, textMain, 32.0f);
        style.typography.display    = MakeTextStyle(36.0f, FontWeight::Bold, textMain, 46.0f);

        // Buttons — subtle, blending with panels; accent for active/CTA
        ButtonStyle btn;
        btn.background.normal  = bg3;
        btn.background.hover   = borderColor;
        btn.background.pressed = bg2;
        btn.background.disabled = Color(bg3.r, bg3.g, bg3.b, 0.4f);
        btn.foreground.normal  = textMain;
        btn.foreground.hover   = textMain;
        btn.foreground.pressed = textMain;
        btn.foreground.disabled = textMuted;
        btn.border.normal  = borderSoft;
        btn.border.hover   = borderColor;
        btn.border.pressed = borderColor;
        btn.border.disabled = Color(0, 0, 0, 0);
        btn.cornerRadius = 4.0f;                    // borderRadiusMedium
        btn.padding = Vec2(12.0f, 5.0f);            // spacingHorizontalM × Fluent Medium V padding
        btn.borderWidth = 1.0f;                     // strokeThin
        btn.shadowOpacity = 0.0f;
        btn.shadowOffsetY = 0.0f;
        btn.text = MakeTextStyle(14.0f, FontWeight::Regular, textMain);  // fontSizeBase300
        style.button = btn;

        // Labels
        style.label.text = MakeTextStyle(14.0f, FontWeight::Regular, textMain);
        style.label.disabledColor = Color(textMain.r, textMain.g, textMain.b, 0.45f);

        // Panels — flat, subtle borders, no heavy shadows
        // Panels are the LIGHTEST dark element to contrast with the dark viewport
        PanelStyle panel;
        panel.background = bg2;
        panel.headerBackground = bg3;
        panel.borderColor = borderSoft;
        panel.borderWidth = 1.0f;
        panel.cornerRadius = 0.0f;   // Flat, docked panels (no rounded corners)
        panel.shadowOpacity = 0.0f;
        panel.shadowOffsetY = 0.0f;
        panel.padding = Vec2(14.0f, 12.0f);
        panel.headerText = MakeTextStyle(12.0f, FontWeight::SemiBold, textMain);
        panel.titleButton.normal = accent;
        panel.titleButton.hover = accentHover;
        panel.titleButton.pressed = accentPressed;
        panel.titleButton.disabled = Color(accent.r, accent.g, accent.b, 0.4f);
        panel.useAcrylic = false;
        panel.acrylicOpacity = 1.0f;
        style.panel = panel;

        // Separator
        style.separator.color = borderSoft;
        style.separator.thickness = 1.0f;
        style.separator.padding = 8.0f;

        return style;
    }

} // namespace FluentUI


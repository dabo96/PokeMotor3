#pragma once
#include "Math/Color.h"

namespace FluentUI {

// Brief 34 Parte F — paleta de colores del sistema (Win32 GetSysColor) para el modo
// HighContrast real. En HighContrast WinUI NO usa colores propios ni opacidades: mapea
// todo a estos 8 colores del SO. Los valores por defecto de abajo son el FALLBACK en
// plataformas no-Windows (negro/blanco/amarillo tipo HC clásico), para que el build y
// el CI en Linux (brief 27) no requieran Win32.
struct SystemColors {
    Color window        = Color(0.0f, 0.0f, 0.0f, 1.0f); // COLOR_WINDOW      (fondo)
    Color windowText    = Color(1.0f, 1.0f, 1.0f, 1.0f); // COLOR_WINDOWTEXT  (texto/bordes)
    Color btnFace       = Color(0.0f, 0.0f, 0.0f, 1.0f); // COLOR_BTNFACE     (cara de control)
    Color btnText       = Color(1.0f, 1.0f, 1.0f, 1.0f); // COLOR_BTNTEXT
    Color highlight     = Color(1.0f, 1.0f, 0.0f, 1.0f); // COLOR_HIGHLIGHT   (selección/acento)
    Color highlightText = Color(0.0f, 0.0f, 0.0f, 1.0f); // COLOR_HIGHLIGHTTEXT
    Color grayText      = Color(0.5f, 0.5f, 0.5f, 1.0f); // COLOR_GRAYTEXT    (deshabilitado)
    Color hotlight      = Color(0.0f, 1.0f, 1.0f, 1.0f); // COLOR_HOTLIGHT    (enlaces)
    bool  highContrast  = false;                         // SPI_GETHIGHCONTRAST activo

    // Consulta al SO. Windows: GetSysColor(...) + SPI_GETHIGHCONTRAST. Otras
    // plataformas: devuelve el fallback de arriba con highContrast=false.
    static SystemColors Query();
};

} // namespace FluentUI

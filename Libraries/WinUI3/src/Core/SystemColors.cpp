// Brief 34 Parte F — consulta de colores del sistema para HighContrast.
#include "core/SystemColors.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace FluentUI {

#if defined(_WIN32)
static Color FromCOLORREF(DWORD c) {
    // COLORREF = 0x00BBGGRR.
    return Color(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f);
}
#endif

SystemColors SystemColors::Query() {
    SystemColors s; // los defaults del struct son el fallback no-Windows
#if defined(_WIN32)
    s.window        = FromCOLORREF(GetSysColor(COLOR_WINDOW));
    s.windowText    = FromCOLORREF(GetSysColor(COLOR_WINDOWTEXT));
    s.btnFace       = FromCOLORREF(GetSysColor(COLOR_BTNFACE));
    s.btnText       = FromCOLORREF(GetSysColor(COLOR_BTNTEXT));
    s.highlight     = FromCOLORREF(GetSysColor(COLOR_HIGHLIGHT));
    s.highlightText = FromCOLORREF(GetSysColor(COLOR_HIGHLIGHTTEXT));
    s.grayText      = FromCOLORREF(GetSysColor(COLOR_GRAYTEXT));
    s.hotlight      = FromCOLORREF(GetSysColor(COLOR_HOTLIGHT));

    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
        s.highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
#endif
    return s;
}

} // namespace FluentUI

#include "UI/Widgets.h"
#include "UI/WidgetHelpers.h"
#include "UI/Icons.h"
#include "core/Context.h"
#include "core/Renderer.h"
#include "Theme/FluentTheme.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace FluentUI {

namespace {

// Zeller-style: returns weekday for given Y/M/D (0 = Sunday).
int DayOfWeek(int year, int month, int day) {
    if (month < 3) { month += 12; year -= 1; }
    int K = year % 100;
    int J = year / 100;
    int h = (day + (13 * (month + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return (h + 6) % 7; // shift so 0 = Sunday
}

bool IsLeapYear(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int DaysInMonth(int year, int month) {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2 && IsLeapYear(year)) return 29;
    return dim[month - 1];
}

const char* MonthName(int m) {
    static const char* names[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    return (m >= 1 && m <= 12) ? names[m - 1] : "?";
}

DateTimeValue Today() {
    DateTimeValue v;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    v.year = lt.tm_year + 1900;
    v.month = lt.tm_mon + 1;
    v.day = lt.tm_mday;
    v.hour = lt.tm_hour;
    v.minute = lt.tm_min;
    v.second = lt.tm_sec;
    return v;
}

bool SmallButton(UIContext* ctx, const std::string& text, const Vec2& pos, const Vec2& size, uint32_t id) {
    Vec2 mp(ctx->input.MouseX(), ctx->input.MouseY());
    bool hover = PointInRect(mp, pos, size);
    bool clicked = hover && ctx->input.IsMousePressed(0);
    Color bg = ctx->style.button.background.normal;
    if (hover) bg = ctx->style.button.background.hover;
    ctx->renderer.DrawRectFilled(pos, size, bg, 4.0f);
    const TextStyle& ts = ctx->style.GetTextStyle(TypographyStyle::Caption);
    Vec2 tsz = MeasureTextCached(ctx, text, ts.fontSize);
    Vec2 tpos(pos.x + (size.x - tsz.x) * 0.5f, pos.y + (size.y - tsz.y) * 0.5f);
    ctx->renderer.DrawText(tpos, text, ts.color, ts.fontSize);
    (void)id;
    return clicked;
}

} // namespace

bool DatePicker(const std::string& label, DateTimeValue* value, std::optional<Vec2> pos) {
    UIContext* ctx = GetContext();
    if (!ctx || !value) return false;

    const TextStyle& titleStyle = ctx->style.GetTextStyle(TypographyStyle::Subtitle);
    const TextStyle& cellStyle = ctx->style.GetTextStyle(TypographyStyle::Caption);
    Color accent = ctx->style.accentColor;

    // Layout: 240×260 box.
    const float W = 240.0f;
    const float HEADER_H = 28.0f;
    const float DOW_H = 18.0f;
    const float CELL_W = W / 7.0f;
    const float CELL_H = 26.0f;
    const float ROWS = 6.0f;
    const float TOTAL_H = HEADER_H + DOW_H + ROWS * CELL_H + 6.0f;

    Vec2 totalSize(W, TOTAL_H);
    LayoutConstraints constraints = ConsumeNextConstraints();
    Vec2 finalSize = ApplyConstraints(ctx, constraints, totalSize);
    Vec2 widgetPos = pos.has_value()
        ? ResolveAbsolutePosition(ctx, pos.value(), finalSize)
        : ctx->cursorPos;

    uint32_t id = GenerateId("DATE:", label.c_str());
    bool changed = false;

    // Background panel
    ctx->renderer.DrawRectFilled(widgetPos, finalSize, ctx->style.panel.background, 6.0f);
    ctx->renderer.DrawRect(widgetPos, finalSize, ctx->style.panel.borderColor, 6.0f);

    // Header: < [Month Year] >
    Vec2 hpos = widgetPos;
    Vec2 prevSz(28.0f, HEADER_H);
    Vec2 nextSz(28.0f, HEADER_H);
    if (SmallButton(ctx, "<", hpos, prevSz, id ^ 0x1)) {
        value->month -= 1;
        if (value->month < 1) { value->month = 12; value->year -= 1; }
        changed = true;
    }
    if (SmallButton(ctx, ">", Vec2(widgetPos.x + W - nextSz.x, hpos.y), nextSz, id ^ 0x2)) {
        value->month += 1;
        if (value->month > 12) { value->month = 1; value->year += 1; }
        changed = true;
    }
    char headerBuf[32];
    std::snprintf(headerBuf, sizeof(headerBuf), "%s %d", MonthName(value->month), value->year);
    Vec2 hSize = MeasureTextCached(ctx, headerBuf, titleStyle.fontSize);
    ctx->renderer.DrawText(Vec2(widgetPos.x + (W - hSize.x) * 0.5f,
                                widgetPos.y + (HEADER_H - hSize.y) * 0.5f),
                           headerBuf, titleStyle.color, titleStyle.fontSize);

    // Day-of-week row
    static const char* dow[] = {"S","M","T","W","T","F","S"};
    float dowY = widgetPos.y + HEADER_H;
    for (int i = 0; i < 7; ++i) {
        Vec2 sz = MeasureTextCached(ctx, dow[i], cellStyle.fontSize);
        Vec2 cp(widgetPos.x + i * CELL_W + (CELL_W - sz.x) * 0.5f,
                dowY + (DOW_H - sz.y) * 0.5f);
        Color c = cellStyle.color; c.a = 0.7f;
        ctx->renderer.DrawText(cp, dow[i], c, cellStyle.fontSize);
    }

    // Day grid
    int firstWeekday = DayOfWeek(value->year, value->month, 1);
    int daysInM = DaysInMonth(value->year, value->month);
    DateTimeValue today = Today();
    float gridY = dowY + DOW_H;
    for (int d = 1; d <= daysInM; ++d) {
        int slot = firstWeekday + d - 1;
        int row = slot / 7;
        int col = slot % 7;
        Vec2 cp(widgetPos.x + col * CELL_W,
                gridY + row * CELL_H);
        Vec2 cs(CELL_W, CELL_H);

        bool isSelected = (d == value->day);
        bool isToday = (today.year == value->year &&
                        today.month == value->month && today.day == d);

        Vec2 mp(ctx->input.MouseX(), ctx->input.MouseY());
        bool hover = PointInRect(mp, cp, cs);
        if (hover && ctx->input.IsMousePressed(0)) {
            value->day = d;
            changed = true;
            isSelected = true;
        }

        if (isSelected) {
            ctx->renderer.DrawRectFilled(cp + Vec2(2,2), cs - Vec2(4,4), accent, 4.0f);
        } else if (hover) {
            Color hb = ctx->style.button.background.hover;
            ctx->renderer.DrawRectFilled(cp + Vec2(2,2), cs - Vec2(4,4), hb, 4.0f);
        } else if (isToday) {
            Color tb = accent; tb.a = 0.4f;
            ctx->renderer.DrawRect(cp + Vec2(2,2), cs - Vec2(4,4), tb, 4.0f);
        }

        char db[8];
        std::snprintf(db, sizeof(db), "%d", d);
        Vec2 ds = MeasureTextCached(ctx, db, cellStyle.fontSize);
        Color dc = isSelected ? Color(1.0f, 1.0f, 1.0f, 1.0f) : cellStyle.color;
        ctx->renderer.DrawText(Vec2(cp.x + (cs.x - ds.x) * 0.5f,
                                    cp.y + (cs.y - ds.y) * 0.5f),
                               db, dc, cellStyle.fontSize);
    }

    // Clamp day to valid range when month changes
    if (value->day > daysInM) value->day = daysInM;
    if (value->day < 1) value->day = 1;

    ctx->lastItemPos = widgetPos;
    AdvanceCursor(ctx, finalSize);
    SetLastItem(id, widgetPos, widgetPos + finalSize, false, false, false, changed);
    return changed;
}

bool TimePicker(const std::string& label, DateTimeValue* value, std::optional<Vec2> pos) {
    UIContext* ctx = GetContext();
    if (!ctx || !value) return false;

    const TextStyle& valStyle = ctx->style.GetTextStyle(TypographyStyle::Body);

    // Cada componente = [campo editable | botones ▲▼]. Separados por ":".
    const float fieldW = 40.0f;
    const float btnW = 18.0f;
    const float gap = 2.0f;
    const float compW = fieldW + gap + btnW;
    const float sepW = 12.0f;
    const float h = 32.0f;
    Vec2 totalSize(compW * 3.0f + sepW * 2.0f, h);
    LayoutConstraints constraints = ConsumeNextConstraints();
    Vec2 finalSize = ApplyConstraints(ctx, constraints, totalSize);
    Vec2 widgetPos = pos.has_value()
        ? ResolveAbsolutePosition(ctx, pos.value(), finalSize)
        : ctx->cursorPos;

    uint32_t id = GenerateId("TIME:", label.c_str());
    bool changed = false;
    // Los TextInput internos mueven el cursor del layout; guardar para restaurar.
    Vec2 savedCursor = ctx->cursorPos;

    // Un componente editable con spinners. `tag` da ids únicos por HH/MM/SS.
    auto Spinner = [&](int* v, int lo, int hi, Vec2 basePos, const char* tag) {
        // TextInput interno con label "##…" → sin header visible, id propio.
        std::string subLabel = "##" + label + tag;
        uint32_t sid = GenerateId("TSPIN:", subLabel.c_str());
        uint32_t txtId = GenerateId("TXT:", subLabel.c_str());
        std::string& buf = ctx->GetWidgetState(sid).stringVal;
        bool& editing = ctx->GetWidgetState(sid).boolVal;

        // Fuera de edición, el buffer refleja el valor (sincronía externa).
        if (!editing) {
            char fb[8]; std::snprintf(fb, sizeof(fb), "%02d", *v);
            buf = fb;
        }

        // Campo editable (permite escribir la hora con el teclado).
        // basePos YA es absoluto (deriva de ctx->cursorPos). TextInput vuelve a
        // pasar su `pos` por ResolveAbsolutePosition, que suma CurrentOffset(ctx).
        // Para no contar el offset del contenedor dos veces (lo que hundía el
        // campo por debajo de los botones al hacer scroll), le pasamos la pos
        // RELATIVA al padre: basePos - CurrentOffset. Ver
        // feedback_resolve_absolute_position_rule.
        Vec2 off = CurrentOffset(ctx);
        Vec2 relPos(basePos.x - off.x, basePos.y - off.y);
        LayoutConstraints fc; fc.width = SizeConstraint::Auto; fc.fixedWidth = fieldW;
        SetNextConstraints(fc);
        SetNextTextInputCenterX();  // HH/MM/SS centrados en X dentro del campo.
        TextInput(subLabel, &buf, fieldW, false, relPos, nullptr, 0);

        Vec2 spinPos(basePos.x + fieldW + gap, basePos.y);
        Vec2 fullRect(compW, h);
        Vec2 mp(ctx->input.MouseX(), ctx->input.MouseY());

        // Commit (Enter / click fuera) → parsear + clamp a [lo,hi].
        bool nowActive = (ctx->activeWidgetId == txtId &&
                          ctx->activeWidgetType == ActiveWidgetType::TextInput);
        bool clickAway = editing && ctx->input.IsMousePressed(0) &&
                         !PointInRect(mp, basePos, fullRect);
        if (nowActive && !clickAway) {
            editing = true;
        } else if (editing) {
            editing = false;
            if (clickAway && ctx->activeWidgetId == txtId) {
                ctx->activeWidgetId = 0;
                ctx->activeWidgetType = ActiveWidgetType::None;
            }
            int parsed = static_cast<int>(std::strtol(buf.c_str(), nullptr, 10));
            parsed = std::clamp(parsed, lo, hi);
            if (parsed != *v) { *v = parsed; changed = true; }
            char fb[8]; std::snprintf(fb, sizeof(fb), "%02d", *v);
            buf = fb;
        }

        // Botones ▲ (arriba) / ▼ (abajo).
        Vec2 upP = spinPos;
        Vec2 upS(btnW, h * 0.5f);
        Vec2 dnP(spinPos.x, spinPos.y + upS.y);
        Vec2 dnS(btnW, h - upS.y);
        auto arrowBtn = [&](Vec2 bp, Vec2 bs, bool isUp) -> bool {
            bool hov = IsMouseOver(ctx, bp, bs);
            ctx->renderer.DrawRectFilled(bp, bs, InputFieldBackground(ctx, hov), 0.0f);
            uint32_t glyph = isUp ? Icons::ChevronUp : Icons::ChevronDown;
            float g = bs.y * 0.9f;
            DrawWidgetIcon(ctx, bp, bs, glyph, valStyle.color, g,
                           (bs.x - g) * 0.5f, 0.0f);
            return hov && ctx->input.IsMousePressed(0);
        };
        bool up = arrowBtn(upP, upS, true);
        bool dn = arrowBtn(dnP, dnS, false);
        ctx->renderer.DrawRect(spinPos, Vec2(btnW, h),
                               ctx->style.panel.borderColor, 0.0f);

        // Subir/bajar con wrap simétrico (23→0, 0→23; igual para 0..59).
        if (up) { *v = (*v >= hi) ? lo : *v + 1; changed = true; editing = false; }
        if (dn) { *v = (*v <= lo) ? hi : *v - 1; changed = true; editing = false; }

        // Rueda del ratón sobre el componente completo.
        if (PointInRect(mp, basePos, fullRect) && ctx->input.MouseWheelY() != 0.0f) {
            int nv = *v + ((ctx->input.MouseWheelY() > 0.0f) ? 1 : -1);
            if (nv < lo) nv = hi;
            if (nv > hi) nv = lo;
            *v = nv;
            changed = true;
        }
    };

    Vec2 cp = widgetPos;
    auto colon = [&](float x) {
        ctx->renderer.DrawText(Vec2(x, cp.y + 8.0f), ":", valStyle.color,
                               valStyle.fontSize);
    };
    Spinner(&value->hour,   0, 23, cp, "h"); colon(cp.x + compW + 3.0f); cp.x += compW + sepW;
    Spinner(&value->minute, 0, 59, cp, "m"); colon(cp.x + compW + 3.0f); cp.x += compW + sepW;
    Spinner(&value->second, 0, 59, cp, "s");

    // Restaurar el cursor (los TextInput internos lo movieron) y avanzar una vez.
    ctx->cursorPos = savedCursor;
    ctx->lastItemPos = widgetPos;
    AdvanceCursor(ctx, finalSize);
    SetLastItem(id, widgetPos, widgetPos + finalSize, false, false, false, changed);
    (void)label;
    return changed;
}

bool DateTimePicker(const std::string& label, DateTimeValue* value, std::optional<Vec2> pos) {
    UIContext* ctx = GetContext();
    if (!ctx || !value) return false;
    bool a = DatePicker(label, value, pos);
    bool b = TimePicker(label, value);
    return a || b;
}

} // namespace FluentUI

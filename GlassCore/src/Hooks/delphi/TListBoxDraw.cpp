// VCL owner-draw: only the HDC and rectangle supplied in WM_DRAWITEM.
#include "CorePch.h"
#include <Hooks/delphi/TListBoxDraw.h>
#include <Hooks/delphi/TBackgroundHooks.h>
#include <Hooks/UxThemeHooks.h>
#include <Subsystems/ThemePaintRegistry.h>
#include <Helpers/ThemeHelper.h>
#include <vssym32.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Subsystems/GdiHookUtils.h>
#include <Subsystems/Settings.h>
#include <Helpers/ColorUtils.h>
#include <BitmapUtils.h>
#include <Providers/MenuContext.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <WinApiUtils.h>
#include <DebugLog.h>
#include <array>
#include <climits>
#include <vector>

#pragma comment(lib, "msimg32.lib")

namespace GlassCore::DelphiListBox
{
    namespace
    {
        using namespace GlassCore::Hooks;
        using namespace std::literals;
        enum : size_t
        {
            FillSlot, ExtTextSlot, DrawTextSlot, RectangleSlot,
            BitBltSlot, StretchBltSlot,
            Count,
        };
        int WINAPI MyFill(HDC, const RECT*, HBRUSH);
        BOOL WINAPI MyExtText(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        int WINAPI MyDrawText(HDC, LPCWSTR, int, LPRECT, UINT);
        BOOL WINAPI MyRectangle(HDC, int, int, int, int);
        BOOL WINAPI MyBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
        BOOL WINAPI MyStretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
        const HookDispatcherDependency<Count, 3> g_dep{{
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "FillRect", reinterpret_cast<void*>(MyFill) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv,
                "ext-ms-win-gdi-desktop-l1-1-0.dll"sv }, "ExtTextOutW", reinterpret_cast<void*>(MyExtText) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "DrawTextW", reinterpret_cast<void*>(MyDrawText) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv,
                "ext-ms-win-gdi-desktop-l1-1-0.dll"sv }, "Rectangle", reinterpret_cast<void*>(MyRectangle) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv,
                "ext-ms-win-gdi-desktop-l1-1-0.dll"sv }, "BitBlt", reinterpret_cast<void*>(MyBitBlt) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv,
                "ext-ms-win-gdi-desktop-l1-1-0.dll"sv }, "StretchBlt", reinterpret_cast<void*>(MyStretchBlt) },
        }};
        HookDispatcher<Count, 3> g_dispatcher{ g_dep, nullptr };
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        struct Session
        {
            HWND hwnd;
            HDC dc;
            RECT item;
            bool colorList;
            bool checkList;
            bool plainList{};
            bool selected{};
            // Keep the supplied full-row brush colour in this draw only.
            // A black text DC alone may be Root's COLOR_WINDOW mapping.
            COLORREF rowBackground{CLR_INVALID};
            // ODT_COMBOBOX fields (TColorBox/TDriveComboBox/TFilterComboBox),
            // or FillRect-only TColorBox popup rows, on the supplied DC area.
            bool combo{};
            bool field{};
            RECT bounds{};
            bool themeDetourAcquired{};
            // Значок уже скомпонован с альфой: следующая монохромная маска
            // ImageList не нужна и вырезала бы готовый значок.
            bool iconAlphaDone{};
            unsigned long long serial{};
            RECT checkboxThemeRect{};
            bool pendingCheckboxBackgroundOutline{};
            std::array<bool, Count> hooks{};
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
            unsigned long long diagId{};
            UINT diagItem{};
            int diagState{};
            RECT diagThemeRect{};
            bool diagHasThemeRect{};
#endif
        };
        thread_local std::vector<Session> g_stack;
        thread_local bool g_inHook = false;
        // Удаление фона значков строк (Root\Control\ListBox|ComboBox). Как
        // ListView/Toolbar: читается один раз на внешнюю сессию; без ключа
        // работает автоматический ключ по фону значка.
        thread_local WndCompat::Menu::IconBackgroundColorRemoval g_iconRemoval{};
        thread_local bool g_inCheckTheme = false;
        thread_local unsigned long long g_sessionSerial = 0;

#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        thread_local unsigned long long g_diagSequence = 0;
        thread_local ULONGLONG g_diagEpoch = 0;
        thread_local unsigned g_diagBudget = 0;
        bool CheckProbeBudget() noexcept;
        void LogComboFill(const Session& s, HDC dc, const RECT& r, const LOGBRUSH& brush,
            const wchar_t* branch) noexcept
        {
            if (!s.combo || !CheckProbeBudget()) return;
            const DWORD lastError = GetLastError();
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiComboDraw] fill hwnd=0x%p dc=0x%p dcType=%u field=%d rect=[%ld,%ld,%ld,%ld] bounds=[%ld,%ld,%ld,%ld] brush=0x%08X branch=%s",
                s.hwnd, dc, static_cast<unsigned>(GetObjectType(dc)), s.field ? 1 : 0,
                r.left, r.top, r.right, r.bottom, s.bounds.left, s.bounds.top,
                s.bounds.right, s.bounds.bottom, static_cast<unsigned>(brush.lbColor), branch);
            SetLastError(lastError);
        }
        void LogComboText(const Session& s, HDC dc, UINT options, UINT count,
            const RECT* r, COLORREF bk, const wchar_t* branch) noexcept
        {
            if (!s.combo || !CheckProbeBudget()) return;
            const DWORD lastError = GetLastError();
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiComboDraw] text hwnd=0x%p dc=0x%p dcType=%u options=0x%X count=%u bk=0x%08X rect=[%ld,%ld,%ld,%ld] branch=%s",
                s.hwnd, dc, static_cast<unsigned>(GetObjectType(dc)), options, count,
                static_cast<unsigned>(bk), r ? r->left : 0, r ? r->top : 0,
                r ? r->right : 0, r ? r->bottom : 0, branch);
            SetLastError(lastError);
        }
        bool CheckProbeBudget() noexcept
        {
            const ULONGLONG now = GetTickCount64();
            if (!g_diagEpoch || now - g_diagEpoch >= 1000) { g_diagEpoch = now; g_diagBudget = 96; }
            if (!g_diagBudget) return false;
            --g_diagBudget;
            return true;
        }
        void LogCheckPixels(const Session& s, const wchar_t* stage, const RECT& r, int state) noexcept
        {
            if (!s.checkList || !s.dc || !CheckProbeBudget()) return;
            const long long width = static_cast<long long>(r.right) - r.left;
            const long long height = static_cast<long long>(r.bottom) - r.top;
            if (width < 2 || height < 2 || width > 256 || height > 256 || r.left == LONG_MIN) return;
            const DWORD lastError = GetLastError();
            GdiFlush();
            // A few RGB samples, not a screenshot or text. CLR_INVALID means the
            // DC could not be read. GetPixel cannot report destination alpha.
            const POINT points[]{ {r.left - 1, r.top + static_cast<LONG>(height / 2)},
                {r.left, r.top}, {r.left + 1, r.top + 1},
                {r.left + static_cast<LONG>(width / 2), r.top + static_cast<LONG>(height / 2)},
                {r.right - 1, r.bottom - 1}, {r.right, r.top + static_cast<LONG>(height / 2)} };
            COLORREF pixels[_countof(points)]{};
            for (size_t i = 0; i < _countof(points); ++i)
                pixels[i] = PtInRect(&s.item, points[i]) ? GetPixel(s.dc, points[i].x, points[i].y) : CLR_INVALID;
            RECT clip{};
            const int clipKind = GetClipBox(s.dc, &clip);
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CheckBgProbe] stage=%s id=%llu hwnd=0x%p item=%u state=%d dc=0x%p dcType=%u rect=[%ld,%ld,%ld,%ld] clipKind=%d rgbOutsideLeft=0x%08X rgbTL=0x%08X rgbInner=0x%08X rgbCenter=0x%08X rgbBR=0x%08X rgbOutsideRight=0x%08X",
                stage, s.diagId, s.hwnd, s.diagItem, state, s.dc, static_cast<unsigned>(GetObjectType(s.dc)),
                r.left, r.top, r.right, r.bottom, clipKind, static_cast<unsigned>(pixels[0]),
                static_cast<unsigned>(pixels[1]), static_cast<unsigned>(pixels[2]),
                static_cast<unsigned>(pixels[3]), static_cast<unsigned>(pixels[4]), static_cast<unsigned>(pixels[5]));
            SetLastError(lastError);
        }
        void LogCheckBuffer(const Session& s, HPAINTBUFFER buffer, const RECT& r, int state) noexcept
        {
            if (!s.checkList || !CheckProbeBudget()) return;
            const DWORD lastError = GetLastError();
            const long long width = static_cast<long long>(r.right) - r.left;
            const long long height = static_cast<long long>(r.bottom) - r.top;
            RGBQUAD* bits = nullptr;
            int stride = 0;
            const HRESULT bitsResult = GetBufferedPaintBits(buffer, &bits, &stride);
            unsigned a0 = 0, a255 = 0, apart = 0, white = 0;
            if (SUCCEEDED(bitsResult) && bits && width > 0 && height > 0
                && width <= 256 && height <= 256 && stride >= width)
            {
                GdiFlush();
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x)
                    {
                        const RGBQUAD& p = bits[static_cast<size_t>(y) * stride + x];
                        if (p.rgbReserved == 0) ++a0;
                        else if (p.rgbReserved == 255) ++a255;
                        else ++apart;
                        if (p.rgbReserved != 0 && p.rgbRed >= 240 && p.rgbGreen >= 240 && p.rgbBlue >= 240) ++white;
                    }
            }
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CheckBgProbe] stage=theme.buffer id=%llu hwnd=0x%p item=%u state=%d bitsHr=0x%08X available=%d size=%lldx%lld stride=%d alpha0=%u alpha255=%u alphaPartial=%u whiteNonzeroAlpha=%u",
                s.diagId, s.hwnd, s.diagItem, state, static_cast<unsigned>(bitsResult), bits ? 1 : 0,
                width, height, stride, a0, a255, apart, white);
            SetLastError(lastError);
        }
        void LogCheckFill(const Session& s, HDC dc, const RECT& r, const LOGBRUSH& brush,
            const wchar_t* branch, BOOL result) noexcept
        {
            if (!s.checkList || !CheckProbeBudget()) return;
            const DWORD lastError = GetLastError();
            RECT clip{};
            const int clipKind = GetClipBox(dc, &clip);
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CheckBgProbe] stage=fill id=%llu hwnd=0x%p item=%u dc=0x%p rect=[%ld,%ld,%ld,%ld] brushStyle=%u brushColor=0x%08X clipKind=%d branch=%s result=%d",
                s.diagId, s.hwnd, s.diagItem, dc, r.left, r.top, r.right, r.bottom,
                static_cast<unsigned>(brush.lbStyle), static_cast<unsigned>(brush.lbColor), clipKind, branch, result);
            SetLastError(lastError);
        }
#endif

        template <size_t index, typename T> T Original() noexcept
        {
            auto lock = g_lock.lock_shared();
            return g_dispatcher.GetOrg<index, T>();
        }
        const Session* Current(HDC dc, bool comboRowFill = false) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || g_inHook || g_stack.empty()) return nullptr;
            const auto& s = g_stack.back();
            // A TColorBox popup row owns only FillRect. Text/image hooks may
            // still be armed by an outer session; never apply the field path.
            if (s.combo && !s.field && !comboRowFill) return nullptr;
            return dc && dc == s.dc ? &s : nullptr;
        }
        bool SystemBackground(COLORREF c) noexcept
        {
            c &= 0xFFFFFFu;
            return c == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)
                || c == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);
        }
        BYTE BackgroundAlpha(const Session& s, COLORREF color) noexcept
        {
            // A100 is a background rule for exact TListBox, not for selection,
            // swatches, other list classes, glyphs or a final client buffer.
            return s.plainList && !s.selected
                && (color & 0xFFFFFFu) != (GetSysColor(COLOR_HIGHLIGHT) & 0xFFFFFFu)
                ? 100 : 255;
        }
        void RememberRowBackground(const Session& s, const RECT& r, COLORREF color, int result) noexcept
        {
            if (result && s.plainList && EqualRect(&r, &s.item)
                && !g_stack.empty() && g_stack.back().serial == s.serial)
                g_stack.back().rowBackground = color & 0xFFFFFFu;
        }
        bool IsColorSample(const Session& s, const RECT& r) noexcept
        {
            const LONG w = s.item.right - s.item.left;
            const LONG h = s.item.bottom - s.item.top;
            // Separate inset swatch, never confuse it with the row background.
            return s.colorList && w > 0 && h > 2 && r.left > s.item.left
                && r.left - s.item.left <= h && r.right > r.left
                && r.right - r.left <= w / 3 && r.top > s.item.top
                && r.bottom < s.item.bottom && r.bottom > r.top;
        }
        bool ComboFillBounds(const Session& s, const RECT& r) noexcept
        {
            return EqualRect(&r, &s.bounds) || EqualRect(&r, &s.item);
        }
        BOOL WINAPI MyRectangle(HDC dc, int left, int top, int right, int bottom)
        {
            auto orig = Original<RectangleSlot, decltype(&::Rectangle)>();
            if (!orig) return Rectangle(dc, left, top, right, bottom);
            const auto* current = Current(dc);
            if (!current || !current->checkList || !current->pendingCheckboxBackgroundOutline)
                return orig(dc, left, top, right, bottom);
            const Session session = *current;
            // Consume on the first Rectangle after the themed checkbox. A later
            // user-drawn rectangle must not inherit this operation's context.
            g_stack.back().pendingCheckboxBackgroundOutline = false;
            const RECT r{left, top, right, bottom};
            LOGBRUSH brush{};
            LOGPEN pen{};
            if (!EqualRect(&r, &session.checkboxThemeRect)
                || GetObjectW(GetCurrentObject(dc, OBJ_BRUSH), sizeof(brush), &brush) != sizeof(brush)
                || brush.lbStyle != BS_NULL
                || GetObjectW(GetCurrentObject(dc, OBJ_PEN), sizeof(pen), &pen) != sizeof(pen)
                || pen.lopnStyle != PS_SOLID || pen.lopnWidth.x != 1
                || (pen.lopnColor & 0xFFFFFFu) != (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu))
                return orig(dc, left, top, right, bottom);

            // VCL paints its background-coloured outline over the theme. Give
            // that background the same transparent black used for list fills;
            // keep the original Rectangle, brush, clip, and themed inner border.
            HGDIOBJ backgroundPen = GetStockObject(BLACK_PEN);
            if (!backgroundPen) return orig(dc, left, top, right, bottom);
            HGDIOBJ oldPen = SelectObject(dc, backgroundPen);
            if (!oldPen || oldPen == HGDI_ERROR) return orig(dc, left, top, right, bottom);
            const BOOL result = orig(dc, left, top, right, bottom);
            const DWORD error = GetLastError();
            SelectObject(dc, oldPen);
#if defined(_DEBUG)
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
            if (budget) { --budget; DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiListDraw] checkbox background pen hwnd=0x%p dc=0x%p result=%d", session.hwnd, dc, result); }
#endif
            SetLastError(error);
            return result;
        }
        // ── Значки строк списка и поля комбо ────────────────────────────────
        // VCL рисует значок ImageList двумя проходами: SRCCOPY цветного
        // битмапа (фон значка непрозрачный — в наблюдаемом пути белый), затем
        // маска (1bpp/4bpp либо ROP 0x00E20746). На прозрачном фоне color key
        // первый проход оставляет белый квадрат вокруг значка.
        // Заменяем эту пару одним GdiAlphaBlend с вырезанным фоном; после
        // композиции второй проход (маска) пропускается — иначе он вырежет
        // уже готовый значок.
        enum class IconBlit { PassThrough, Done, Skip };
        enum class IconBackgroundKind { Unknown, Flat };

        struct IconPixels
        {
            DWORD* data{};
            int    width{};
            int    height{};
            bool   topDown{};
        };

        // Пиксели 32bpp DIBSECTION (результат Promise32BPP): шаг строки в
        // пикселях равен ширине, потому что 32bpp уже выровнен по 4 байтам.
        bool GetIconPixels(HBITMAP bmp32, IconPixels& out) noexcept
        {
            DIBSECTION ds{};
            if (!bmp32 || !GetObject(bmp32, sizeof(ds), &ds) || !ds.dsBm.bmBits
                || ds.dsBm.bmBitsPixel != 32)
                return false;
            out.data = static_cast<DWORD*>(ds.dsBm.bmBits);
            out.width = ds.dsBm.bmWidth;
            out.height = ds.dsBm.bmHeight < 0 ? -ds.dsBm.bmHeight : ds.dsBm.bmHeight;
            out.topDown = ds.dsBm.bmHeight < 0;
            return out.width > 0 && out.height > 0;
        }

        DWORD* IconRow(const IconPixels& px, int y) noexcept
        {
            const int row = px.topDown ? y : (px.height - 1 - y);
            return px.data + static_cast<size_t>(row) * static_cast<size_t>(px.width);
        }

        COLORREF IconRgb(DWORD pixel) noexcept
        {
            return static_cast<COLORREF>(pixel & 0xFFFFFFu);
        }

        int ChannelDelta(BYTE a, BYTE b) noexcept
        {
            return a > b ? static_cast<int>(a) - static_cast<int>(b)
                         : static_cast<int>(b) - static_cast<int>(a);
        }

        bool ColorNear(COLORREF a, COLORREF b, BYTE threshold) noexcept
        {
            return ChannelDelta(GetRValue(a), GetRValue(b)) <= threshold
                && ChannelDelta(GetGValue(a), GetGValue(b)) <= threshold
                && ChannelDelta(GetBValue(a), GetBValue(b)) <= threshold;
        }

        // Фон значка = цвет, одинаковый во всех четырёх углах (подложка).
        // У сплошной заливки/образца цвета фон совпадает со всем остальным,
        // вырез съест её целиком, и вызов уйдёт в оригинал (HasOpaquePixels).
        IconBackgroundKind ClassifyIconBackground(HBITMAP bmp32, COLORREF& bg) noexcept
        {
            IconPixels px{};
            if (!GetIconPixels(bmp32, px) || px.width < 3 || px.height < 3)
                return IconBackgroundKind::Unknown;
            const DWORD* first = IconRow(px, 0);
            const DWORD* last = IconRow(px, px.height - 1);
            const COLORREF corner = IconRgb(first[0]);
            if (corner != IconRgb(first[px.width - 1]) || corner != IconRgb(last[0])
                || corner != IconRgb(last[px.width - 1]))
                return IconBackgroundKind::Unknown;
            bg = corner;
            return IconBackgroundKind::Flat;
        }

        // Вырез фона «заливкой от края»: удаляются только пиксели фона,
        // связанные с рамкой битмапа. Ключ по цвету съедал и белые области
        // внутри значка (страница в папке) — они становились прозрачными.
        // Возвращает false, если удалять нечего (сплошная заливка/образец).
        bool CutIconBackground(HBITMAP bmp32, COLORREF bg, BYTE threshold) noexcept
        {
            IconPixels px{};
            if (!GetIconPixels(bmp32, px))
                return false;
            const int w = px.width;
            const int h = px.height;
            const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
            if (count < 9 || count > 128 * 128)
                return false;
            std::vector<unsigned char> cut(count, 0);
            std::vector<int> queue;
            queue.reserve(count);
            const auto matches = [&](int x, int y) noexcept {
                return ColorNear(IconRgb(IconRow(px, y)[x]), bg, threshold);
            };
            const auto push = [&](int x, int y) noexcept {
                if (x < 0 || y < 0 || x >= w || y >= h)
                    return;
                const size_t index = static_cast<size_t>(y) * static_cast<size_t>(w) + x;
                if (cut[index] || !matches(x, y))
                    return;
                cut[index] = 1;
                queue.push_back(static_cast<int>(index));
            };
            for (int x = 0; x < w; ++x)
            {
                push(x, 0);
                push(x, h - 1);
            }
            for (int y = 0; y < h; ++y)
            {
                push(0, y);
                push(w - 1, y);
            }
            for (size_t head = 0; head < queue.size(); ++head)
            {
                const int index = queue[head];
                const int x = index % w;
                const int y = index / w;
                push(x - 1, y);
                push(x + 1, y);
                push(x, y - 1);
                push(x, y + 1);
            }
            size_t removed = 0;
            for (int y = 0; y < h; ++y)
            {
                DWORD* row = IconRow(px, y);
                for (int x = 0; x < w; ++x)
                {
                    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(w) + x;
                    if (cut[index])
                    {
                        row[x] = 0;
                        ++removed;
                    }
                    else
                        row[x] |= 0xFF000000u; // 4-й байт не используется — непрозрачно
                }
            }
            return removed > 0;
        }

        // У VCL-битмапов 4-й байт не используется (мусор или нули), поэтому
        // пиксель значка с alpha==0 и цветом, отличным от фона, делаем
        // непрозрачным: иначе GdiAlphaBlend рисует его как «ничего».
        void RestoreGlyphAlpha(HBITMAP bmp32, COLORREF bg, BYTE threshold) noexcept
        {
            IconPixels px{};
            if (!GetIconPixels(bmp32, px))
                return;
            for (int y = 0; y < px.height; ++y)
            {
                DWORD* row = IconRow(px, y);
                for (int x = 0; x < px.width; ++x)
                    if ((row[x] & 0xFF000000u) == 0 && !ColorNear(IconRgb(row[x]), bg, threshold))
                        row[x] |= 0xFF000000u;
            }
        }

        struct AlphaStats
        {
            int transparent{};
            int opaque{};
        };

        AlphaStats ScanIconAlpha(HBITMAP bmp32) noexcept
        {
            AlphaStats stats{};
            IconPixels px{};
            if (!GetIconPixels(bmp32, px))
                return stats;
            for (int y = 0; y < px.height; ++y)
            {
                const DWORD* row = IconRow(px, y);
                for (int x = 0; x < px.width; ++x)
                {
                    const DWORD a = row[x] >> 24;
                    if (a == 0)
                        ++stats.transparent;
                    else if (a == 0xFF)
                        ++stats.opaque;
                }
            }
            return stats;
        }

        bool HasOpaquePixels(HBITMAP bmp32) noexcept
        {
            IconPixels px{};
            if (!GetIconPixels(bmp32, px))
                return false;
            for (int y = 0; y < px.height; ++y)
            {
                const DWORD* row = IconRow(px, y);
                for (int x = 0; x < px.width; ++x)
                    if (row[x] & 0xFF000000u)
                        return true;
            }
            return false;
        }

        // Подготовка значка к GdiAlphaBlend. false — рисовать нечего: это
        // образец цвета/плитка либо после вырезания не осталось непрозрачных
        // пикселей (лучше оставить оригинальный вызов, чем потерять значок).
        bool PrepareItemIcon(HBITMAP bmp32, const wchar_t*& branch, COLORREF& bg) noexcept
        {
            const AlphaStats alpha = ScanIconAlpha(bmp32);
            bool result = false;
            // Явная настройка (Root\\Control\\ListBox|ComboBox\\ColorTreatAsTransparent)
            // перекрывает автоматику — как у ListView/Toolbar/Button. Это ручной
            // выбор цвета, поэтому вырезается весь цвет, а не только край.
            if (g_iconRemoval.enable)
            {
                bg = Shared::ColorUtils::ARGBToCOLORREF(
                    g_iconRemoval.colorTreatAsTransparent);
                const BYTE threshold =
                    static_cast<BYTE>(g_iconRemoval.colorTreatAsTransparentThreshold);
                Shared::BitmapUtils::SpriteEffect(bg, threshold).Apply(bmp32);
                RestoreGlyphAlpha(bmp32, bg, threshold);
                branch = L"custom";
                result = HasOpaquePixels(bmp32);
            }
            // Настоящая альфа значка: есть и полностью прозрачные, и полностью
            // непрозрачные пиксели. Тогда 4-й байт осмысленный — не трогаем.
            else if (alpha.transparent > 0 && alpha.opaque > 0)
            {
                branch = L"alpha";
                result = HasOpaquePixels(bmp32);
            }
            else if (ClassifyIconBackground(bmp32, bg) == IconBackgroundKind::Flat
                && CutIconBackground(bmp32, bg, 10))
            {
                // Фон вырезан заливкой от края; всё остальное непрозрачно.
                branch = L"flood";
                result = HasOpaquePixels(bmp32);
            }
            else
            {
                bg = RGB(0, 0, 0);
                GlassCore::GdiUtils::ApplyBlackKeyAlpha(bmp32);
                RestoreGlyphAlpha(bmp32, bg, 0);
                branch = L"blackkey";
                result = HasOpaquePixels(bmp32);
            }
#if defined(_DEBUG)
            // Цвет фона и статистика: по ним видно, «синий фон» — это остаток
            // значка или стекло под прозрачной подложкой.
            {
                static thread_local ULONGLONG epoch = 0;
                static thread_local unsigned budget = 0;
                const ULONGLONG now = GetTickCount64();
                if (!epoch || now - epoch >= 1000) { epoch = now; budget = 16; }
                if (budget)
                {
                    --budget;
                    IconPixels px{};
                    const bool havePixels = GetIconPixels(bmp32, px);
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][DelphiListDraw] icon-prepare branch=%s bg=0x%08X size=%dx%d alpha0=%d alpha255=%d draw=%d",
                        branch, static_cast<unsigned>(bg),
                        havePixels ? px.width : 0, havePixels ? px.height : 0,
                        alpha.transparent, alpha.opaque, result ? 1 : 0);
                }
            }
#endif
            return result;
        }

#if defined(_DEBUG)
        // Ранние выходы значка видны в журнале: иначе «иконка не изменилась»
        // невозможно отличить от «наш слот не вызван».
        void IconDiag(const Session& session, const wchar_t* reason, int x, int y,
            int cx, int cy, LONG bpp, DWORD rop) noexcept
        {
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 16; }
            if (!budget)
                return;
            --budget;
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiListDraw] icon-bypass hwnd=0x%p reason=%s dst=[%d,%d,%d,%d] bpp=%ld rop=0x%08X",
                session.hwnd, reason, x, y, cx, cy, bpp, static_cast<unsigned>(rop));
        }
#else
        void IconDiag(const Session&, const wchar_t*, int, int, int, int, LONG, DWORD) noexcept {}
#endif

        IconBlit BlitListItemIcon(const Session& session, HDC dst, int x, int y, int cx, int cy,
            HDC src, int sx, int sy, int scx, int scy, DWORD rop) noexcept
        {
            if (!cx || !cy || !src || !scx || !scy)
            {
                IconDiag(session, L"empty", x, y, cx, cy, 0, rop);
                return IconBlit::PassThrough;
            }
            const HBITMAP source = static_cast<HBITMAP>(GetCurrentObject(src, OBJ_BITMAP));
            if (!source || Shared::BitmapUtils::IsOemBitmap(source))
                return IconBlit::PassThrough;
            BITMAP bm{};
            if (!GetObject(source, sizeof(bm), &bm))
                return IconBlit::PassThrough;
            // Маска ImageList — всегда 1bpp (монохром). 4bpp/8bpp здесь может
            // быть цветным значком в 16/256 цветах, его нельзя принимать за
            // маску: иначе он уходит в оригинал и остаётся белым квадратом.
            const bool maskSource = bm.bmBitsPixel == 1;
            const bool maskRop = rop == 0x00E20746 || rop == 0x00220326
                || rop == SRCAND || rop == SRCINVERT;
            if (maskSource || maskRop)
            {
                if (!session.iconAlphaDone)
                {
                    IconDiag(session, L"mask-passthrough", x, y, cx, cy, bm.bmBitsPixel, rop);
                    return IconBlit::PassThrough;
                }
                g_stack.back().iconAlphaDone = false;
                return IconBlit::Skip;
            }
            if (rop != SRCCOPY)
            {
                IconDiag(session, L"rop", x, y, cx, cy, bm.bmBitsPixel, rop);
                return IconBlit::PassThrough;
            }
            // Значок — небольшое изображение внутри строки, а не её фон и не
            // посторонняя графика (скроллбар, рамка).
            if (cx > 128 || cy > 128)
            {
                IconDiag(session, L"size", x, y, cx, cy, bm.bmBitsPixel, rop);
                return IconBlit::PassThrough;
            }
            if (x <= session.item.left && y <= session.item.top
                && x + cx >= session.item.right && y + cy >= session.item.bottom)
            {
                IconDiag(session, L"whole-item", x, y, cx, cy, bm.bmBitsPixel, rop);
                return IconBlit::PassThrough;
            }
            const POINT center{ x + cx / 2, y + cy / 2 };
            if (!PtInRect(&session.item, center))
            {
                IconDiag(session, L"outside-item", x, y, cx, cy, bm.bmBitsPixel, rop);
                return IconBlit::PassThrough;
            }

            wil::unique_hbitmap converted{ Shared::BitmapUtils::Promise32BPP(source) };
            if (!converted)
                return IconBlit::PassThrough;
            const wchar_t* branch = L"";
            COLORREF iconBg{};
            if (!PrepareItemIcon(converted.get(), branch, iconBg))
            {
#if defined(_DEBUG)
                static thread_local ULONGLONG skipEpoch = 0;
                static thread_local unsigned skipBudget = 0;
                const ULONGLONG skipNow = GetTickCount64();
                if (!skipEpoch || skipNow - skipEpoch >= 1000) { skipEpoch = skipNow; skipBudget = 16; }
                if (skipBudget)
                {
                    --skipBudget;
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][DelphiListDraw] icon-skip hwnd=0x%p dst=[%d,%d,%d,%d] branch=%s srcBpp=%ld",
                        session.hwnd, x, y, cx, cy, branch, bm.bmBitsPixel);
                }
#endif
                return IconBlit::PassThrough;
            }
            const HGDIOBJ oldBitmap = SelectObject(src, converted.get());
            auto restore = wil::scope_exit([&]() noexcept {
                if (oldBitmap && oldBitmap != HGDI_ERROR)
                    SelectObject(src, oldBitmap);
            });

            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            const bool blended = GdiAlphaBlend(dst, x, y, cx, cy, src, sx, sy, scx, scy, bf) != FALSE;
            if (!blended)
                return IconBlit::PassThrough;
            g_stack.back().iconAlphaDone = true;
#if defined(_DEBUG)
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 24; }
            if (budget)
            {
                --budget;
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiListDraw] icon hwnd=0x%p combo=%d dst=[%d,%d,%d,%d] srcBpp=%ld branch=%s",
                    session.hwnd, session.combo ? 1 : 0, x, y, cx, cy,
                    bm.bmBitsPixel, branch);
            }
#endif
            return IconBlit::Done;
        }

        BOOL WINAPI MyBitBlt(HDC dc, int x, int y, int cx, int cy,
            HDC src, int sx, int sy, DWORD rop)
        {
            auto orig = Original<BitBltSlot, decltype(&::BitBlt)>();
            if (!orig) return BitBlt(dc, x, y, cx, cy, src, sx, sy, rop);
            const auto* current = Current(dc);
            if (!current) return orig(dc, x, y, cx, cy, src, sx, sy, rop);
            const IconBlit result = [&]() noexcept {
                g_inHook = true;
                auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
                return BlitListItemIcon(*current, dc, x, y, cx, cy, src, sx, sy, cx, cy, rop);
            }();
            if (result != IconBlit::PassThrough)
                return TRUE;
            return orig(dc, x, y, cx, cy, src, sx, sy, rop);
        }

        BOOL WINAPI MyStretchBlt(HDC dc, int x, int y, int cx, int cy,
            HDC src, int sx, int sy, int scx, int scy, DWORD rop)
        {
            auto orig = Original<StretchBltSlot, decltype(&::StretchBlt)>();
            if (!orig) return StretchBlt(dc, x, y, cx, cy, src, sx, sy, scx, scy, rop);
            const auto* current = Current(dc);
            if (!current) return orig(dc, x, y, cx, cy, src, sx, sy, scx, scy, rop);
            const IconBlit result = [&]() noexcept {
                g_inHook = true;
                auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
                return BlitListItemIcon(*current, dc, x, y, cx, cy, src, sx, sy, scx, scy, rop);
            }();
            if (result != IconBlit::PassThrough)
                return TRUE;
            return orig(dc, x, y, cx, cy, src, sx, sy, scx, scy, rop);
        }

        int WINAPI MyFill(HDC dc, const RECT* r, HBRUSH brush)
        {
            const DWORD entryError = GetLastError();
            auto orig = Original<FillSlot, decltype(&::FillRect)>();
            if (!orig) return FillRect(dc, r, brush);
            const auto* current = Current(dc, true);
            if (!current || !r || !brush) return orig(dc, r, brush);
            const Session s = *current;
            RECT clipped{};
            if (!IntersectRect(&clipped, r, &s.bounds)) return orig(dc, r, brush);
            // Do not change a call crossing the item boundary.
            if (!EqualRect(&clipped, r)) return orig(dc, r, brush);
            LOGBRUSH lb{};
            if (GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID)
                return orig(dc, r, brush);
            if (s.combo && !s.field)
            {
                // Popup row, not the closed field: only an exact, unselected
                // full-row system fill is background. All inset samples keep
                // their RGB/A255, including white, black and system colours.
                g_inHook = true;
                auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
                if (!s.selected && EqualRect(r, &s.item) && SystemBackground(lb.lbColor))
                {
                    SetLastError(entryError);
                    const int result = orig(dc, r, brush);
                    const DWORD error = GetLastError();
                    // Native failure must not become a successful clear. Keep
                    // its result/error even if the optional clear itself fails.
                    const long long width = static_cast<long long>(r->right) - r->left;
                    const long long height = static_cast<long long>(r->bottom) - r->top;
                    const BOOL cleared = result && width > 0 && width <= INT_MAX
                        && height > 0 && height <= INT_MAX && PatBlt(dc, r->left, r->top,
                            static_cast<int>(width), static_cast<int>(height), BLACKNESS);
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
                    LogComboFill(s, dc, *r, lb, cleared ? L"row-system-background" : L"row-native-or-clear-failed");
#endif
                    (void)cleared;
                    SetLastError(error);
                    return result;
                }
                wil::unique_hbrush opaque{
                    Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255) };
                SetLastError(entryError);
                const int result = opaque ? orig(dc, r, opaque.get()) : orig(dc, r, brush);
                const DWORD error = GetLastError();
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
                LogComboFill(s, dc, *r, lb, opaque ? L"row-opaque-brush" : L"row-original");
#endif
                opaque.reset();
                SetLastError(error);
                return result;
            }
            if (s.combo)
            {
                // Owner-draw combo field. A system-coloured fill covering the
                // field is what VCL paints when the value is empty: make it the
                // transparent black used for glass. Every other solid fill
                // (swatch, selection bar, inset background) keeps its colour and
                // is drawn with a fully opaque brush, because these fills
                // otherwise rely on the removed white background.
                const RECT& b = s.bounds;
                const long long fieldArea =
                    static_cast<long long>(b.right - b.left) * (b.bottom - b.top);
                const long long fillArea =
                    static_cast<long long>(r->right - r->left) * (r->bottom - r->top);
                const bool fieldBackground = ComboFillBounds(s, *r)
                    && fieldArea > 0 && fillArea * 2 >= fieldArea;
                g_inHook = true;
                auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
                if (SystemBackground(lb.lbColor) && fieldBackground)
                {
                    const BOOL cleared = PatBlt(dc, r->left, r->top,
                        r->right - r->left, r->bottom - r->top, BLACKNESS);
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
                    LogComboFill(s, dc, *r, lb, cleared ? L"system-background" : L"failed");
#endif
                    return cleared ? 1 : orig(dc, r, brush);
                }
                wil::unique_hbrush opaque{
                    Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255) };
                const int result = opaque ? orig(dc, r, opaque.get()) : orig(dc, r, brush);
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
                LogComboFill(s, dc, *r, lb, opaque ? L"opaque-brush" : L"original");
#endif
                return result;
            }
            const bool sample = IsColorSample(s, *r);
            // VCL's own DC clip governs each fill, including checkbox backgrounds.
            // Do not infer or alter checkbox geometry or clipping here.
            g_inHook = true;
            auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
            if (!sample && SystemBackground(lb.lbColor))
            {
                const BOOL cleared = PatBlt(dc, r->left, r->top, r->right-r->left, r->bottom-r->top, BLACKNESS);
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
                LogCheckFill(s, dc, *r, lb, L"system-background", cleared);
#endif
                const int result = cleared ? 1 : orig(dc, r, brush);
                RememberRowBackground(s, *r, lb.lbColor, result);
                return result;
            }
            // A full row is a proven background. Smaller application graphics
            // keep their existing A255, even in an unselected TListBox row.
            const BYTE opacity = EqualRect(r, &s.item) ? BackgroundAlpha(s, lb.lbColor) : 255;
            wil::unique_hbrush alpha{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, opacity) };
            const int result = alpha ? orig(dc, r, alpha.get()) : orig(dc, r, brush);
            RememberRowBackground(s, *r, lb.lbColor, result);
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
            LogCheckFill(s, dc, *r, lb, alpha ? L"alpha-brush" : L"original", result);
#endif
            return result;
        }
        BOOL WINAPI MyExtText(HDC dc, int x, int y, UINT options, const RECT* rect,
            LPCWSTR text, UINT count, const INT* dx)
        {
            auto orig = Original<ExtTextSlot, decltype(&::ExtTextOutW)>();
            if (!orig) return ExtTextOutW(dc, x, y, options, rect, text, count, dx);
            const auto* current = Current(dc);
            if (!current) return orig(dc, x, y, options, rect, text, count, dx);
            if (current->combo)
            {
                // VCL paints the field text with ETO_OPAQUE and a window-colour
                // background, which Root maps to black; that fill would erase
                // the themed selection under the text. Drop only the opaque
                // fill and render the glyphs once through the alpha pipeline.
                const UINT comboOptions = options & ~static_cast<UINT>(ETO_OPAQUE);
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
                const Session s = *current;
                LogComboText(s, dc, options, count, rect, GetBkColor(dc),
                    (options & ETO_OPAQUE) ? L"opaque-stripped" : L"text");
#endif
                if (!count) return TRUE;
                if (!text) return orig(dc, x, y, comboOptions, rect, text, count, dx);
                if (SUCCEEDED(GlassCore::Text::ExtTextOutWithAlpha(dc, x, y, comboOptions,
                        rect, text, count, dx, orig)))
                    return TRUE;
                return orig(dc, x, y, comboOptions, rect, text, count, dx);
            }
            if ((GetTextAlign(dc) & TA_UPDATECP) || (options & ETO_PDY)
                || ((options & ETO_OPAQUE) && !rect))
                return orig(dc, x, y, options, rect, text, count, dx);
            const Session s = *current;
            const int saved = SaveDC(dc);
            if (!saved) return orig(dc, x, y, options, rect, text, count, dx);
            auto restore = wil::scope_exit([&]() noexcept { RestoreDC(dc, saved); });
            if (IntersectClipRect(dc, s.bounds.left, s.bounds.top, s.bounds.right, s.bounds.bottom) == ERROR)
                return orig(dc, x, y, options, rect, text, count, dx);
            g_inHook = true;
            auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
            UINT drawOptions = options;
            if ((options & ETO_OPAQUE) && rect)
            {
                const COLORREF bk = GetBkColor(dc);
                // Keep the old mapped-black fallback unless this row actually
                // supplied a black full-row brush. No HWND/global colour cache.
                const bool confirmedBlack = s.plainList && s.rowBackground == 0;
                if (((bk & 0xFFFFFFu) == 0 && !confirmedBlack) || SystemBackground(bk))
                {
                    if (!PatBlt(dc, rect->left, rect->top, rect->right-rect->left, rect->bottom-rect->top, BLACKNESS))
                        return orig(dc, x, y, options, rect, text, count, dx);
                }
                else
                {
                    wil::unique_hbrush alpha{
                        Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(bk, BackgroundAlpha(s, bk)) };
                    if (!alpha || !FillRect(dc, rect, alpha.get()))
                        return orig(dc, x, y, options, rect, text, count, dx);
                }
                // A later full-row opaque background supersedes the brush
                // colour, but ambient black must never establish its own proof.
                if ((bk & 0xFFFFFFu) != 0) RememberRowBackground(s, *rect, bk, 1);
                drawOptions &= ~static_cast<UINT>(ETO_OPAQUE);
            }
            if (!count) return TRUE;
            if (!text) return orig(dc, x, y, drawOptions, rect, text, count, dx);
            if (SUCCEEDED(GlassCore::Text::ExtTextOutWithAlpha(dc, x, y, drawOptions, rect, text, count, dx, orig)))
                return TRUE;
            return orig(dc, x, y, drawOptions, rect, text, count, dx);
        }
        int WINAPI MyDrawText(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format)
        {
            auto orig = Original<DrawTextSlot, decltype(&::DrawTextW)>();
            if (!orig) return DrawTextW(dc, text, count, rect, format);
            const auto* current = Current(dc);
            if (!current || !rect || !text || (format & (DT_CALCRECT | DT_MODIFYSTRING)))
                return orig(dc, text, count, rect, format);
            if (current->combo)
            {
                // VCL may paint the field text with DrawTextW on the owner DC.
                // Composite the glyphs without filling the background so the
                // selection/theme under the text stays intact.
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
                const Session comboSession = *current;
                LogComboText(comboSession, dc, format, static_cast<UINT>(count), rect,
                    GetBkColor(dc), L"drawtext");
#endif
                int comboResult = 0;
                if (SUCCEEDED(GlassCore::Text::DrawTextWithAlpha(
                        dc, text, count, rect, format, comboResult, true)))
                    return comboResult;
                return orig(dc, text, count, rect, format);
            }
            const Session s = *current;
            const int saved = SaveDC(dc);
            if (!saved) return orig(dc, text, count, rect, format);
            auto restore = wil::scope_exit([&]() noexcept { RestoreDC(dc, saved); });
            if (IntersectClipRect(dc, s.bounds.left, s.bounds.top, s.bounds.right, s.bounds.bottom) == ERROR)
                return orig(dc, text, count, rect, format);
            g_inHook = true;
            auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
            int result = 0;
            // Exact TListBox already painted its row (A100 or selection A255).
            // Do not let the text buffer's BPPF_ERASE replace that background.
            if (SUCCEEDED(GlassCore::Text::DrawTextWithAlpha(
                    dc, text, count, rect, format, result, s.plainList))) return result;
            return orig(dc, text, count, rect, format);
        }
    }

    void Startup() noexcept { g_shutdown.store(false, std::memory_order_release); }
    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        {
            auto lock = g_lock.lock_exclusive();
            g_dispatcher.DisableAllHooks();
        }
        // Shutdown can occur on this thread within an owner-draw session. Other
        // threads keep their own marker and release it in EndDraw.
        for (auto& s : g_stack)
        {
            if (s.themeDetourAcquired)
            {
                s.themeDetourAcquired = false;
                GlassCore::Hooks::UxThemeHooks::EnableThemeHooks(false);
            }
        }
    }
    bool IsInsideDraw() noexcept
    {
        // Existing callers suppress their text renderer for the full owner-draw
        // pipeline. A new FillRect-only row must not claim that text ownership;
        // keep any pre-existing outer list/field scope visible to those callers.
        for (const auto& s : g_stack)
            if (!s.combo || s.field) return true;
        return false;
    }
    bool BeginDraw(HWND parent, const DRAWITEMSTRUCT* dis) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !GlassCore::Root::IsFullInjectionMode()
            || !dis || (dis->CtlType != ODT_LISTBOX && dis->CtlType != ODT_COMBOBOX)
            || !dis->hDC || !dis->hwndItem
            || GetParent(dis->hwndItem) != parent || IsRectEmpty(&dis->rcItem)
            || !GlassShared::GlassIsBackdropAllowedForHwnd(dis->hwndItem)) return false;
        WCHAR cls[64]{};
        if (!GetClassNameW(dis->hwndItem, cls, _countof(cls))) return false;
        const bool list = dis->CtlType == ODT_LISTBOX;
        const bool combo = dis->CtlType == ODT_COMBOBOX;
        const bool color = list && _wcsicmp(cls, L"TColorListBox") == 0;
        const bool check = list && _wcsicmp(cls, L"TCheckListBox") == 0;
        // itemID=-1 also occurs on popup focus-only notifications; only the
        // explicit state bit proves a closed-field draw. TColorBox rows have
        // their own FillRect-only scope; other combos keep their previous path.
        const bool comboField = combo && (dis->itemState & ODS_COMBOBOXEDIT) != 0;
        const bool comboRow = combo && !comboField && _wcsicmp(cls, L"TColorBox") == 0
            && dis->itemID != static_cast<UINT>(-1)
            && (dis->itemAction & (ODA_DRAWENTIRE | ODA_SELECT)) != 0;
        if (list)
        {
            if (!color && !check && _wcsicmp(cls, L"TListBox") != 0
                && _wcsicmp(cls, L"TFileListBox") != 0
                && _wcsicmp(cls, L"TDirectoryListBox") != 0) return false;
            if (GlassShared::GlassGetWindowKind(dis->hwndItem) != GlassShared::GlassControlKind::ListBox)
                return false;
        }
        else
        {
            // TComboBox is deliberately excluded: its field text is already
            // painted correctly through the child Edit and themed field path.
            if (!comboField && !comboRow) return false;
            if (_wcsicmp(cls, L"TColorBox") != 0 && _wcsicmp(cls, L"TDriveComboBox") != 0
                && _wcsicmp(cls, L"TFilterComboBox") != 0) return false;
            if (GlassShared::GlassGetWindowKind(dis->hwndItem) != GlassShared::GlassControlKind::ComboBox)
                return false;
        }
        if (!comboRow && !IsInsideDraw())
        {
            // A FillRect-only outer row has no icon context to inherit.
            // Значки строк: Root\Control\ListBox; значок поля — ...\ComboBox.
            // Ключа может не быть — тогда фон значка определяется по пикселям.
            if (list)
                GlassCore::Settings::QueryIconBackgroundColorRemovalContext(
                    { L"Root", L"Control", L"ListBox" }, g_iconRemoval);
            else
                GlassCore::Settings::QueryIconBackgroundColorRemovalContext(
                    { L"Root", L"Control", L"ComboBox" }, g_iconRemoval);
        }
        if (list)
            GlassCore::DelphiBackground::ClearObservedListTail(dis->hwndItem, dis);
        HMODULE mod = GlassCore::ModuleHandler::g_entryPointModule;
        if (!mod) return false;
        Session s{dis->hwndItem, dis->hDC, dis->rcItem, color, check};
        s.plainList = list && _wcsicmp(cls, L"TListBox") == 0;
        s.selected = (dis->itemState & ODS_SELECTED) != 0;
        s.combo = combo;
        s.field = comboField;
        // A field owner-draw paints its background over the whole client area,
        // not only over rcItem. Both rectangles stay inside this one DC.
        s.bounds = s.item;
        if (s.field)
        {
            RECT client{};
            if (GetClientRect(dis->hwndItem, &client) && !IsRectEmpty(&client))
                s.bounds = client;
        }
        s.serial = ++g_sessionSerial;
        {
            auto lock = g_lock.lock_exclusive();
            if (g_shutdown.load(std::memory_order_acquire)) return false;
            g_dispatcher.moduleAddress = mod;
            for (size_t i = 0; i < Count; ++i)
            {
                // Leave the existing ComboLBox text/icon/selection path alone.
                if (comboRow && i != FillSlot) continue;
                // Combo field: FillRect plus the text APIs that VCL uses for
                // the field (ExtTextOutW strips the opaque background, DrawTextW
                // renders glyphs). Rectangle stays with VCL.
                if (s.combo && i != FillSlot && i != ExtTextSlot && i != DrawTextSlot
                    && i != BitBltSlot && i != StretchBltSlot) continue;
                if (i == RectangleSlot && !s.checkList) continue;
                const int before = g_dispatcher.GetRef(i);
                g_dispatcher.EnableHook(i, true);
                s.hooks[i] = g_dispatcher.GetRef(i) > before;
            }
        }
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        s.diagId = ++g_diagSequence;
        s.diagItem = dis->itemID;
#endif
        g_stack.push_back(s);
        if (s.checkList)
        {
            g_stack.back().themeDetourAcquired = GlassCore::Hooks::UxThemeHooks::EnableThemeHooks(true);
            s.themeDetourAcquired = g_stack.back().themeDetourAcquired;
        }
#if defined(_DEBUG)
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 24; }
        if (budget)
        {
            --budget;
            RECT client{};
            GetClientRect(s.hwnd, &client);
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiListDraw] hwnd=0x%p class=%s dc=0x%p combo=%d field=%d bounds=[%ld,%ld,%ld,%ld] fill=%d ext=%d draw=%d rectangle=%d bitblt=%d stretch=%d theme=%d state=0x%X item=%u rect=[%ld,%ld,%ld,%ld] client=[%ld,%ld,%ld,%ld]",
                s.hwnd, cls, s.dc, s.combo ? 1 : 0, s.field ? 1 : 0,
                s.bounds.left, s.bounds.top, s.bounds.right, s.bounds.bottom,
                s.hooks[FillSlot], s.hooks[ExtTextSlot], s.hooks[DrawTextSlot], s.hooks[RectangleSlot],
                s.hooks[BitBltSlot], s.hooks[StretchBltSlot], s.themeDetourAcquired,
                dis->itemState, dis->itemID, s.item.left, s.item.top, s.item.right, s.item.bottom,
                client.left, client.top, client.right, client.bottom);
        }
#endif
        return true;
    }
    bool TryCheckListThemeBackground(decltype(&::DrawThemeBackground) orig,
        HTHEME theme, HDC dc, int part, int state, LPCRECT rect, LPCRECT clip,
        HRESULT& result) noexcept
    {
        if (!orig || g_inCheckTheme || !theme || !rect || IsRectEmpty(rect) || part != BP_CHECKBOX)
            return false;
        const auto* current = Current(dc);
        if (!current || !current->checkList || !current->themeDetourAcquired) return false;
        const Session session = *current;
        WCHAR themeClass[MAX_PATH + 1]{};
        if (FAILED(Shared::ThemeHelper::GetThemeClass(theme, themeClass, MAX_PATH))
            || _wcsicmp(themeClass, L"Button") != 0) return false;
        // A later themed draw replaces (and on failure invalidates) any previous candidate.
        if (g_stack.empty() || g_stack.back().serial != session.serial) return false;
        g_stack.back().pendingCheckboxBackgroundOutline = false;

#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        g_stack.back().diagThemeRect = *rect;
        g_stack.back().diagState = state;
        g_stack.back().diagHasThemeRect = true;
        LogCheckPixels(session, L"theme.before", *rect, state);
#endif
        // Use the theme's own drawing and per-pixel alpha, not a reconstructed
        // checkbox, colour key, or a substitute clipping region. The buffer is
        // scoped to the API's original rectangle and the original clip is passed on.
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        BP_PAINTPARAMS params{sizeof(BP_PAINTPARAMS), 0, nullptr, &blend};
        HDC bufferDc = nullptr;
        HPAINTBUFFER buffer = BeginBufferedPaint(dc, rect, BPBF_TOPDOWNDIB, &params, &bufferDc);
        if (!buffer || !bufferDc)
        {
            if (buffer) EndBufferedPaint(buffer, FALSE);
            return false;
        }
        if (FAILED(BufferedPaintClear(buffer, nullptr)))
        {
            EndBufferedPaint(buffer, FALSE);
            return false;
        }
        g_inCheckTheme = true;
        const HRESULT draw = orig(theme, bufferDc, part, state, rect, clip);
        g_inCheckTheme = false;
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        LogCheckBuffer(session, buffer, *rect, state);
#endif
        const HRESULT end = EndBufferedPaint(buffer, SUCCEEDED(draw));
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        LogCheckPixels(session, L"theme.after", *rect, state);
#endif
#if defined(_DEBUG)
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
        if (budget)
        {
            --budget;
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiListDraw] checkbox DTB hwnd=0x%p dc=0x%p part=%d state=%d draw=0x%08X end=0x%08X",
                session.hwnd, dc, part, state, static_cast<unsigned>(draw), static_cast<unsigned>(end));
        }
#endif
        if (FAILED(draw) || FAILED(end)) return false;
        if (!g_stack.empty() && g_stack.back().serial == session.serial)
        {
            g_stack.back().checkboxThemeRect = *rect;
            g_stack.back().pendingCheckboxBackgroundOutline = true;
        }
        result = draw;
        return true;
    }
    void EndDraw() noexcept
    {
        if (g_stack.empty()) return;
        const Session s = g_stack.back();
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        if (s.diagHasThemeRect) LogCheckPixels(s, L"item.end", s.diagThemeRect, s.diagState);
#endif
        g_stack.pop_back();
        if (s.themeDetourAcquired)
            GlassCore::Hooks::UxThemeHooks::EnableThemeHooks(false);
        auto lock = g_lock.lock_exclusive();
        if (!g_shutdown.load(std::memory_order_acquire))
            for (size_t i = 0; i < Count; ++i)
                if (s.hooks[i]) g_dispatcher.EnableHook(i, false);
    }
}

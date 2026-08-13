// Тестовые глобальные Detours: трассировка вызовов (только Debug-сборка, см. vcxproj).
// Индекс слота N совпадает с битом DDH_* (1ull<<N) в DebugDetourHooks.h; группы DDH_ по смыслу — только в заголовке.
//
// Фон:   N=0  DrawThemeBackground; N=10 dui70::Element::PaintBackground (GetProcAddress; ABI x64: this, HDC, Value*, RECT&×4);
//        N=22 DrawThemeBackgroundEx
// Текст: N=1,2,3,4,5,8,23 — DTT, DTTEx, DrawTextW, DrawTextExW, ExtTextOutW, TextOutW, DrawTextWithGlow (uxtheme #126)
// GDI:   N=6 BitBlt; N=9 PatBlt; N=17 SetDIBits; N=15 SetDCBrushColor; N=16 SetBkColor; N=18 CreateSolidBrush;
//        N=19 GetSysColorBrush; N=20 GetStockObject; N=21 GetSysColor; N=27 Rectangle; N=28 FillRect (user32);
//        N=29 StretchBlt (gdi32)
// BP:    N=30 BeginBufferedPaint; N=31 EndBufferedPaint; N=32 BufferedPaintClear; N=33 BufferedPaintSetAlpha
// Окно:  N=7 AnimateWindow; N=14 SetClassLongPtrW
// DWM:   N=11 DwmExtendFrameIntoClientArea; N=13 DwmSetWindowAttribute;
//       N=25 DwmEnableBlurBehindWindow — lazy GetProcAddress(dwmapi);
//       N=26 SetWindowCompositionAttribute — lazy GetProcAddress(user32, SWCA)
// Тема:  N=12 GetThemeColor; N=24 SetSystemVisualStyle (uxtheme #65, .msstyles + схемы)
// Не включать одновременно с глобальными Detours в UxThemeHooks на те же экспорты без понимания цепочки.

#include "CorePch.h"

#if defined(_DEBUG)

#include <Core/Intercept/DebugDetourHooks.h>
#include <Core/Intercept/HookHelper.h>

#include <DebugLog.h>

#include <detours.h>
#include <intrin.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

#include <Helpers/ThemeHelper.h>

#include <wil/resource.h>

using namespace std::literals;

namespace GlassCore::Hooks::DebugDetourHooks
{
    namespace
    {
        constexpr size_t kSlotCount = 34;
        constexpr size_t kTextPreviewMax = 120;

        wil::srwlock g_lock;
        size_t g_ref[kSlotCount]{};

        // Глобальный Detour: указатели = то, что даёт линкер (IAT GlassCore → системная реализация).
        // «Кто вызвал» для лога: не _ReturnAddress() — при цепочке UiRib_My* → orig(DrawTextW) адрес
        // возврата лежит в GlassCore; обходим стек и берём первый кадр вне GlassCore.dll.
        decltype(&::DrawThemeBackground) g_trueDrawThemeBackground = ::DrawThemeBackground;
        decltype(&::DrawThemeBackgroundEx) g_trueDrawThemeBackgroundEx = ::DrawThemeBackgroundEx;
        decltype(&::DrawThemeText) g_trueDrawThemeText = ::DrawThemeText;
        decltype(&::DrawThemeTextEx) g_trueDrawThemeTextEx = ::DrawThemeTextEx;
        decltype(&::GetThemeColor) g_trueGetThemeColor = ::GetThemeColor;
        decltype(&::DrawTextW) g_trueDrawTextW = ::DrawTextW;
        decltype(&::DrawTextExW) g_trueDrawTextExW = ::DrawTextExW;
        decltype(&::ExtTextOutW) g_trueExtTextOutW = ::ExtTextOutW;
        decltype(&::BitBlt) g_trueBitBlt = ::BitBlt;
        decltype(&::AnimateWindow) g_trueAnimateWindow = ::AnimateWindow;
        decltype(&::TextOutW) g_trueTextOutW = ::TextOutW;
        decltype(&::PatBlt) g_truePatBlt = ::PatBlt;
        decltype(&::Rectangle) g_trueRectangle = ::Rectangle;
        decltype(&::FillRect) g_trueFillRect = ::FillRect;
        decltype(&::StretchBlt) g_trueStretchBlt = ::StretchBlt;
        decltype(&::DwmExtendFrameIntoClientArea) g_trueDwmExtendFrameIntoClientArea =
            ::DwmExtendFrameIntoClientArea;
        decltype(&::DwmSetWindowAttribute) g_trueDwmSetWindowAttribute = ::DwmSetWindowAttribute;
        using DwmEnableBlurBehindWindow_t = HRESULT(WINAPI*)(HWND hwnd, const DWM_BLURBEHIND* pBlurBehind);
        DwmEnableBlurBehindWindow_t g_trueDwmEnableBlurBehindWindow = nullptr;

        // user32: SetWindowCompositionAttribute (тот же layout, что в Shared::SwcaBackdrop).
        struct SwcaAttribData
        {
            DWORD dwAttribute;
            PVOID pvData;
            SIZE_T cbData;
        };

        using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND hwnd, SwcaAttribData* pData);
        SetWindowCompositionAttribute_t g_trueSetWindowCompositionAttribute = nullptr;
        decltype(&::SetClassLongPtrW) g_trueSetClassLongPtrW = ::SetClassLongPtrW;
        decltype(&::SetDCBrushColor) g_trueSetDCBrushColor = ::SetDCBrushColor;
        decltype(&::SetBkColor) g_trueSetBkColor = ::SetBkColor;
        decltype(&::SetDIBits) g_trueSetDIBits = ::SetDIBits;
        decltype(&::CreateSolidBrush) g_trueCreateSolidBrush = ::CreateSolidBrush;
        decltype(&::GetSysColorBrush) g_trueGetSysColorBrush = ::GetSysColorBrush;
        decltype(&::GetStockObject) g_trueGetStockObject = ::GetStockObject;
        decltype(&::GetSysColor) g_trueGetSysColor = ::GetSysColor;
        decltype(&::BeginBufferedPaint) g_trueBeginBufferedPaint = ::BeginBufferedPaint;
        decltype(&::EndBufferedPaint) g_trueEndBufferedPaint = ::EndBufferedPaint;
        decltype(&::BufferedPaintClear) g_trueBufferedPaintClear = ::BufferedPaintClear;
        decltype(&::BufferedPaintSetAlpha) g_trueBufferedPaintSetAlpha = ::BufferedPaintSetAlpha;

        // uxtheme.dll ordinal #126 — DrawTextWithGlow (RenderText.cpp / community name); не импортируется линкером.
        using DrawTextWithGlow_t = HRESULT(WINAPI*)(
            HDC hdc,
            LPCWSTR pszText,
            int cchText,
            LPRECT prc,
            UINT format,
            UINT textColor,
            UINT glowColor,
            UINT glowRadius,
            UINT glowIntensity,
            BOOL bPreMultiply,
            DTT_CALLBACK_PROC pfnDrawTextCallback,
            LPARAM lParam);

        DrawTextWithGlow_t g_trueDrawTextWithGlow = nullptr;

        // uxtheme.dll ordinal #65 — SetSystemVisualStyle; не в импортах линкера.
        using SetSystemVisualStyle_t = HRESULT(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, int);
        SetSystemVisualStyle_t g_trueSetSystemVisualStyle = nullptr;

        // dui70.dll — не системный импорт GlassCore; адрес через GetProcAddress при первом attach.
        using DuiPaintBackground_t = void(*)(
            void* pThis,
            HDC hdc,
            void* pValue,
            const RECT* pRect1,
            const RECT* pRect2,
            const RECT* pRect3,
            const RECT* pRect4);

        DuiPaintBackground_t g_trueDuiPaintBackground = nullptr;
    }

    namespace
    {
        [[nodiscard]] HMODULE GetCallerModule(void* returnAddress) noexcept
        {
            HMODULE hMod = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(returnAddress),
                &hMod);
            return hMod;
        }

        void GetModuleBaseName(HMODULE mod, wchar_t* buf, size_t bufChars) noexcept
        {
            if (!mod)
            {
                wcscpy_s(buf, bufChars, L"?");
                return;
            }
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path))))
            {
                wcscpy_s(buf, bufChars, L"?");
                return;
            }
            const wchar_t* base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            wcscpy_s(buf, bufChars, base);
        }

        [[nodiscard]] HMODULE GlassCoreModuleHandle() noexcept
        {
            HMODULE hMod = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(static_cast<void*>(&GlassCoreModuleHandle)),
                &hMod);
            return hMod;
        }

        // Прямой вызывающий (адрес возврата в хук): если это наш GlassCore — не логируем и не
        // применяем тестовые подмены параметров из хука (тихая передача в оригинал).
        [[nodiscard]] bool CallerModuleIsGlassCore(void* retAddr) noexcept
        {
            const HMODULE caller = GetCallerModule(retAddr);
            const HMODULE glassCore = GlassCoreModuleHandle();
            return caller != nullptr && caller == glassCore;
        }

        [[nodiscard]] static bool IsNoiseModuleBaseName(const wchar_t* base) noexcept
        {
            if (!base || !*base)
                return true;
            static const wchar_t* const kSkip[] = {
                L"ntdll.dll",
                L"kernel32.dll",
                L"KernelBase.dll",
                L"msvcp_win.dll",
                L"ucrtbase.dll",
            };
            for (const wchar_t* s : kSkip)
            {
                if (_wcsicmp(base, s) == 0)
                    return true;
            }
            return false;
        }

        // Имя модуля «внешнего» вызывающего: первый кадр стека вне GlassCore.dll и без ntdll/kernel32.
        void GetOuterCallerModuleBaseName(wchar_t* buf, size_t bufChars) noexcept
        {
            void* stack[48]{};
            const USHORT n = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(stack)), stack, nullptr);
            const HMODULE glassCore = GlassCoreModuleHandle();

            for (USHORT i = 0; i < n; ++i)
            {
                HMODULE frameMod = nullptr;
                if (!GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(stack[i]),
                        &frameMod))
                    continue;
                if (frameMod == glassCore)
                    continue;
                wchar_t tmp[64]{};
                GetModuleBaseName(frameMod, tmp, std::size(tmp));
                if (IsNoiseModuleBaseName(tmp))
                    continue;
                wcscpy_s(buf, bufChars, tmp);
                return;
            }

            wcscpy_s(buf, bufChars, L"?");
        }

        void FormatTextPreview(wchar_t* out, size_t outChars, LPCWSTR s, int cch) noexcept
        {
            if (!s)
            {
                wcscpy_s(out, outChars, L"(null)");
                return;
            }

            size_t n = 0;
            if (cch < 0)
            {
                n = wcsnlen(s, kTextPreviewMax + 1);
                if (n > kTextPreviewMax)
                    n = kTextPreviewMax;
            }
            else
            {
                n = static_cast<size_t>(cch);
                if (n > kTextPreviewMax)
                    n = kTextPreviewMax;
            }

            wmemcpy_s(out, outChars, s, n);
            out[n] = L'\0';

            const bool truncated =
                (cch < 0) ? (wcsnlen(s, kTextPreviewMax + 2) > kTextPreviewMax)
                          : (static_cast<size_t>(cch) > kTextPreviewMax);

            if (truncated)
                wcscat_s(out, outChars, L"…");
        }

        void FormatRectShort(wchar_t* buf, size_t bufChars, LPCRECT rc) noexcept
        {
            if (!rc)
            {
                wcscpy_s(buf, bufChars, L"-");
                return;
            }
            swprintf_s(
                buf,
                bufChars,
                L"%d,%d,%d,%d",
                rc->left,
                rc->top,
                rc->right,
                rc->bottom);
        }

        void FormatClassLongIndex(int nIndex, wchar_t* buf, size_t bufChars) noexcept
        {
            switch (nIndex)
            {
            case GCLP_MENUNAME:
                wcscpy_s(buf, bufChars, L"GCLP_MENUNAME");
                return;
            case GCLP_HBRBACKGROUND:
                wcscpy_s(buf, bufChars, L"GCLP_HBRBACKGROUND");
                return;
            case GCLP_HMODULE:
                wcscpy_s(buf, bufChars, L"GCLP_HMODULE");
                return;
            case GCL_CBWNDEXTRA:
                swprintf_s(buf, bufChars, L"GCL_CBWNDEXTRA(%d)", nIndex);
                return;
            case GCL_CBCLSEXTRA:
                swprintf_s(buf, bufChars, L"GCL_CBCLSEXTRA(%d)", nIndex);
                return;
            case GCLP_WNDPROC:
                wcscpy_s(buf, bufChars, L"GCLP_WNDPROC");
                return;
            case GCL_STYLE:
                wcscpy_s(buf, bufChars, L"GCL_STYLE");
                return;
            case GCLP_HICON:
                wcscpy_s(buf, bufChars, L"GCLP_HICON");
                return;
            case GCLP_HICONSM:
                wcscpy_s(buf, bufChars, L"GCLP_HICONSM");
                return;
            default:
                swprintf_s(buf, bufChars, L"nIndex=%d", nIndex);
                return;
            }
        }

        // Имя индекса GetSysColor / GetSysColorBrush (COLOR_*), как WindowCornerPreferenceName для DWM.
        [[nodiscard]] const wchar_t* SysColorIndexName(int nIndex) noexcept
        {
            switch (nIndex)
            {
            case COLOR_SCROLLBAR:
                return L"COLOR_SCROLLBAR";
            case COLOR_BACKGROUND:
                return L"COLOR_BACKGROUND";
            case COLOR_ACTIVECAPTION:
                return L"COLOR_ACTIVECAPTION";
            case COLOR_INACTIVECAPTION:
                return L"COLOR_INACTIVECAPTION";
            case COLOR_MENU:
                return L"COLOR_MENU";
            case COLOR_WINDOW:
                return L"COLOR_WINDOW";
            case COLOR_WINDOWFRAME:
                return L"COLOR_WINDOWFRAME";
            case COLOR_MENUTEXT:
                return L"COLOR_MENUTEXT";
            case COLOR_WINDOWTEXT:
                return L"COLOR_WINDOWTEXT";
            case COLOR_CAPTIONTEXT:
                return L"COLOR_CAPTIONTEXT";
            case COLOR_ACTIVEBORDER:
                return L"COLOR_ACTIVEBORDER";
            case COLOR_INACTIVEBORDER:
                return L"COLOR_INACTIVEBORDER";
            case COLOR_APPWORKSPACE:
                return L"COLOR_APPWORKSPACE";
            case COLOR_HIGHLIGHT:
                return L"COLOR_HIGHLIGHT";
            case COLOR_HIGHLIGHTTEXT:
                return L"COLOR_HIGHLIGHTTEXT";
            case COLOR_BTNFACE:
                return L"COLOR_BTNFACE";
            case COLOR_BTNSHADOW:
                return L"COLOR_BTNSHADOW";
            case COLOR_GRAYTEXT:
                return L"COLOR_GRAYTEXT";
            case COLOR_BTNTEXT:
                return L"COLOR_BTNTEXT";
            case COLOR_INACTIVECAPTIONTEXT:
                return L"COLOR_INACTIVECAPTIONTEXT";
            case COLOR_BTNHIGHLIGHT:
                return L"COLOR_BTNHIGHLIGHT";
            default:
                return nullptr;
            }
        }

        void FormatSysColorIndexTag(int nIndex, wchar_t* buf, size_t bufChars) noexcept
        {
            if (const wchar_t* name = SysColorIndexName(nIndex))
                wcscpy_s(buf, bufChars, name);
            else
                swprintf_s(buf, bufChars, L"nIndex=%d(?)", nIndex);
        }

        void FormatStockObjectTag(int fnObject, wchar_t* buf, size_t bufChars) noexcept
        {
            switch (fnObject)
            {
            case WHITE_BRUSH:
                wcscpy_s(buf, bufChars, L"WHITE_BRUSH");
                return;
            case LTGRAY_BRUSH:
                wcscpy_s(buf, bufChars, L"LTGRAY_BRUSH");
                return;
            case GRAY_BRUSH:
                wcscpy_s(buf, bufChars, L"GRAY_BRUSH");
                return;
            case DKGRAY_BRUSH:
                wcscpy_s(buf, bufChars, L"DKGRAY_BRUSH");
                return;
            case BLACK_BRUSH:
                wcscpy_s(buf, bufChars, L"BLACK_BRUSH");
                return;
            case NULL_BRUSH:
                wcscpy_s(buf, bufChars, L"NULL_BRUSH/HOLLOW_BRUSH");
                return;
            case WHITE_PEN:
                wcscpy_s(buf, bufChars, L"WHITE_PEN");
                return;
            case BLACK_PEN:
                wcscpy_s(buf, bufChars, L"BLACK_PEN");
                return;
            case NULL_PEN:
                wcscpy_s(buf, bufChars, L"NULL_PEN");
                return;
            case OEM_FIXED_FONT:
                wcscpy_s(buf, bufChars, L"OEM_FIXED_FONT");
                return;
            case ANSI_FIXED_FONT:
                wcscpy_s(buf, bufChars, L"ANSI_FIXED_FONT");
                return;
            case ANSI_VAR_FONT:
                wcscpy_s(buf, bufChars, L"ANSI_VAR_FONT");
                return;
            case SYSTEM_FONT:
                wcscpy_s(buf, bufChars, L"SYSTEM_FONT");
                return;
            case DEVICE_DEFAULT_FONT:
                wcscpy_s(buf, bufChars, L"DEVICE_DEFAULT_FONT");
                return;
            case DEFAULT_PALETTE:
                wcscpy_s(buf, bufChars, L"DEFAULT_PALETTE");
                return;
            case SYSTEM_FIXED_FONT:
                wcscpy_s(buf, bufChars, L"SYSTEM_FIXED_FONT");
                return;
            case DEFAULT_GUI_FONT:
                wcscpy_s(buf, bufChars, L"DEFAULT_GUI_FONT");
                return;
            case DC_BRUSH:
                wcscpy_s(buf, bufChars, L"DC_BRUSH");
                return;
            case DC_PEN:
                wcscpy_s(buf, bufChars, L"DC_PEN");
                return;
            default:
                swprintf_s(buf, bufChars, L"fnObject=%d(?)", fnObject);
                return;
            }
        }

        // Окно, связанное с DC (для логов: какой класс реально рисуется).
        void FormatHdcWindowSuffix(HDC hdc, wchar_t* buf, size_t bufChars) noexcept
        {
            if (!hdc)
            {
                wcscpy_s(buf, bufChars, L"hdcWin=-");
                return;
            }
            HWND const w = WindowFromDC(hdc);
            if (!w)
            {
                wcscpy_s(buf, bufChars, L"hdcWin=-");
                return;
            }
            wchar_t cls[96]{};
            if (!GetClassNameW(w, cls, static_cast<int>(std::size(cls))))
                wcscpy_s(cls, L"?");
            swprintf_s(buf, bufChars, L"hdcWin=%p cls=\"%s\"", w, cls);
        }

        // Расшифровка DTBGOPTS.dwFlags для лога DrawThemeBackgroundEx.
        void FormatDtbgOptsFlagsDetail(DWORD flags, wchar_t* buf, size_t bufChars) noexcept
        {
            if (flags == 0)
            {
                wcscpy_s(buf, bufChars, L"(none)");
                return;
            }
            buf[0] = L'\0';
            size_t pos = 0;
            auto appendSep = [&]() {
                if (pos > 0 && pos + 2 < bufChars)
                    pos += static_cast<size_t>(swprintf_s(buf + pos, bufChars - pos, L"|"));
            };
            auto appendStr = [&](const wchar_t* s) {
                if (!s || pos >= bufChars)
                    return;
                pos += static_cast<size_t>(swprintf_s(buf + pos, bufChars - pos, L"%s", s));
            };
            if (flags & DTBG_DRAWSOLID)
            {
                appendSep();
                appendStr(L"DRAWSOLID");
            }
            if (flags & DTBG_OMITBORDER)
            {
                appendSep();
                appendStr(L"OMITBORDER");
            }
            if (flags & DTBG_OMITCONTENT)
            {
                appendSep();
                appendStr(L"OMITCONTENT");
            }
            if (flags & DTBG_COMPUTINGREGION)
            {
                appendSep();
                appendStr(L"COMPUTINGREGION");
            }
            if (flags & DTBG_CLIPRECT)
            {
                appendSep();
                appendStr(L"CLIPRECT");
            }
            if (flags & DTBG_MIRRORDC)
            {
                appendSep();
                appendStr(L"MIRRORDC");
            }
            if (flags & DTBG_NOMIRROR)
            {
                appendSep();
                appendStr(L"NOMIRROR");
            }
            const DWORD kKnown = DTBG_VALIDBITS;
            if (const DWORD unk = flags & ~kKnown)
            {
                appendSep();
                swprintf_s(buf + pos, bufChars - pos, L"unk=0x%X", unk);
            }
            if (buf[0] == L'\0')
                wcscpy_s(buf, bufChars, L"(none)");
        }

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_CAPTION_COLOR_GRADIENT
#define DWMWA_CAPTION_COLOR_GRADIENT 37
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWA_MICA_EFFECT
#define DWMWA_MICA_EFFECT 1029
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFEu
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFFu
#endif

        void FormatDwmColorArgb(wchar_t* buf, size_t bufChars, DWORD cr) noexcept
        {
            if (cr == DWMWA_COLOR_DEFAULT)
            {
                wcscpy_s(buf, bufChars, L"DWMWA_COLOR_DEFAULT");
                return;
            }
            if (cr == DWMWA_COLOR_NONE)
            {
                wcscpy_s(buf, bufChars, L"DWMWA_COLOR_NONE");
                return;
            }
            BYTE a = static_cast<BYTE>((cr >> 24) & 0xFFu);
            const BYTE r = GetRValue(cr);
            const BYTE g = GetGValue(cr);
            const BYTE b = GetBValue(cr);
            if (a == 0)
                a = 0xFF;
            const DWORD argb =
                (static_cast<DWORD>(a) << 24u) | (static_cast<DWORD>(r) << 16u) |
                (static_cast<DWORD>(g) << 8u) | static_cast<DWORD>(b);
            swprintf_s(
                buf,
                bufChars,
                L"ARGB=0x%08X A=%u R=%u G=%u B=%u raw=0x%08X",
                argb,
                static_cast<unsigned>(a),
                static_cast<unsigned>(r),
                static_cast<unsigned>(g),
                static_cast<unsigned>(b),
                cr);
        }

        [[nodiscard]] const wchar_t* SystemBackdropKindName(DWORD v) noexcept
        {
            switch (v)
            {
            case 0:
                return L"DWMSBT_AUTO";
            case 1:
                return L"DWMSBT_NONE";
            case 2:
                return L"DWMSBT_MAINWINDOW";
            case 3:
                return L"DWMSBT_TRANSIENTWINDOW";
            case 4:
                return L"DWMSBT_TABBEDWINDOW";
            default:
                return L"?";
            }
        }

        [[nodiscard]] const wchar_t* WindowCornerPreferenceName(DWORD v) noexcept
        {
            switch (v)
            {
            case 0:
                return L"DWMWCP_DEFAULT";
            case 1:
                return L"DWMWCP_DONOTROUND";
            case 2:
                return L"DWMWCP_ROUND";
            case 3:
                return L"DWMWCP_ROUNDSMALL";
            default:
                return L"?";
            }
        }

        void DwmAttributeTag(DWORD attr, wchar_t* buf, size_t bufChars) noexcept
        {
            const wchar_t* name = nullptr;
            switch (attr)
            {
            case DWMWA_USE_IMMERSIVE_DARK_MODE:
                name = L"DWMWA_USE_IMMERSIVE_DARK_MODE";
                break;
            case DWMWA_WINDOW_CORNER_PREFERENCE:
                name = L"DWMWA_WINDOW_CORNER_PREFERENCE";
                break;
            case DWMWA_BORDER_COLOR:
                name = L"DWMWA_BORDER_COLOR";
                break;
            case DWMWA_CAPTION_COLOR:
                name = L"DWMWA_CAPTION_COLOR";
                break;
            case DWMWA_TEXT_COLOR:
                name = L"DWMWA_TEXT_COLOR";
                break;
            case DWMWA_CAPTION_COLOR_GRADIENT:
                name = L"DWMWA_CAPTION_COLOR_GRADIENT";
                break;
            case DWMWA_SYSTEMBACKDROP_TYPE:
                name = L"DWMWA_SYSTEMBACKDROP_TYPE";
                break;
            case DWMWA_MICA_EFFECT:
                name = L"DWMWA_MICA_EFFECT";
                break;
            default:
                swprintf_s(buf, bufChars, L"attr=%lu(?)", static_cast<unsigned long>(attr));
                return;
            }
            swprintf_s(
                buf,
                bufChars,
                L"attr=%lu(%s)",
                static_cast<unsigned long>(attr),
                name);
        }

        void BuildDwmSetWindowAttributeDetail(
            DWORD dwAttribute,
            LPCVOID pvAttribute,
            DWORD cbAttribute,
            wchar_t* detail,
            size_t detailChars) noexcept
        {
            if (!pvAttribute)
            {
                wcscpy_s(detail, detailChars, L"pv=null");
                return;
            }
            if (cbAttribute < sizeof(DWORD))
            {
                swprintf_s(
                    detail,
                    detailChars,
                    L"cb=%u (short buffer)",
                    static_cast<unsigned>(cbAttribute));
                return;
            }

            DWORD v{};
            {
                const auto* const p = static_cast<const BYTE*>(pvAttribute);
                v = static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8u) |
                    (static_cast<DWORD>(p[2]) << 16u) | (static_cast<DWORD>(p[3]) << 24u);
            }

            switch (dwAttribute)
            {
            case DWMWA_SYSTEMBACKDROP_TYPE:
                swprintf_s(
                    detail,
                    detailChars,
                    L"backdrop=%s(%lu)",
                    SystemBackdropKindName(v),
                    static_cast<unsigned long>(v));
                return;
            case DWMWA_WINDOW_CORNER_PREFERENCE:
                swprintf_s(
                    detail,
                    detailChars,
                    L"corner=%s(%lu)",
                    WindowCornerPreferenceName(v),
                    static_cast<unsigned long>(v));
                return;
            case DWMWA_CAPTION_COLOR_GRADIENT:
            {
                wchar_t col[160]{};
                FormatDwmColorArgb(col, std::size(col), v);
                swprintf_s(detail, detailChars, L"captionGradient %s", col);
                return;
            }
            case DWMWA_BORDER_COLOR:
            case DWMWA_CAPTION_COLOR:
            case DWMWA_TEXT_COLOR:
            {
                wchar_t col[160]{};
                FormatDwmColorArgb(col, std::size(col), v);
                swprintf_s(detail, detailChars, L"color %s", col);
                return;
            }
            case DWMWA_USE_IMMERSIVE_DARK_MODE:
            case DWMWA_MICA_EFFECT:
                swprintf_s(
                    detail,
                    detailChars,
                    L"BOOL/int=%lu (%s)",
                    static_cast<unsigned long>(v),
                    v ? L"true/on" : L"false/off");
                return;
            default:
                swprintf_s(
                    detail,
                    detailChars,
                    L"value32=0x%08X",
                    v);
                return;
            }
        }

        void FormatWcaAttrib(DWORD attr, wchar_t* buf, size_t bufChars) noexcept
        {
            const wchar_t* name = nullptr;
            switch (attr)
            {
            case 0:
                name = L"WCA_UNDEFINED";
                break;
            case 1:
                name = L"WCA_NCRENDERING_ENABLED";
                break;
            case 2:
                name = L"WCA_NCRENDERING_POLICY";
                break;
            case 3:
                name = L"WCA_TRANSITIONS_FORCEDISABLED";
                break;
            case 4:
                name = L"WCA_ALLOW_NCPAINT";
                break;
            case 5:
                name = L"WCA_CAPTION_BUTTON_BOUNDS";
                break;
            case 6:
                name = L"WCA_NONCLIENT_RTL_LAYOUT";
                break;
            case 7:
                name = L"WCA_FORCE_ICONIC_REPRESENTATION";
                break;
            case 8:
                name = L"WCA_EXTENDED_FRAME_BOUNDS";
                break;
            case 9:
                name = L"WCA_HAS_ICONIC_BITMAP";
                break;
            case 10:
                name = L"WCA_THEME_ATTRIBUTES";
                break;
            case 11:
                name = L"WCA_NCRENDERING_EXILED";
                break;
            case 12:
                name = L"WCA_NCADORNMENTINFO";
                break;
            case 13:
                name = L"WCA_EXCLUDED_FROM_LIVEPREVIEW";
                break;
            case 14:
                name = L"WCA_VIDEO_OVERLAY_ACTIVE";
                break;
            case 15:
                name = L"WCA_FORCE_ACTIVEWINDOW_APPEARANCE";
                break;
            case 16:
                name = L"WCA_DISALLOW_PEEK";
                break;
            case 17:
                name = L"WCA_CLOAK";
                break;
            case 18:
                name = L"WCA_CLOAKED";
                break;
            case 19:
                name = L"WCA_ACCENT_POLICY";
                break;
            case 20:
                name = L"WCA_FREEZE_REPRESENTATION";
                break;
            case 21:
                name = L"WCA_EVER_UNCLOAKED";
                break;
            case 22:
                name = L"WCA_VISUAL_OWNER";
                break;
            case 23:
                name = L"WCA_HOLOGRAPHIC";
                break;
            case 24:
                name = L"WCA_EXCLUDED_FROM_DDA";
                break;
            case 25:
                name = L"WCA_PASSIVEUPDATEMODE";
                break;
            case 26:
                name = L"WCA_LAST";
                break;
            default:
                swprintf_s(
                    buf,
                    bufChars,
                    L"attr=%lu(?)",
                    static_cast<unsigned long>(attr));
                return;
            }
            swprintf_s(
                buf,
                bufChars,
                L"attr=%lu(%s)",
                static_cast<unsigned long>(attr),
                name);
        }

        [[nodiscard]] const wchar_t* AccentStateName(DWORD state) noexcept
        {
            switch (state)
            {
            case 0:
                return L"ACCENT_DISABLED";
            case 1:
                return L"ACCENT_ENABLE_GRADIENT";
            case 2:
                return L"ACCENT_ENABLE_TRANSPARENTGRADIENT";
            case 3:
                return L"ACCENT_ENABLE_BLURBEHIND";
            case 4:
                return L"ACCENT_ENABLE_ACRYLICBLURBEHIND";
            case 5:
                return L"ACCENT_ENABLE_HOSTBACKDROP";
            default:
                return nullptr;
            }
        }

        void BuildSetWindowCompositionAttributeDetail(
            DWORD dwAttribute,
            PVOID pvData,
            SIZE_T cbData,
            wchar_t* detail,
            size_t detailChars) noexcept
        {
            constexpr DWORD kWcaAccentPolicy = 19;
            if (!pvData)
            {
                wcscpy_s(detail, detailChars, L"pv=null");
                return;
            }
            if (dwAttribute == kWcaAccentPolicy)
            {
                constexpr size_t kAccentPolicyBytes = sizeof(DWORD) * 4;
                if (cbData < kAccentPolicyBytes)
                {
                    swprintf_s(
                        detail,
                        detailChars,
                        L"ACCENT_POLICY cb=%llu (short<%zu)",
                        static_cast<unsigned long long>(cbData),
                        kAccentPolicyBytes);
                    return;
                }
                const auto* p = static_cast<const BYTE*>(pvData);
                const DWORD state = static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8u) |
                    (static_cast<DWORD>(p[2]) << 16u) | (static_cast<DWORD>(p[3]) << 24u);
                const DWORD flags = static_cast<DWORD>(p[4]) | (static_cast<DWORD>(p[5]) << 8u) |
                    (static_cast<DWORD>(p[6]) << 16u) | (static_cast<DWORD>(p[7]) << 24u);
                const DWORD grad = static_cast<DWORD>(p[8]) | (static_cast<DWORD>(p[9]) << 8u) |
                    (static_cast<DWORD>(p[10]) << 16u) | (static_cast<DWORD>(p[11]) << 24u);
                const DWORD anim = static_cast<DWORD>(p[12]) | (static_cast<DWORD>(p[13]) << 8u) |
                    (static_cast<DWORD>(p[14]) << 16u) | (static_cast<DWORD>(p[15]) << 24u);
                if (const wchar_t* st = AccentStateName(state))
                {
                    swprintf_s(
                        detail,
                        detailChars,
                        L"ACCENT_POLICY state=%s(%lu) flags=0x%08X gradient=0x%08X anim=%lu",
                        st,
                        static_cast<unsigned long>(state),
                        flags,
                        grad,
                        static_cast<unsigned long>(anim));
                }
                else
                {
                    swprintf_s(
                        detail,
                        detailChars,
                        L"ACCENT_POLICY state=%lu(?) flags=0x%08X gradient=0x%08X anim=%lu",
                        static_cast<unsigned long>(state),
                        flags,
                        grad,
                        static_cast<unsigned long>(anim));
                }
                return;
            }
            swprintf_s(
                detail,
                detailChars,
                L"pv=%p cb=%llu",
                pvData,
                static_cast<unsigned long long>(cbData));
        }

        // Корневое окно оболочки проводника (Win11): только для него — тест подмены Mica→Acrylic.
        [[nodiscard]] bool IsExplorerShellHostFrame(HWND hwnd) noexcept
        {
            if (!hwnd || !IsWindow(hwnd))
                return false;
            wchar_t cls[96]{};
            if (!GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))))
                return false;
            return _wcsicmp(cls, L"CabinetWClass") == 0 || _wcsicmp(cls, L"ExploreWClass") == 0;
        }

        [[nodiscard]] HRESULT EnsureDuiPaintBackgroundPtr() noexcept
        {
            if (g_trueDuiPaintBackground)
                return S_OK;
            HMODULE h = GetModuleHandleW(L"dui70.dll");
            if (!h)
                h = LoadLibraryW(L"dui70.dll");
            if (!h)
                return HRESULT_FROM_WIN32(GetLastError());

            static const char* const kPaintNames[] = {
                "?PaintBackground@Element@DirectUI@@QEAAXPEAUHDC__@@PEAVValue@2@AEBUtagRECT@@222@Z",
            };
            for (const char* name : kPaintNames)
            {
                if (void* p = reinterpret_cast<void*>(GetProcAddress(h, name)))
                {
                    g_trueDuiPaintBackground = reinterpret_cast<DuiPaintBackground_t>(p);
                    return S_OK;
                }
            }
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }

        [[nodiscard]] HRESULT EnsureDrawTextWithGlowPtr() noexcept
        {
            if (g_trueDrawTextWithGlow)
                return S_OK;
            HMODULE h = GetModuleHandleW(L"uxtheme.dll");
            if (!h)
                h = LoadLibraryW(L"uxtheme.dll");
            if (!h)
                return HRESULT_FROM_WIN32(GetLastError());
            auto* const p = reinterpret_cast<DrawTextWithGlow_t>(
                GetProcAddress(h, MAKEINTRESOURCEA(126)));
            if (!p)
                return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            g_trueDrawTextWithGlow = p;
            return S_OK;
        }

        [[nodiscard]] HRESULT EnsureSetSystemVisualStylePtr() noexcept
        {
            if (g_trueSetSystemVisualStyle)
                return S_OK;
            HMODULE h = GetModuleHandleW(L"uxtheme.dll");
            if (!h)
                h = LoadLibraryW(L"uxtheme.dll");
            if (!h)
                return HRESULT_FROM_WIN32(GetLastError());
            auto* const p = reinterpret_cast<SetSystemVisualStyle_t>(
                GetProcAddress(h, MAKEINTRESOURCEA(65)));
            if (!p)
                return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            g_trueSetSystemVisualStyle = p;
            return S_OK;
        }

        [[nodiscard]] HRESULT EnsureDwmEnableBlurBehindWindowPtr() noexcept
        {
            if (g_trueDwmEnableBlurBehindWindow)
                return S_OK;
            HMODULE h = GetModuleHandleW(L"dwmapi.dll");
            if (!h)
                h = LoadLibraryW(L"dwmapi.dll");
            if (!h)
                return HRESULT_FROM_WIN32(GetLastError());
            auto* const p =
                reinterpret_cast<DwmEnableBlurBehindWindow_t>(GetProcAddress(h, "DwmEnableBlurBehindWindow"));
            if (!p)
                return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            g_trueDwmEnableBlurBehindWindow = p;
            return S_OK;
        }

        [[nodiscard]] HRESULT EnsureSetWindowCompositionAttributePtr() noexcept
        {
            if (g_trueSetWindowCompositionAttribute)
                return S_OK;
            HMODULE h = GetModuleHandleW(L"user32.dll");
            if (!h)
                h = LoadLibraryW(L"user32.dll");
            if (!h)
                return HRESULT_FROM_WIN32(GetLastError());
            auto* const p = reinterpret_cast<SetWindowCompositionAttribute_t>(
                GetProcAddress(h, "SetWindowCompositionAttribute"));
            if (!p)
                return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            g_trueSetWindowCompositionAttribute = p;
            return S_OK;
        }

        HRESULT WINAPI HookDrawThemeBackground(
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCRECT pRect,
            LPCRECT pClipRect);

        HRESULT WINAPI HookDrawThemeBackgroundEx(
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCRECT pRect,
            const DTBGOPTS* pOptions);

        HRESULT WINAPI HookDrawThemeText(
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCWSTR pszText,
            int cchText,
            DWORD dwTextFlags,
            DWORD dwTextFlags2,
            LPCRECT pRect);

        HRESULT WINAPI HookDrawThemeTextEx(
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCWSTR pszText,
            int cchText,
            DWORD dwTextFlags,
            LPRECT pRect,
            const DTTOPTS* pOptions);

        int WINAPI HookDrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format);

        int WINAPI HookDrawTextExW(
            HDC hdc,
            LPWSTR lpchText,
            int cchText,
            LPRECT lprc,
            UINT format,
            LPDRAWTEXTPARAMS lpdtp);

        BOOL WINAPI HookExtTextOutW(
            HDC hdc,
            int x,
            int y,
            UINT options,
            const RECT* lprect,
            LPCWSTR lpString,
            UINT c,
            const INT* lpDx);

        BOOL WINAPI HookBitBlt(
            HDC hdc,
            int x,
            int y,
            int cx,
            int cy,
            HDC hdcSrc,
            int x1,
            int y1,
            DWORD rop);

        BOOL WINAPI HookAnimateWindow(HWND hWnd, DWORD dwTime, DWORD dwFlags);

        BOOL WINAPI HookTextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c);

        BOOL WINAPI HookPatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop);

        BOOL WINAPI HookRectangle(HDC hdc, int left, int top, int right, int bottom);

        int WINAPI HookFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr);

        BOOL WINAPI HookStretchBlt(
            HDC hdcDest,
            int xDest,
            int yDest,
            int wDest,
            int hDest,
            HDC hdcSrc,
            int xSrc,
            int ySrc,
            int wSrc,
            int hSrc,
            DWORD rop);

        HPAINTBUFFER WINAPI HookBeginBufferedPaint(
            HDC hdcTarget,
            const RECT* prcTarget,
            BP_BUFFERFORMAT dwFormat,
            BP_PAINTPARAMS* pPaintParams,
            HDC* phdc);

        HRESULT WINAPI HookEndBufferedPaint(HPAINTBUFFER hBufferedPaint, BOOL fUpdateTarget);

        HRESULT WINAPI HookBufferedPaintClear(HPAINTBUFFER hBufferedPaint, const RECT* prc);

        HRESULT WINAPI HookBufferedPaintSetAlpha(
            HPAINTBUFFER hBufferedPaint, const RECT* prc, BYTE alpha);

        HRESULT WINAPI HookDwmExtendFrameIntoClientArea(HWND hWnd, const MARGINS* pMarInset);

        HRESULT WINAPI HookDwmEnableBlurBehindWindow(HWND hWnd, const DWM_BLURBEHIND* pBlurBehind);

        HRESULT WINAPI HookDwmSetWindowAttribute(
            HWND hwnd,
            DWORD dwAttribute,
            LPCVOID pvAttribute,
            DWORD cbAttribute);

        BOOL WINAPI HookSetWindowCompositionAttribute(HWND hwnd, SwcaAttribData* pData);

        HRESULT WINAPI HookGetThemeColor(
            HTHEME hTheme,
            int iPartId,
            int iStateId,
            int iPropId,
            COLORREF* pColor);

        LONG_PTR WINAPI HookSetClassLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong);

        COLORREF WINAPI HookSetDCBrushColor(HDC hdc, COLORREF color);

        COLORREF WINAPI HookSetBkColor(HDC hdc, COLORREF color);

        HBRUSH WINAPI HookCreateSolidBrush(COLORREF cr);

        HBRUSH WINAPI HookGetSysColorBrush(int nIndex);

        HGDIOBJ WINAPI HookGetStockObject(int fnObject);

        COLORREF WINAPI HookGetSysColor(int nIndex);

        HRESULT WINAPI HookDrawTextWithGlow(
            HDC hdc,
            LPCWSTR pszText,
            int cchText,
            LPRECT prc,
            UINT format,
            UINT textColor,
            UINT glowColor,
            UINT glowRadius,
            UINT glowIntensity,
            BOOL bPreMultiply,
            DTT_CALLBACK_PROC pfnDrawTextCallback,
            LPARAM lParam);

        HRESULT WINAPI HookSetSystemVisualStyle(
            LPCWSTR pszFilename, LPCWSTR pszColor, LPCWSTR pszSize, int reserved);

        int WINAPI HookSetDIBits(
            HDC hdc,
            HBITMAP hbm,
            UINT uStartScan,
            UINT cScanLines,
            CONST VOID* lpvBits,
            CONST BITMAPINFO* lpbmi,
            UINT fuColorUse);

        void HookDuiPaintBackground(
            void* pThis,
            HDC hdc,
            void* pValue,
            const RECT* pRect1,
            const RECT* pRect2,
            const RECT* pRect3,
            const RECT* pRect4);

        void AttachSlot(size_t slot)
        {
            switch (slot)
            {
            case 0:
                THROW_HR_IF(E_POINTER, !g_trueDrawThemeBackground);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDrawThemeBackground),
                    reinterpret_cast<void*>(&HookDrawThemeBackground));
                break;
            case 1:
                THROW_HR_IF(E_POINTER, !g_trueDrawThemeText);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDrawThemeText),
                    reinterpret_cast<void*>(&HookDrawThemeText));
                break;
            case 2:
                THROW_HR_IF(E_POINTER, !g_trueDrawThemeTextEx);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDrawThemeTextEx),
                    reinterpret_cast<void*>(&HookDrawThemeTextEx));
                break;
            case 3:
                THROW_HR_IF(E_POINTER, !g_trueDrawTextW);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDrawTextW),
                    reinterpret_cast<void*>(&HookDrawTextW));
                break;
            case 4:
                THROW_HR_IF(E_POINTER, !g_trueDrawTextExW);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDrawTextExW),
                    reinterpret_cast<void*>(&HookDrawTextExW));
                break;
            case 5:
                THROW_HR_IF(E_POINTER, !g_trueExtTextOutW);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueExtTextOutW),
                    reinterpret_cast<void*>(&HookExtTextOutW));
                break;
            case 6:
                THROW_HR_IF(E_POINTER, !g_trueBitBlt);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueBitBlt),
                    reinterpret_cast<void*>(&HookBitBlt));
                break;
            case 7:
                THROW_HR_IF(E_POINTER, !g_trueAnimateWindow);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueAnimateWindow),
                    reinterpret_cast<void*>(&HookAnimateWindow));
                break;
            case 8:
                THROW_HR_IF(E_POINTER, !g_trueTextOutW);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueTextOutW),
                    reinterpret_cast<void*>(&HookTextOutW));
                break;
            case 9:
                THROW_HR_IF(E_POINTER, !g_truePatBlt);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_truePatBlt),
                    reinterpret_cast<void*>(&HookPatBlt));
                break;
            case 10:
                THROW_IF_FAILED(EnsureDuiPaintBackgroundPtr());
                THROW_HR_IF(E_POINTER, !g_trueDuiPaintBackground);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDuiPaintBackground),
                    reinterpret_cast<void*>(&HookDuiPaintBackground));
                break;
            case 11:
                THROW_HR_IF(E_POINTER, !g_trueDwmExtendFrameIntoClientArea);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDwmExtendFrameIntoClientArea),
                    reinterpret_cast<void*>(&HookDwmExtendFrameIntoClientArea));
                break;
            case 12:
                THROW_HR_IF(E_POINTER, !g_trueGetThemeColor);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueGetThemeColor),
                    reinterpret_cast<void*>(&HookGetThemeColor));
                break;
            case 13:
                THROW_HR_IF(E_POINTER, !g_trueDwmSetWindowAttribute);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDwmSetWindowAttribute),
                    reinterpret_cast<void*>(&HookDwmSetWindowAttribute));
                break;
            case 14:
                THROW_HR_IF(E_POINTER, !g_trueSetClassLongPtrW);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueSetClassLongPtrW),
                    reinterpret_cast<void*>(&HookSetClassLongPtrW));
                break;
            case 15:
                THROW_HR_IF(E_POINTER, !g_trueSetDCBrushColor);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueSetDCBrushColor),
                    reinterpret_cast<void*>(&HookSetDCBrushColor));
                break;
            case 16:
                THROW_HR_IF(E_POINTER, !g_trueSetBkColor);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueSetBkColor),
                    reinterpret_cast<void*>(&HookSetBkColor));
                break;
            case 17:
                THROW_HR_IF(E_POINTER, !g_trueSetDIBits);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueSetDIBits),
                    reinterpret_cast<void*>(&HookSetDIBits));
                break;
            case 18:
                THROW_HR_IF(E_POINTER, !g_trueCreateSolidBrush);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueCreateSolidBrush),
                    reinterpret_cast<void*>(&HookCreateSolidBrush));
                break;
            case 19:
                THROW_HR_IF(E_POINTER, !g_trueGetSysColorBrush);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueGetSysColorBrush),
                    reinterpret_cast<void*>(&HookGetSysColorBrush));
                break;
            case 20:
                THROW_HR_IF(E_POINTER, !g_trueGetStockObject);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueGetStockObject),
                    reinterpret_cast<void*>(&HookGetStockObject));
                break;
            case 21:
                THROW_HR_IF(E_POINTER, !g_trueGetSysColor);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueGetSysColor),
                    reinterpret_cast<void*>(&HookGetSysColor));
                break;
            case 22:
                THROW_HR_IF(E_POINTER, !g_trueDrawThemeBackgroundEx);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDrawThemeBackgroundEx),
                    reinterpret_cast<void*>(&HookDrawThemeBackgroundEx));
                break;
            case 23:
                THROW_IF_FAILED(EnsureDrawTextWithGlowPtr());
                THROW_HR_IF(E_POINTER, !g_trueDrawTextWithGlow);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDrawTextWithGlow),
                    reinterpret_cast<void*>(&HookDrawTextWithGlow));
                break;
            case 24:
                THROW_IF_FAILED(EnsureSetSystemVisualStylePtr());
                THROW_HR_IF(E_POINTER, !g_trueSetSystemVisualStyle);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueSetSystemVisualStyle),
                    reinterpret_cast<void*>(&HookSetSystemVisualStyle));
                break;
            case 25:
                THROW_IF_FAILED(EnsureDwmEnableBlurBehindWindowPtr());
                THROW_HR_IF(E_POINTER, !g_trueDwmEnableBlurBehindWindow);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueDwmEnableBlurBehindWindow),
                    reinterpret_cast<void*>(&HookDwmEnableBlurBehindWindow));
                break;
            case 26:
                THROW_IF_FAILED(EnsureSetWindowCompositionAttributePtr());
                THROW_HR_IF(E_POINTER, !g_trueSetWindowCompositionAttribute);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueSetWindowCompositionAttribute),
                    reinterpret_cast<void*>(&HookSetWindowCompositionAttribute));
                break;
            case 27:
                THROW_HR_IF(E_POINTER, !g_trueRectangle);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueRectangle),
                    reinterpret_cast<void*>(&HookRectangle));
                break;
            case 28:
                THROW_HR_IF(E_POINTER, !g_trueFillRect);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueFillRect),
                    reinterpret_cast<void*>(&HookFillRect));
                break;
            case 29:
                THROW_HR_IF(E_POINTER, !g_trueStretchBlt);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueStretchBlt),
                    reinterpret_cast<void*>(&HookStretchBlt));
                break;
            case 30:
                THROW_HR_IF(E_POINTER, !g_trueBeginBufferedPaint);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueBeginBufferedPaint),
                    reinterpret_cast<void*>(&HookBeginBufferedPaint));
                break;
            case 31:
                THROW_HR_IF(E_POINTER, !g_trueEndBufferedPaint);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueEndBufferedPaint),
                    reinterpret_cast<void*>(&HookEndBufferedPaint));
                break;
            case 32:
                THROW_HR_IF(E_POINTER, !g_trueBufferedPaintClear);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueBufferedPaintClear),
                    reinterpret_cast<void*>(&HookBufferedPaintClear));
                break;
            case 33:
                THROW_HR_IF(E_POINTER, !g_trueBufferedPaintSetAlpha);
                Hooks::Detours::Attach(
                    reinterpret_cast<void**>(&g_trueBufferedPaintSetAlpha),
                    reinterpret_cast<void*>(&HookBufferedPaintSetAlpha));
                break;
            default:
                THROW_HR(E_INVALIDARG);
            }
        }

        void DetachSlot(size_t slot)
        {
            switch (slot)
            {
            case 0:
                if (g_trueDrawThemeBackground)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDrawThemeBackground),
                        reinterpret_cast<void*>(&HookDrawThemeBackground));
                break;
            case 1:
                if (g_trueDrawThemeText)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDrawThemeText),
                        reinterpret_cast<void*>(&HookDrawThemeText));
                break;
            case 2:
                if (g_trueDrawThemeTextEx)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDrawThemeTextEx),
                        reinterpret_cast<void*>(&HookDrawThemeTextEx));
                break;
            case 3:
                if (g_trueDrawTextW)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDrawTextW),
                        reinterpret_cast<void*>(&HookDrawTextW));
                break;
            case 4:
                if (g_trueDrawTextExW)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDrawTextExW),
                        reinterpret_cast<void*>(&HookDrawTextExW));
                break;
            case 5:
                if (g_trueExtTextOutW)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueExtTextOutW),
                        reinterpret_cast<void*>(&HookExtTextOutW));
                break;
            case 6:
                if (g_trueBitBlt)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueBitBlt),
                        reinterpret_cast<void*>(&HookBitBlt));
                break;
            case 7:
                if (g_trueAnimateWindow)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueAnimateWindow),
                        reinterpret_cast<void*>(&HookAnimateWindow));
                break;
            case 8:
                if (g_trueTextOutW)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueTextOutW),
                        reinterpret_cast<void*>(&HookTextOutW));
                break;
            case 9:
                if (g_truePatBlt)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_truePatBlt),
                        reinterpret_cast<void*>(&HookPatBlt));
                break;
            case 10:
                if (g_trueDuiPaintBackground)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDuiPaintBackground),
                        reinterpret_cast<void*>(&HookDuiPaintBackground));
                break;
            case 11:
                if (g_trueDwmExtendFrameIntoClientArea)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDwmExtendFrameIntoClientArea),
                        reinterpret_cast<void*>(&HookDwmExtendFrameIntoClientArea));
                break;
            case 12:
                if (g_trueGetThemeColor)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueGetThemeColor),
                        reinterpret_cast<void*>(&HookGetThemeColor));
                break;
            case 13:
                if (g_trueDwmSetWindowAttribute)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDwmSetWindowAttribute),
                        reinterpret_cast<void*>(&HookDwmSetWindowAttribute));
                break;
            case 14:
                if (g_trueSetClassLongPtrW)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueSetClassLongPtrW),
                        reinterpret_cast<void*>(&HookSetClassLongPtrW));
                break;
            case 15:
                if (g_trueSetDCBrushColor)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueSetDCBrushColor),
                        reinterpret_cast<void*>(&HookSetDCBrushColor));
                break;
            case 16:
                if (g_trueSetBkColor)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueSetBkColor),
                        reinterpret_cast<void*>(&HookSetBkColor));
                break;
            case 17:
                if (g_trueSetDIBits)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueSetDIBits),
                        reinterpret_cast<void*>(&HookSetDIBits));
                break;
            case 18:
                if (g_trueCreateSolidBrush)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueCreateSolidBrush),
                        reinterpret_cast<void*>(&HookCreateSolidBrush));
                break;
            case 19:
                if (g_trueGetSysColorBrush)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueGetSysColorBrush),
                        reinterpret_cast<void*>(&HookGetSysColorBrush));
                break;
            case 20:
                if (g_trueGetStockObject)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueGetStockObject),
                        reinterpret_cast<void*>(&HookGetStockObject));
                break;
            case 21:
                if (g_trueGetSysColor)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueGetSysColor),
                        reinterpret_cast<void*>(&HookGetSysColor));
                break;
            case 22:
                if (g_trueDrawThemeBackgroundEx)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDrawThemeBackgroundEx),
                        reinterpret_cast<void*>(&HookDrawThemeBackgroundEx));
                break;
            case 23:
                if (g_trueDrawTextWithGlow)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDrawTextWithGlow),
                        reinterpret_cast<void*>(&HookDrawTextWithGlow));
                break;
            case 24:
                if (g_trueSetSystemVisualStyle)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueSetSystemVisualStyle),
                        reinterpret_cast<void*>(&HookSetSystemVisualStyle));
                break;
            case 25:
                if (g_trueDwmEnableBlurBehindWindow)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueDwmEnableBlurBehindWindow),
                        reinterpret_cast<void*>(&HookDwmEnableBlurBehindWindow));
                break;
            case 26:
                if (g_trueSetWindowCompositionAttribute)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueSetWindowCompositionAttribute),
                        reinterpret_cast<void*>(&HookSetWindowCompositionAttribute));
                break;
            case 27:
                if (g_trueRectangle)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueRectangle),
                        reinterpret_cast<void*>(&HookRectangle));
                break;
            case 28:
                if (g_trueFillRect)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueFillRect),
                        reinterpret_cast<void*>(&HookFillRect));
                break;
            case 29:
                if (g_trueStretchBlt)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueStretchBlt),
                        reinterpret_cast<void*>(&HookStretchBlt));
                break;
            case 30:
                if (g_trueBeginBufferedPaint)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueBeginBufferedPaint),
                        reinterpret_cast<void*>(&HookBeginBufferedPaint));
                break;
            case 31:
                if (g_trueEndBufferedPaint)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueEndBufferedPaint),
                        reinterpret_cast<void*>(&HookEndBufferedPaint));
                break;
            case 32:
                if (g_trueBufferedPaintClear)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueBufferedPaintClear),
                        reinterpret_cast<void*>(&HookBufferedPaintClear));
                break;
            case 33:
                if (g_trueBufferedPaintSetAlpha)
                    Hooks::Detours::Detach(
                        reinterpret_cast<void**>(&g_trueBufferedPaintSetAlpha),
                        reinterpret_cast<void*>(&HookBufferedPaintSetAlpha));
                break;
            default:
                break;
            }
        }

        HRESULT WINAPI HookDrawThemeBackground(
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCRECT pRect,
            LPCRECT pClipRect)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
            wchar_t modName[64]{};
            GetOuterCallerModuleBaseName(modName, std::size(modName));
            WCHAR cls[MAX_PATH + 1]{};
            const bool classOk = SUCCEEDED(
                Shared::ThemeHelper::GetThemeClass(hTheme, cls, MAX_PATH));
            wchar_t rcBuf[48]{};
            wchar_t clipBuf[48]{};
            wchar_t winBuf[160]{};
            FormatRectShort(rcBuf, std::size(rcBuf), pRect);
            FormatRectShort(clipBuf, std::size(clipBuf), pClipRect);
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] DrawThemeBackground caller=%s className=\"%s\" classOk=%d hdc=%p part=%d state=%d rect=%s clip=%s %s",
                modName,
                classOk ? cls : L"",
                classOk ? 1 : 0,
                hdc,
                iPartId,
                iStateId,
                rcBuf,
                clipBuf,
                winBuf);
            return g_trueDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        HRESULT WINAPI HookDrawThemeBackgroundEx(
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCRECT pRect,
            const DTBGOPTS* pOptions)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDrawThemeBackgroundEx(hTheme, hdc, iPartId, iStateId, pRect, pOptions);
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            WCHAR cls[MAX_PATH + 1]{};
            const bool classOk = SUCCEEDED(
                Shared::ThemeHelper::GetThemeClass(hTheme, cls, MAX_PATH));
            wchar_t rcBuf[48]{};
            FormatRectShort(rcBuf, std::size(rcBuf), pRect);
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));

            wchar_t optDetail[384]{};
            if (!pOptions)
            {
                wcscpy_s(optDetail, L"pOptions=null");
            }
            else
            {
                const DWORD sz = pOptions->dwSize;
                wchar_t flagsHuman[192]{};
                FormatDtbgOptsFlagsDetail(pOptions->dwFlags, flagsHuman, std::size(flagsHuman));
                if (sz < sizeof(DTBGOPTS))
                {
                    swprintf_s(
                        optDetail,
                        std::size(optDetail),
                        L"pOptions=%p dwSize=%lu(short<%zu) dwFlags=0x%08X [%s]",
                        static_cast<const void*>(pOptions),
                        static_cast<unsigned long>(sz),
                        sizeof(DTBGOPTS),
                        pOptions->dwFlags,
                        flagsHuman);
                }
                else
                {
                    wchar_t clipBuf[48]{};
                    FormatRectShort(clipBuf, std::size(clipBuf), &pOptions->rcClip);
                    swprintf_s(
                        optDetail,
                        std::size(optDetail),
                        L"pOptions=%p dwSize=%lu dwFlags=0x%08X [%s] rcClip=%s",
                        static_cast<const void*>(pOptions),
                        static_cast<unsigned long>(sz),
                        pOptions->dwFlags,
                        flagsHuman,
                        clipBuf);
                }
            }

            const HRESULT hr = g_trueDrawThemeBackgroundEx(
                hTheme, hdc, iPartId, iStateId, pRect, pOptions);

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] DrawThemeBackgroundEx outerCaller=%s retAddrMod=%s "
                L"className=\"%s\" classOk=%d hdc=%p part=%d state=%d rect=%s %s | %s | hr=0x%08X",
                outerMod,
                retAddrMod,
                classOk ? cls : L"",
                classOk ? 1 : 0,
                hdc,
                iPartId,
                iStateId,
                rcBuf,
                winBuf,
                optDetail,
                static_cast<unsigned int>(hr));

            return hr;
        }

        HRESULT WINAPI HookDrawThemeText(
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCWSTR pszText,
            int cchText,
            DWORD dwTextFlags,
            DWORD dwTextFlags2,
            LPCRECT pRect)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDrawThemeText(
                    hTheme,
                    hdc,
                    iPartId,
                    iStateId,
                    pszText,
                    cchText,
                    dwTextFlags,
                    dwTextFlags2,
                    pRect);
            wchar_t modName[64]{};
            GetOuterCallerModuleBaseName(modName, std::size(modName));
            WCHAR cls[MAX_PATH + 1]{};
            const bool classOk = SUCCEEDED(
                Shared::ThemeHelper::GetThemeClass(hTheme, cls, MAX_PATH));
            wchar_t preview[160]{};
            FormatTextPreview(preview, std::size(preview), pszText, cchText);
            wchar_t rcBuf[48]{};
            wchar_t winBuf[160]{};
            FormatRectShort(rcBuf, std::size(rcBuf), pRect);
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] DrawThemeText caller=%s className=\"%s\" classOk=%d hdc=%p part=%d state=%d text=\"%s\" cch=%d flags=0x%X flags2=0x%X rect=%s %s",
                modName,
                classOk ? cls : L"",
                classOk ? 1 : 0,
                hdc,
                iPartId,
                iStateId,
                preview,
                cchText,
                dwTextFlags,
                dwTextFlags2,
                rcBuf,
                winBuf);
            return g_trueDrawThemeText(
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pszText,
                cchText,
                dwTextFlags,
                dwTextFlags2,
                pRect);
        }

        HRESULT WINAPI HookDrawThemeTextEx(
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCWSTR pszText,
            int cchText,
            DWORD dwTextFlags,
            LPRECT pRect,
            const DTTOPTS* pOptions)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDrawThemeTextEx(
                    hTheme,
                    hdc,
                    iPartId,
                    iStateId,
                    pszText,
                    cchText,
                    dwTextFlags,
                    pRect,
                    pOptions);
            wchar_t modName[64]{};
            GetOuterCallerModuleBaseName(modName, std::size(modName));
            WCHAR cls[MAX_PATH + 1]{};
            const bool classOk = SUCCEEDED(
                Shared::ThemeHelper::GetThemeClass(hTheme, cls, MAX_PATH));
            wchar_t preview[160]{};
            FormatTextPreview(preview, std::size(preview), pszText, cchText);
            wchar_t rcBuf[48]{};
            FormatRectShort(rcBuf, std::size(rcBuf), pRect);
            const DWORD optSize = pOptions ? pOptions->dwSize : 0;
            const DWORD optFlags = pOptions ? pOptions->dwFlags : 0;
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] DrawThemeTextEx caller=%s className=\"%s\" classOk=%d hdc=%p part=%d state=%d text=\"%s\" cch=%d textFlags=0x%X rect=%s DTTOPTS(size=%u flags=0x%X) %s",
                modName,
                classOk ? cls : L"",
                classOk ? 1 : 0,
                hdc,
                iPartId,
                iStateId,
                preview,
                cchText,
                dwTextFlags,
                rcBuf,
                optSize,
                optFlags,
                winBuf);
            return g_trueDrawThemeTextEx(
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pszText,
                cchText,
                dwTextFlags,
                pRect,
                pOptions);
        }

        HRESULT WINAPI HookDrawTextWithGlow(
            HDC hdc,
            LPCWSTR pszText,
            int cchText,
            LPRECT prc,
            UINT format,
            UINT textColor,
            UINT glowColor,
            UINT glowRadius,
            UINT glowIntensity,
            BOOL bPreMultiply,
            DTT_CALLBACK_PROC pfnDrawTextCallback,
            LPARAM lParam)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDrawTextWithGlow(
                    hdc,
                    pszText,
                    cchText,
                    prc,
                    format,
                    textColor,
                    glowColor,
                    glowRadius,
                    glowIntensity,
                    bPreMultiply,
                    pfnDrawTextCallback,
                    lParam);
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t preview[160]{};
            FormatTextPreview(preview, std::size(preview), pszText, cchText);
            wchar_t rcBuf[48]{};
            FormatRectShort(rcBuf, std::size(rcBuf), prc);
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] DrawTextWithGlow(uxtheme#126) outerCaller=%s retAddrMod=%s hdc=%p "
                L"text=\"%s\" cch=%d format=0x%X rect=%s textColor=0x%08X glowColor=0x%08X gRad=%u gInt=%u "
                L"bPreMul=%d pfn=%p lParam=%p %s",
                outerMod,
                retAddrMod,
                hdc,
                preview,
                cchText,
                format,
                rcBuf,
                textColor,
                glowColor,
                glowRadius,
                glowIntensity,
                static_cast<int>(bPreMultiply),
                reinterpret_cast<const void*>(pfnDrawTextCallback),
                reinterpret_cast<const void*>(lParam),
                winBuf);
            return g_trueDrawTextWithGlow(
                hdc,
                pszText,
                cchText,
                prc,
                format,
                textColor,
                glowColor,
                glowRadius,
                glowIntensity,
                bPreMultiply,
                pfnDrawTextCallback,
                lParam);
        }

        HRESULT WINAPI HookSetSystemVisualStyle(
            LPCWSTR pszFilename, LPCWSTR pszColor, LPCWSTR pszSize, int reserved)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueSetSystemVisualStyle(pszFilename, pszColor, pszSize, reserved);
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t filePrev[160]{};
            wchar_t colorPrev[160]{};
            wchar_t sizePrev[160]{};
            FormatTextPreview(filePrev, std::size(filePrev), pszFilename, -1);
            FormatTextPreview(colorPrev, std::size(colorPrev), pszColor, -1);
            FormatTextPreview(sizePrev, std::size(sizePrev), pszSize, -1);

            const HRESULT hr = g_trueSetSystemVisualStyle(pszFilename, pszColor, pszSize, reserved);

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] SetSystemVisualStyle(uxtheme#65) outerCaller=%s retAddrMod=%s "
                L"file=\"%s\" color=\"%s\" size=\"%s\" reserved=%d hr=0x%08X",
                outerMod,
                retAddrMod,
                filePrev,
                colorPrev,
                sizePrev,
                reserved,
                static_cast<unsigned int>(hr));
            return hr;
        }

        int WINAPI HookDrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDrawTextW(hdc, lpchText, cchText, lprc, format);
            wchar_t modName[64]{};
            GetOuterCallerModuleBaseName(modName, std::size(modName));
            wchar_t preview[160]{};
            FormatTextPreview(preview, std::size(preview), lpchText, cchText);
            wchar_t rcBuf[48]{};
            wchar_t winBuf[160]{};
            FormatRectShort(rcBuf, std::size(rcBuf), lprc);
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] DrawTextW caller=%s hdc=%p text=\"%s\" cch=%d format=0x%X rect=%s %s",
                modName,
                hdc,
                preview,
                cchText,
                format,
                rcBuf,
                winBuf);
            return g_trueDrawTextW(hdc, lpchText, cchText, lprc, format);
        }

        int WINAPI HookDrawTextExW(
            HDC hdc,
            LPWSTR lpchText,
            int cchText,
            LPRECT lprc,
            UINT format,
            LPDRAWTEXTPARAMS lpdtp)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
            wchar_t modName[64]{};
            GetOuterCallerModuleBaseName(modName, std::size(modName));
            wchar_t preview[160]{};
            FormatTextPreview(preview, std::size(preview), lpchText, cchText);
            wchar_t rcBuf[48]{};
            wchar_t winBuf[160]{};
            FormatRectShort(rcBuf, std::size(rcBuf), lprc);
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] DrawTextExW caller=%s hdc=%p text=\"%s\" cch=%d format=0x%X rect=%s lpdtp=%p %s",
                modName,
                hdc,
                preview,
                cchText,
                format,
                rcBuf,
                lpdtp,
                winBuf);
            return g_trueDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
        }

        BOOL WINAPI HookExtTextOutW(
            HDC hdc,
            int x,
            int y,
            UINT options,
            const RECT* lprect,
            LPCWSTR lpString,
            UINT c,
            const INT* lpDx)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
            wchar_t modName[64]{};
            GetOuterCallerModuleBaseName(modName, std::size(modName));
            wchar_t preview[160]{};
            const int cch = (c == 0xFFFFFFFFu) ? -1 : static_cast<int>(c);
            FormatTextPreview(preview, std::size(preview), lpString, cch);
            wchar_t rcBuf[48]{};
            wchar_t winBuf[160]{};
            FormatRectShort(rcBuf, std::size(rcBuf), lprect);
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] ExtTextOutW caller=%s hdc=%p x=%d y=%d opt=0x%X rect=%s c=%u text=\"%s\" lpDx=%p %s",
                modName,
                hdc,
                x,
                y,
                options,
                rcBuf,
                c,
                preview,
                lpDx,
                winBuf);
            /*if (GetCallerModule(_ReturnAddress()) == GetModuleHandleW(L"UxTheme.dll"))
            {
                PatBlt(hdc, x, y, lprect->right, lprect->bottom, BLACKNESS);
                return 1;
            }*/
            
            return g_trueExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
        }

        BOOL WINAPI HookBitBlt(
            HDC hdc,
            int x,
            int y,
            int cx,
            int cy,
            HDC hdcSrc,
            int x1,
            int y1,
            DWORD rop)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
            wchar_t modName[64]{};
            GetOuterCallerModuleBaseName(modName, std::size(modName));
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] BitBlt caller=%s hdc=%p dst=(%d,%d) %dx%d hdcSrc=%p src=(%d,%d) rop=0x%08X %s",
                modName,
                hdc,
                x,
                y,
                cx,
                cy,
                hdcSrc,
                x1,
                y1,
                rop,
                winBuf);

            // Диагностика places-bar: цвет пикселей src до SRCCOPY (буфер вне FillRect-зонда).
            const bool uxCaller = (_wcsicmp(modName, L"uxtheme.dll") == 0);
            HWND hDest = WindowFromDC(hdc);
            wchar_t destCls[64]{};
            if (hDest)
                GetClassNameW(hDest, destCls, static_cast<int>(std::size(destCls)));
            const bool toolbarDest = (destCls[0] && _wcsicmp(destCls, L"ToolbarWindow32") == 0);
            const bool largeBlit = (cx >= 100 && cy >= 100);
            if (hdcSrc && ((uxCaller && largeBlit) || toolbarDest))
            {
                const COLORREF px00 = GetPixel(hdcSrc, x1, y1);
                const COLORREF pxMid = GetPixel(
                    hdcSrc,
                    x1 + (cx > 0 ? cx / 2 : 0),
                    y1 + (cy > 0 ? cy / 2 : 0));
                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCore][DebugDetour] BitBlt srcPixels caller=%s %dx%d "
                    L"px00=0x%06X pxMid=0x%06X destCls=\"%s\" hdcSrc=%p",
                    modName,
                    cx,
                    cy,
                    static_cast<unsigned>(px00 & 0xFFFFFF),
                    static_cast<unsigned>(pxMid & 0xFFFFFF),
                    destCls[0] ? destCls : L"-",
                    hdcSrc);
            }

            return g_trueBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        }

        BOOL WINAPI HookAnimateWindow(HWND hWnd, DWORD dwTime, DWORD dwFlags)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueAnimateWindow(hWnd, dwTime, dwFlags);
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t cls[72]{};
            if (hWnd && IsWindow(hWnd))
                GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls)));
            else
                wcscpy_s(cls, L"?");

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] AnimateWindow outerCaller=%s retAddrMod=%s hwnd=%p class=\"%s\" time=%u flags=0x%X",
                outerMod,
                retAddrMod,
                hWnd,
                cls,
                dwTime,
                dwFlags);
            return g_trueAnimateWindow(hWnd, dwTime, dwFlags);
        }

        BOOL WINAPI HookTextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueTextOutW(hdc, x, y, lpString, c);
            wchar_t modName[64]{};
            GetOuterCallerModuleBaseName(modName, std::size(modName));
            wchar_t preview[160]{};
            FormatTextPreview(preview, std::size(preview), lpString, c);
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] TextOutW caller=%s hdc=%p x=%d y=%d c=%d text=\"%s\" %s",
                modName,
                hdc,
                x,
                y,
                c,
                preview,
                winBuf);
            return g_trueTextOutW(hdc, x, y, lpString, c);
        }

        BOOL WINAPI HookPatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_truePatBlt(hdc, x, y, w, h, rop);
            wchar_t modName[64]{};
            GetOuterCallerModuleBaseName(modName, std::size(modName));
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] PatBlt caller=%s hdc=%p (%d,%d) %dx%d rop=0x%08X %s",
                modName,
                hdc,
                x,
                y,
                w,
                h,
                rop,
                winBuf);
            return g_truePatBlt(hdc, x, y, w, h, rop);
        }

        BOOL WINAPI HookRectangle(HDC hdc, int left, int top, int right, int bottom)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueRectangle(hdc, left, top, right, bottom);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] Rectangle outerCaller=%s retAddrMod=%s hdc=%p rect=[%d,%d,%d,%d] %s",
                outerMod,
                retAddrMod,
                hdc,
                left,
                top,
                right,
                bottom,
                winBuf);

            

            return g_trueRectangle(hdc, left, top, right, bottom);
        }

        int WINAPI HookFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueFillRect(hdc, lprc, hbr);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));

            wchar_t brushBuf[96]{};
            LOGBRUSH lb{};
            if (hbr && GetObjectW(hbr, sizeof(lb), &lb) == sizeof(lb))
            {
                swprintf_s(
                    brushBuf,
                    L"hbr=%p style=%lu color=0x%08X hatch=%lld",
                    hbr,
                    static_cast<unsigned long>(lb.lbStyle),
                    static_cast<unsigned>(lb.lbColor),
                    static_cast<long long>(lb.lbHatch));
            }
            else
            {
                swprintf_s(brushBuf, L"hbr=%p", hbr);
            }

            if (lprc)
            {
                DebugLog(
                    DBG_NORMAL,
                    L"[GlassCore][DebugDetour] FillRect outerCaller=%s retAddrMod=%s hdc=%p rect=[%d,%d,%d,%d] %s %s",
                    outerMod,
                    retAddrMod,
                    hdc,
                    lprc->left,
                    lprc->top,
                    lprc->right,
                    lprc->bottom,
                    brushBuf,
                    winBuf);
            }
            else
            {
                DebugLog(
                    DBG_NORMAL,
                    L"[GlassCore][DebugDetour] FillRect outerCaller=%s retAddrMod=%s hdc=%p rect=(null) %s %s",
                    outerMod,
                    retAddrMod,
                    hdc,
                    brushBuf,
                    winBuf);
            }
            
            return g_trueFillRect(hdc, lprc, hbr);
        }

        BOOL WINAPI HookStretchBlt(
            HDC hdcDest,
            int xDest,
            int yDest,
            int wDest,
            int hDest,
            HDC hdcSrc,
            int xSrc,
            int ySrc,
            int wSrc,
            int hSrc,
            DWORD rop)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueStretchBlt(
                    hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdcDest, winBuf, std::size(winBuf));

            const int stretchMode = hdcDest ? GetStretchBltMode(hdcDest) : 0;

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] StretchBlt outerCaller=%s retAddrMod=%s "
                L"hdcDest=%p dst=(%d,%d) %dx%d hdcSrc=%p src=(%d,%d) %dx%d rop=0x%08X stretchMode=%d %s",
                outerMod,
                retAddrMod,
                hdcDest,
                xDest,
                yDest,
                wDest,
                hDest,
                hdcSrc,
                xSrc,
                ySrc,
                wSrc,
                hSrc,
                rop,
                stretchMode,
                winBuf);

            return g_trueStretchBlt(
                hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }

        HPAINTBUFFER WINAPI HookBeginBufferedPaint(
            HDC hdcTarget,
            const RECT* prcTarget,
            BP_BUFFERFORMAT dwFormat,
            BP_PAINTPARAMS* pPaintParams,
            HDC* phdc)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueBeginBufferedPaint(hdcTarget, prcTarget, dwFormat, pPaintParams, phdc);

            const HPAINTBUFFER hbp = g_trueBeginBufferedPaint(
                hdcTarget, prcTarget, dwFormat, pPaintParams, phdc);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdcTarget, winBuf, std::size(winBuf));
            wchar_t rcBuf[48]{};
            FormatRectShort(rcBuf, std::size(rcBuf), prcTarget);

            DWORD bpFlags = 0;
            const void* pBlend = nullptr;
            if (pPaintParams && pPaintParams->cbSize >= sizeof(BP_PAINTPARAMS))
            {
                bpFlags = pPaintParams->dwFlags;
                pBlend = pPaintParams->pBlendFunction;
            }

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] BeginBufferedPaint outerCaller=%s retAddrMod=%s "
                L"hdcTarget=%p target=%s format=%d flags=0x%X pBlend=%p hbp=%p hdcBuf=%p %s",
                outerMod,
                retAddrMod,
                hdcTarget,
                rcBuf,
                static_cast<int>(dwFormat),
                static_cast<unsigned>(bpFlags),
                pBlend,
                hbp,
                phdc ? *phdc : nullptr,
                winBuf);

            return hbp;
        }

        HRESULT WINAPI HookEndBufferedPaint(HPAINTBUFFER hBufferedPaint, BOOL fUpdateTarget)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueEndBufferedPaint(hBufferedPaint, fUpdateTarget);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));

            const HRESULT hr = g_trueEndBufferedPaint(hBufferedPaint, fUpdateTarget);

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] EndBufferedPaint outerCaller=%s retAddrMod=%s "
                L"hbp=%p fUpdate=%d hr=0x%08X",
                outerMod,
                retAddrMod,
                hBufferedPaint,
                fUpdateTarget ? 1 : 0,
                static_cast<unsigned>(hr));

            return hr;
        }

        HRESULT WINAPI HookBufferedPaintClear(HPAINTBUFFER hBufferedPaint, const RECT* prc)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueBufferedPaintClear(hBufferedPaint, prc);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t rcBuf[48]{};
            FormatRectShort(rcBuf, std::size(rcBuf), prc);

            const HRESULT hr = g_trueBufferedPaintClear(hBufferedPaint, prc);

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] BufferedPaintClear outerCaller=%s retAddrMod=%s "
                L"hbp=%p rect=%s hr=0x%08X",
                outerMod,
                retAddrMod,
                hBufferedPaint,
                rcBuf,
                static_cast<unsigned>(hr));

            return hr;
        }

        HRESULT WINAPI HookBufferedPaintSetAlpha(
            HPAINTBUFFER hBufferedPaint, const RECT* prc, BYTE alpha)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueBufferedPaintSetAlpha(hBufferedPaint, prc, alpha);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t rcBuf[48]{};
            FormatRectShort(rcBuf, std::size(rcBuf), prc);

            const HRESULT hr = g_trueBufferedPaintSetAlpha(hBufferedPaint, prc, alpha);

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] BufferedPaintSetAlpha outerCaller=%s retAddrMod=%s "
                L"hbp=%p rect=%s alpha=%u hr=0x%08X",
                outerMod,
                retAddrMod,
                hBufferedPaint,
                rcBuf,
                static_cast<unsigned>(alpha),
                static_cast<unsigned>(hr));

            return hr;
        }

        HRESULT WINAPI HookDwmExtendFrameIntoClientArea(HWND hWnd, const MARGINS* pMarInset)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDwmExtendFrameIntoClientArea(hWnd, pMarInset);
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t cls[96]{};
            if (hWnd && IsWindow(hWnd))
            {
                if (!GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))))
                    wcscpy_s(cls, L"?");
            }
            else
            {
                wcscpy_s(cls, L"?");
            }

            wchar_t margins[96]{};
            if (!pMarInset)
            {
                wcscpy_s(margins, L"(null)");
            }
            else
            {
                swprintf_s(
                    margins,
                    L"l=%d t=%d r=%d b=%d",
                    pMarInset->cxLeftWidth,
                    pMarInset->cyTopHeight,
                    pMarInset->cxRightWidth,
                    pMarInset->cyBottomHeight);
            }

            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][DebugDetour] DwmExtendFrameIntoClientArea outerCaller=%s retAddrMod=%s hwnd=%p class=\"%s\" margins=%s",
                outerMod,
                retAddrMod,
                hWnd,
                cls,
                margins);

            return g_trueDwmExtendFrameIntoClientArea(hWnd, pMarInset);
        }

        void FormatBlurBehindDetail(const DWM_BLURBEHIND* pBb, wchar_t* detail, size_t detailChars) noexcept
        {
            if (!pBb)
            {
                wcscpy_s(detail, detailChars, L"pBlurBehind=null");
                return;
            }
            const DWORD f = pBb->dwFlags;
            wchar_t parts[288]{};
            size_t pos = 0;
            auto appendSep = [&]() noexcept {
                if (pos > 0 && pos + 4 < std::size(parts))
                {
                    parts[pos++] = L' ';
                    parts[pos++] = L'|';
                    parts[pos++] = L' ';
                }
            };
            if (f & DWM_BB_ENABLE)
            {
                appendSep();
                pos += static_cast<size_t>(
                    swprintf_s(parts + pos, std::size(parts) - pos, L"ENABLE fEnable=%d", pBb->fEnable ? 1 : 0));
            }
            if (f & DWM_BB_BLURREGION)
            {
                appendSep();
                pos += static_cast<size_t>(
                    swprintf_s(
                        parts + pos,
                        std::size(parts) - pos,
                        L"BLURREGION hRgn=%p",
                        static_cast<void*>(pBb->hRgnBlur)));
            }
            if (f & DWM_BB_TRANSITIONONMAXIMIZED)
            {
                appendSep();
                pos += static_cast<size_t>(swprintf_s(
                    parts + pos,
                    std::size(parts) - pos,
                    L"TRANS_ON_MAX fTransitionOnMaximized=%d",
                    pBb->fTransitionOnMaximized ? 1 : 0));
            }
            const DWORD known = DWM_BB_ENABLE | DWM_BB_BLURREGION | DWM_BB_TRANSITIONONMAXIMIZED;
            const DWORD unk = f & ~known;
            if (unk != 0)
            {
                appendSep();
                pos += static_cast<size_t>(
                    swprintf_s(parts + pos, std::size(parts) - pos, L"unkFlags=0x%08X", unk));
            }
            if (!f)
                wcscpy_s(parts, std::size(parts), L"(dwFlags=0, none)");

            swprintf_s(
                detail,
                detailChars,
                L"dwFlags=0x%08X [%s]",
                f,
                parts[0] ? parts : L"");
        }

        HRESULT WINAPI HookDwmEnableBlurBehindWindow(HWND hWnd, const DWM_BLURBEHIND* pBlurBehind)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDwmEnableBlurBehindWindow(hWnd, pBlurBehind);
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t cls[96]{};
            if (hWnd && IsWindow(hWnd))
            {
                if (!GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))))
                    wcscpy_s(cls, L"?");
            }
            else
            {
                wcscpy_s(cls, L"?");
            }

            wchar_t bbDetail[400]{};
            FormatBlurBehindDetail(pBlurBehind, bbDetail, std::size(bbDetail));

            const HRESULT hr = g_trueDwmEnableBlurBehindWindow(hWnd, pBlurBehind);

            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][DebugDetour] DwmEnableBlurBehindWindow outerCaller=%s retAddrMod=%s hwnd=%p class=\"%s\" %s hr=0x%08X",
                outerMod,
                retAddrMod,
                hWnd,
                cls,
                bbDetail,
                static_cast<unsigned int>(hr));

            return hr;
        }

        HRESULT WINAPI HookDwmSetWindowAttribute(
            HWND hwnd,
            DWORD dwAttribute,
            LPCVOID pvAttribute,
            DWORD cbAttribute)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);
            // Тест сохранения анимации: Mica(7)→Acrylic(6) для окон проводника.
            // В терминах DWM: Mica = DWMSBT_MAINWINDOW(2), Acrylic = DWMSBT_TRANSIENTWINDOW(3).
            //
            // Два сценария:
            // 1. DWMWA_SYSTEMBACKDROP_TYPE при открытии/настройке окна — подменяем MAINWINDOW→TRANSIENTWINDOW.
            // 2. DWMWA_CAPTION_COLOR при смене фокуса (activate/deactivate) — explorerframe не повторяет
            //    backdrop; вручную инжектируем DWMSBT_TRANSIENTWINDOW после оригинального вызова.
            //    (g_trueDwmSetWindowAttribute — оригинал; рекурсии нет)

            const bool isExplorer = IsExplorerShellHostFrame(hwnd);

            // Путь 1: подмена backdrop при установке типа.
            LPCVOID pvSend = pvAttribute;
            DWORD localBackdrop{};
            bool remappedMicaToAcrylic = false;

            if (dwAttribute == DWMWA_SYSTEMBACKDROP_TYPE &&
                pvAttribute && cbAttribute >= sizeof(DWORD) && isExplorer)
            {
                const auto* const p = static_cast<const BYTE*>(pvAttribute);
                const DWORD v =
                    static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8u) |
                    (static_cast<DWORD>(p[2]) << 16u) | (static_cast<DWORD>(p[3]) << 24u);
                if (v == static_cast<DWORD>(DWMSBT_MAINWINDOW))
                {
                    localBackdrop = static_cast<DWORD>(DWMSBT_TRANSIENTWINDOW);
                    pvSend = &localBackdrop;
                    remappedMicaToAcrylic = true;
                }
            }

            HRESULT const hr =
                g_trueDwmSetWindowAttribute(hwnd, dwAttribute, pvSend, cbAttribute);

            // Путь 2: инжект backdrop при смене фокуса (CAPTION_COLOR от explorerframe).
            // Вызываем ПОСЛЕ оригинала — DWM сначала получает обновление заголовка.
            bool injectedBackdropOnFocus = false;
            if (dwAttribute == DWMWA_CAPTION_COLOR && isExplorer && SUCCEEDED(hr))
            {
                const DWORD acrylicType = static_cast<DWORD>(DWMSBT_TRANSIENTWINDOW);
                const HRESULT hrInject = g_trueDwmSetWindowAttribute(
                    hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &acrylicType, sizeof(acrylicType));
                injectedBackdropOnFocus = true;
                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCore][DebugDetour] DwmSetWindowAttribute +backdrop_inject hwnd=%p "
                    L"DWMSBT_TRANSIENTWINDOW(Acrylic) hrInject=0x%08X",
                    hwnd,
                    hrInject);
            }

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t cls[96]{};
            if (hwnd && IsWindow(hwnd))
            {
                if (!GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))))
                    wcscpy_s(cls, L"?");
            }
            else
            {
                wcscpy_s(cls, L"?");
            }

            wchar_t attrTag[140]{};
            DwmAttributeTag(dwAttribute, attrTag, std::size(attrTag));
            wchar_t detail[400]{};
            BuildDwmSetWindowAttributeDetail(dwAttribute, pvAttribute, cbAttribute, detail, std::size(detail));

            wchar_t remapNote[128]{};
            if (remappedMicaToAcrylic)
                wcscpy_s(remapNote, L" | applied=DWMSBT_TRANSIENTWINDOW(Acrylic) insteadOf DWMSBT_MAINWINDOW(Mica)");
            else if (injectedBackdropOnFocus)
                wcscpy_s(remapNote, L" | +backdrop_inject");

            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][DebugDetour] DwmSetWindowAttribute outerCaller=%s retAddrMod=%s hwnd=%p class=\"%s\" %s "
                L"cb=%u detail=%s%s hr=0x%08X",
                outerMod,
                retAddrMod,
                hwnd,
                cls,
                attrTag,
                cbAttribute,
                detail,
                remapNote,
                hr);

            return hr;
        }

        BOOL WINAPI HookSetWindowCompositionAttribute(HWND hwnd, SwcaAttribData* pData)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueSetWindowCompositionAttribute(hwnd, pData);

            const DWORD dwAttr = pData ? pData->dwAttribute : 0;
            PVOID const pv = pData ? pData->pvData : nullptr;
            const SIZE_T cb = pData ? pData->cbData : 0;

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t cls[96]{};
            if (hwnd && IsWindow(hwnd))
            {
                if (!GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))))
                    wcscpy_s(cls, L"?");
            }
            else
            {
                wcscpy_s(cls, L"?");
            }

            wchar_t attrTag[180]{};
            if (pData)
                FormatWcaAttrib(dwAttr, attrTag, std::size(attrTag));
            else
                wcscpy_s(attrTag, L"pData=null");

            wchar_t detail[400]{};
            if (pData)
                BuildSetWindowCompositionAttributeDetail(dwAttr, pv, cb, detail, std::size(detail));
            else
                wcscpy_s(detail, L"-");

            const BOOL ok = g_trueSetWindowCompositionAttribute(hwnd, pData);

            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][DebugDetour] SetWindowCompositionAttribute outerCaller=%s retAddrMod=%s hwnd=%p "
                L"class=\"%s\" %s detail=%s ok=%d",
                outerMod,
                retAddrMod,
                hwnd,
                cls,
                attrTag,
                detail,
                static_cast<int>(ok));

            return ok;
        }

        HRESULT WINAPI HookGetThemeColor(
            HTHEME hTheme,
            int iPartId,
            int iStateId,
            int iPropId,
            COLORREF* pColor)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueGetThemeColor(hTheme, iPartId, iStateId, iPropId, pColor);
            // Call original first to obtain resulting color and HRESULT
            COLORREF returnedColor{};
            HRESULT hr = g_trueGetThemeColor(hTheme, iPartId, iStateId, iPropId, pColor);
            if (SUCCEEDED(hr) && pColor)
                returnedColor = *pColor;

            // Outer caller (first meaningful frame outside GlassCore)
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));

            // Immediate caller module (module containing return address)
            wchar_t callerMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), callerMod, std::size(callerMod));

            // Theme class name
            WCHAR themeClass[MAX_PATH + 1]{};
            HRESULT hrClass = Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH);
            if (FAILED(hrClass))
                wcscpy_s(themeClass, L"?");

            wchar_t colorBuf[64]{};
            if (!pColor)
            {
                wcscpy_s(colorBuf, L"(null)");
            }
            else if (FAILED(hr))
            {
                swprintf_s(colorBuf, L"FAILED_0x%08X", static_cast<unsigned>(hr));
            }
            else
            {
                swprintf_s(colorBuf, L"0x%06X", returnedColor & 0xFFFFFF);
            }

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] GetThemeColor outerCaller=%s caller=%s hTheme=%p class=\"%s\" part=%d state=%d prop=%d hr=0x%08X color=%s",
                outerMod,
                callerMod,
                hTheme,
                themeClass,
                iPartId,
                iStateId,
                iPropId,
                hr,
                colorBuf);
            
            return hr;
        }

        LONG_PTR WINAPI HookSetClassLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueSetClassLongPtrW(hWnd, nIndex, dwNewLong);
            const LONG_PTR prev = g_trueSetClassLongPtrW(hWnd, nIndex, dwNewLong);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t cls[96]{};
            if (hWnd && IsWindow(hWnd))
            {
                if (!GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))))
                    wcscpy_s(cls, L"?");
            }
            else
            {
                wcscpy_s(cls, L"?");
            }

            wchar_t idxTag[112]{};
            FormatClassLongIndex(nIndex, idxTag, std::size(idxTag));

            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][DebugDetour] SetClassLongPtrW outerCaller=%s retAddrMod=%s hwnd=%p class=\"%s\" %s "
                L"newVal=0x%llX prevRet=0x%llX",
                outerMod,
                retAddrMod,
                hWnd,
                cls,
                idxTag,
                static_cast<unsigned long long>(static_cast<ULONG_PTR>(dwNewLong)),
                static_cast<unsigned long long>(static_cast<ULONG_PTR>(prev)));

            return prev;
        }

        COLORREF WINAPI HookSetDCBrushColor(HDC hdc, COLORREF color)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueSetDCBrushColor(hdc, color);
            const COLORREF prev = g_trueSetDCBrushColor(hdc, color);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] SetDCBrushColor outerCaller=%s retAddrMod=%s hdc=%p color=0x%08X prevColor=0x%08X %s",
                outerMod,
                retAddrMod,
                hdc,
                color,
                prev,
                winBuf);

            return prev;
        }

        COLORREF WINAPI HookSetBkColor(HDC hdc, COLORREF color)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueSetBkColor(hdc, color);
           
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] SetBkColor outerCaller=%s retAddrMod=%s hdc=%p color=0x%08X %s",
                outerMod,
                retAddrMod,
                hdc,
                color,
                winBuf);
            if (GetCallerModule(_ReturnAddress()) == GetModuleHandleW(L"user32.dll"))
                color = 0;
            return g_trueSetBkColor(hdc, color);
        }

        HBRUSH WINAPI HookCreateSolidBrush(COLORREF cr)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueCreateSolidBrush(cr);
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] CreateSolidBrush outerCaller=%s retAddrMod=%s color=0x%08X R=%u G=%u B=%u",
                outerMod,
                retAddrMod,
                cr,
                static_cast<unsigned>(GetRValue(cr)),
                static_cast<unsigned>(GetGValue(cr)),
                static_cast<unsigned>(GetBValue(cr)));
            //cr = 0;
            return g_trueCreateSolidBrush(cr);
        }

        HBRUSH WINAPI HookGetSysColorBrush(int nIndex)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueGetSysColorBrush(nIndex);
            HBRUSH const h = g_trueGetSysColorBrush(nIndex);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t idxTag[112]{};
            FormatSysColorIndexTag(nIndex, idxTag, std::size(idxTag));

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] GetSysColorBrush outerCaller=%s retAddrMod=%s %s hbr=%p",
                outerMod,
                retAddrMod,
                idxTag,
                h);
            if (nIndex == COLOR_WINDOW) return GetStockBrush(BLACK_BRUSH);
            return h;
        }

        HGDIOBJ WINAPI HookGetStockObject(int fnObject)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueGetStockObject(fnObject);
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t objTag[112]{};
            FormatStockObjectTag(fnObject, objTag, std::size(objTag));

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] GetStockObject outerCaller=%s retAddrMod=%s fnObject=%d %s",
                outerMod,
                retAddrMod,
                fnObject,
                objTag);
            //if (fnObject == WHITE_BRUSH) fnObject = BLACK_BRUSH;
            return g_trueGetStockObject(fnObject);
        }

        COLORREF WINAPI HookGetSysColor(int nIndex)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueGetSysColor(nIndex);
            
            const COLORREF cr = g_trueGetSysColor(nIndex);

            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            const wchar_t* const colorName = SysColorIndexName(nIndex);

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] GetSysColor outerCaller=%s retAddrMod=%s nIndex=%d name=%s "
                L"color=0x%08X R=%u G=%u B=%u",
                outerMod,
                retAddrMod,
                nIndex,
                colorName ? colorName : L"?",
                cr,
                static_cast<unsigned>(GetRValue(cr)),
                static_cast<unsigned>(GetGValue(cr)),
                static_cast<unsigned>(GetBValue(cr)));
            //if (nIndex == COLOR_BTNFACE) return 0;
            return cr;
        }

        int WINAPI HookSetDIBits(
            HDC hdc,
            HBITMAP hbm,
            UINT uStartScan,
            UINT cScanLines,
            CONST VOID* lpvBits,
            CONST BITMAPINFO* lpbmi,
            UINT fuColorUse)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
                return g_trueSetDIBits(hdc, hbm, uStartScan, cScanLines, lpvBits, lpbmi, fuColorUse);
            wchar_t outerMod[64]{};
            GetOuterCallerModuleBaseName(outerMod, std::size(outerMod));
            wchar_t retAddrMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retAddrMod, std::size(retAddrMod));
            wchar_t winBuf[160]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));

            int biW = 0;
            int biH = 0;
            WORD biBitCount = 0;
            if (lpbmi)
            {
                biW = lpbmi->bmiHeader.biWidth;
                biH = lpbmi->bmiHeader.biHeight;
                biBitCount = lpbmi->bmiHeader.biBitCount;
            }

            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] SetDIBits outerCaller=%s retAddrMod=%s hdc=%p hbm=%p "
                L"startScan=%u cLines=%u ColorUse=%u bmi=%dx%d bpp=%u bits=%p %s",
                outerMod,
                retAddrMod,
                hdc,
                hbm,
                uStartScan,
                cScanLines,
                fuColorUse,
                biW,
                biH,
                static_cast<unsigned>(biBitCount),
                lpvBits,
                winBuf);
            
            return g_trueSetDIBits(
                hdc, hbm, uStartScan, cScanLines, lpvBits, lpbmi, fuColorUse);
        }

        void HookDuiPaintBackground(
            void* pThis,
            HDC hdc,
            void* pValue,
            const RECT* pRect1,
            const RECT* pRect2,
            const RECT* pRect3,
            const RECT* pRect4)
        {
            if (CallerModuleIsGlassCore(_ReturnAddress()))
            {
                g_trueDuiPaintBackground(pThis, hdc, pValue, pRect1, pRect2, pRect3, pRect4);
                return;
            }
            // Сначала оригинал: тяжёлый лог до вызова давал реентрантность/риск в проводнике; FileBrows вызывает оригинал
            // после своих правок, но для трассировки порядок «orig → log» безопаснее.
            g_trueDuiPaintBackground(pThis, hdc, pValue, pRect1, pRect2, pRect3, pRect4);

            wchar_t retMod[64]{};
            GetModuleBaseName(GetCallerModule(_ReturnAddress()), retMod, std::size(retMod));
            wchar_t winBuf[200]{};
            FormatHdcWindowSuffix(hdc, winBuf, std::size(winBuf));
            wchar_t r1[48]{};
            wchar_t r2[48]{};
            wchar_t r3[48]{};
            wchar_t r4[48]{};
            FormatRectShort(r1, std::size(r1), pRect1);
            FormatRectShort(r2, std::size(r2), pRect2);
            FormatRectShort(r3, std::size(r3), pRect3);
            FormatRectShort(r4, std::size(r4), pRect4);
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][DebugDetour] DirectUI::Element::PaintBackground retAddrMod=%s this=%p hdc=%p pValue=%p "
                L"r1=%s r2=%s r3=%s r4=%s %s",
                retMod,
                pThis,
                hdc,
                pValue,
                r1,
                r2,
                r3,
                r4,
                winBuf);
        }
    } // namespace

    HRESULT SetEnabled(uint64_t mask, bool enable) noexcept
    {
        auto lock = g_lock.lock_exclusive();
        DebugLog(L"[GlassCore][DebugDetour] SetEnabled");
        try
        {
            return Hooks::Detours::Write([&]() {
                for (size_t i = 0; i < kSlotCount; ++i)
                {
                    const uint64_t bit = 1ull << static_cast<uint32_t>(i);
                    if ((mask & bit) == 0)
                        continue;

                    if (enable)
                    {
                        if (g_ref[i] == 0)
                            AttachSlot(i);
                        g_ref[i]++;
                    }
                    else
                    {
                        if (g_ref[i] > 0)
                        {
                            g_ref[i]--;
                            if (g_ref[i] == 0)
                                DetachSlot(i);
                        }
                    }
                }
            });
        }
        catch (...)
        {
            return wil::ResultFromCaughtException();
        }
    }
}

#endif // _DEBUG

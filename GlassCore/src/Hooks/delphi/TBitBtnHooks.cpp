// VCL TBitBtn: owner-draw session and scoped EXE IAT detours.
#include "CorePch.h"
#include <Hooks/delphi/TBitBtnHooks.h>
#include <Hooks/delphi/TBitBtnBridge.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Core/Intercept/HookHelper.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <array>
#include <vector>

using namespace std::literals;
namespace GlassCore::Button
{
    // VCL TBitBtn использует собственный glyph при BS_OWNERDRAW, но обычно
    // не выставляет BS_BITMAP/BS_ICON: по одному стилю нельзя исключить картинку.
    [[nodiscard]] bool IsDelphiBitBtn(HWND hwnd) noexcept
    {
        wchar_t cls[64]{};
        return hwnd && GetClassNameW(hwnd, cls, _countof(cls))
            && _wcsicmp(cls, L"TBitBtn") == 0;
    }

#if defined(_DEBUG)
    void LogDelphiBitBtnBeginPaint(HWND hWnd, HMODULE callerMod, size_t depth) noexcept
    {
        WCHAR cls[64]{};
        if (GetClassNameW(hWnd, cls, _countof(cls)) && _wcsicmp(cls, L"TBitBtn") == 0)
        {
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 16; }
            if (budget)
            {
                --budget;
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBitBtn] BeginPaint hwnd=0x%p style=0x%08X caller=0x%p depth=%zu",
                    hWnd, static_cast<unsigned>(GetWindowLongW(hWnd, GWL_STYLE)), callerMod,
                    depth);
            }
        }
    }
#endif

    // WM_DRAWITEM даёт точный HDC, в том числе временный comctl32 memory DC.
    // Сессия ограничена вызовом DefSubclassProc родителя, а не эвристикой
    // WindowFromDC / чужим или кэшированным compatible DC.
    struct DelphiBitBtnDrawSession
    {
        HWND hwnd;
        HDC dc;
        RECT rect;
        HMODULE module;
        std::array<bool, 3> acquired{};
#if defined(_DEBUG)
        std::array<bool, 3> slotFound{};
#endif
    };
    static thread_local std::vector<DelphiBitBtnDrawSession> g_delphiBitBtnDrawStack;

#if defined(_DEBUG)
    // Только чтение PE импортов. Слот означает наличие записи, не факт вызова.
    [[nodiscard]] static bool HasDelphiThemeImport(HMODULE mod, LPCSTR name) noexcept
    {
        for (const auto dll : { "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv,
                                "api-ms-win-uxtheme-l1-1-0.dll"sv })
        {
            if (Hooks::GetIAT(mod, dll, name) || Hooks::GetDelayloadIAT(mod, dll, name).second)
                return true;
        }
        return false;
    }
#endif

    bool BeginDelphiBitBtnOwnerDraw(HWND parent, const DRAWITEMSTRUCT* dis) noexcept
    {
        if (IsButtonHookShutdown() || !GlassCore::Root::IsFullInjectionMode()
            || !dis || dis->CtlType != ODT_BUTTON || !dis->hwndItem || !dis->hDC
            || GetParent(dis->hwndItem) != parent || IsRectEmpty(&dis->rcItem)
            || GlassShared::GlassGetWindowKind(dis->hwndItem) != GlassShared::GlassControlKind::Button
            || !GlassShared::GlassIsBackdropAllowedForHwnd(dis->hwndItem)) return false;
        wchar_t cls[64]{};
        if (!GetClassNameW(dis->hwndItem, cls, _countof(cls)) || _wcsicmp(cls, L"TBitBtn") != 0
            || (GetWindowLongW(dis->hwndItem, GWL_STYLE) & BS_TYPEMASK) != BS_OWNERDRAW)
            return false;
        HMODULE module = GlassCore::ModuleHandler::g_entryPointModule;
        if (!module) return false;
        DelphiBitBtnDrawSession session{ dis->hwndItem, dis->hDC, dis->rcItem, module };
        for (size_t i = 0; i < session.acquired.size(); ++i)
        {
#if defined(_DEBUG)
            session.acquired[i] = AcquireDelphiBitBtnHook(module, i, &session.slotFound[i]);
#else
            session.acquired[i] = AcquireDelphiBitBtnHook(module, i, nullptr);
#endif
        }
        g_delphiBitBtnDrawStack.push_back(session);
#if defined(_DEBUG)
        // Импорты EXE неизменны за жизнь процесса. Сканируем один раз, но
        // выводим кеш вместе с каждой сессией: пользователь может включить
        // логирование после первой отрисовки кнопки.
        static const std::array<bool, 4> namedImports{
            HasDelphiThemeImport(module, "DrawThemeBackground"),
            HasDelphiThemeImport(module, "DrawThemeBackgroundEx"),
            HasDelphiThemeImport(module, "DrawThemeParentBackground"),
            HasDelphiThemeImport(module, "DrawThemeParentBackgroundEx")
        };
        DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBitBtn] DrawItem begin hwnd=0x%p dc=0x%p action=0x%X theme=%d text=%d parent=%d slotTheme=%d slotText=%d slotParent=%d importDtb=%d importDtbEx=%d importParent=%d importParentEx=%d",
            session.hwnd, session.dc, dis->itemAction, session.acquired[0], session.acquired[1], session.acquired[2],
            session.slotFound[0], session.slotFound[1], session.slotFound[2],
            namedImports[0], namedImports[1], namedImports[2], namedImports[3]);
#endif
        return true;
    }

    void EndDelphiBitBtnOwnerDraw() noexcept
    {
        if (g_delphiBitBtnDrawStack.empty()) return;
        const auto session = g_delphiBitBtnDrawStack.back();
        g_delphiBitBtnDrawStack.pop_back();
        for (size_t i = 0; i < session.acquired.size(); ++i)
            if (session.acquired[i]) ReleaseDelphiBitBtnHook(session.module, i);
    }

    [[nodiscard]] static const DelphiBitBtnDrawSession* GetDelphiBitBtnDraw(HDC dc) noexcept
    {
        if (IsButtonHookShutdown() || g_delphiBitBtnDrawStack.empty()) return nullptr;
        const auto& s = g_delphiBitBtnDrawStack.back();
        return dc && s.dc == dc ? &s : nullptr;
    }

    [[nodiscard]] static bool IsDelphiBitBtnTheme(HTHEME theme, int part) noexcept
    {
        WCHAR name[MAX_PATH + 1]{};
        return theme && part == BP_PUSHBUTTON
            && SUCCEEDED(Shared::ThemeHelper::GetThemeClass(theme, name, MAX_PATH))
            && _wcsicmp(name, L"Button") == 0;
    }

    HRESULT WINAPI DelphiBitBtnThemeBg(HTHEME theme, HDC dc, int part, int state, LPCRECT rect, LPCRECT clip)
    {
        auto orig = GetDelphiBitBtnThemeOriginal(GlassCore::ModuleHandler::g_entryPointModule);
        if (!orig) return DrawThemeBackground(theme, dc, part, state, rect, clip);
#if defined(_DEBUG)
        if (!g_delphiBitBtnDrawStack.empty())
        {
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 24; }
            if (budget)
            {
                --budget;
                const auto session = g_delphiBitBtnDrawStack.back();
                const RECT r = rect ? *rect : RECT{};
                WCHAR themeClass[MAX_PATH + 1]{};
                const HRESULT themeHr = theme ? Shared::ThemeHelper::GetThemeClass(theme, themeClass, MAX_PATH) : E_INVALIDARG;
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiBitBtn] DTB entry hwnd=0x%p dc=0x%p itemDc=0x%p matched=%d part=%d state=%d themeHr=0x%08X theme=%s rect=[%ld,%ld,%ld,%ld] item=[%ld,%ld,%ld,%ld]",
                    session.hwnd, dc, session.dc, dc && session.dc == dc ? 1 : 0, part, state, static_cast<unsigned>(themeHr),
                    SUCCEEDED(themeHr) ? themeClass : L"?", r.left, r.top, r.right, r.bottom,
                    session.rect.left, session.rect.top, session.rect.right, session.rect.bottom);
            }
        }
#endif
        const auto* current = GetDelphiBitBtnDraw(dc);
        if (!current) return orig(theme, dc, part, state, rect, clip);
        const auto session = *current; // вложенный WM_DRAWITEM может перевыделить стек
        RECT overlap{};
        if (!rect || !IntersectRect(&overlap, rect, &session.rect) || !IsDelphiBitBtnTheme(theme, part))
            return orig(theme, dc, part, state, rect, clip);
        RECT drawClip = session.rect;
        if (clip && !IntersectRect(&drawClip, &session.rect, clip)) return S_OK;
        const int saved = SaveDC(dc);
        if (!saved) return orig(theme, dc, part, state, rect, clip);
        // Учитываем и clip вызывающего, и rcItem: alpha renderer копирует
        // исходный rect целиком, поэтому ограничить только его source недостаточно.
        if (IntersectClipRect(dc, drawClip.left, drawClip.top, drawClip.right, drawClip.bottom) == ERROR)
        {
            RestoreDC(dc, saved);
            return orig(theme, dc, part, state, rect, clip);
        }
        auto restore = wil::scope_exit([&]() noexcept { RestoreDC(dc, saved); });
#if defined(_DEBUG)
        DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBitBtn] DrawThemeBackground hwnd=0x%p dc=0x%p part=%d state=%d", session.hwnd, dc, part, state);
#endif
        return InvokeDelphiBitBtnThemeBackground(orig, session.module, session.hwnd, theme, dc, part, state, rect, clip);
    }

    HRESULT WINAPI DelphiBitBtnThemeText(HTHEME theme, HDC dc, int part, int state, LPCWSTR text,
        int count, DWORD flags, LPRECT rect, const DTTOPTS* options)
    {
        auto orig = GetDelphiBitBtnTextOriginal(GlassCore::ModuleHandler::g_entryPointModule);
        if (!orig) return DrawThemeTextEx(theme, dc, part, state, text, count, flags, rect, options);
        const auto* current = GetDelphiBitBtnDraw(dc);
        if (!current) return orig(theme, dc, part, state, text, count, flags, rect, options);
        const auto session = *current;
        RECT overlap{};
        if (!text || count == 0 || !rect || IsRectEmpty(rect)
            || !IntersectRect(&overlap, rect, &session.rect) || !IsDelphiBitBtnTheme(theme, part)
            || (flags & DT_CALCRECT)
            || (options && (options->dwSize != sizeof(DTTOPTS)
                || (options->dwFlags & (DTT_CALCRECT | DTT_CALLBACK)))))
            return orig(theme, dc, part, state, text, count, flags, rect, options);
        DTTOPTS opts{ sizeof(DTTOPTS) };
        if (options) opts = *options;
        opts.dwFlags |= DTT_COMPOSITED;
        HRESULT drawResult = E_FAIL;
        const HRESULT bufferResult = GlassCore::Text::DrawThemeContent(
            dc, *rect, &session.rect, nullptr, 0,
            [&](HDC mem, HPAINTBUFFER, RGBQUAD*, int) {
                RECT work = *rect;
                drawResult = DrawThemeTextEx(theme, mem, part, state, text, count, flags, &work, &opts);
            }, 255, false, true, false);
#if defined(_DEBUG)
        DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBitBtn] DrawThemeTextEx hwnd=0x%p dc=0x%p part=%d state=%d buffer=0x%08X draw=0x%08X",
            session.hwnd, dc, part, state, static_cast<unsigned>(bufferResult), static_cast<unsigned>(drawResult));
#endif
        return SUCCEEDED(bufferResult) && SUCCEEDED(drawResult) ? drawResult
            : orig(theme, dc, part, state, text, count, flags, rect, options);
    }

    HRESULT WINAPI DelphiBitBtnParentBg(HWND hwnd, HDC dc, const RECT* rect)
    {
        auto orig = GetDelphiBitBtnParentOriginal(GlassCore::ModuleHandler::g_entryPointModule);
        if (!orig) return DrawThemeParentBackground(hwnd, dc, rect);
        const auto* current = GetDelphiBitBtnDraw(dc);
        if (!current) return orig(hwnd, dc, rect);
        const auto session = *current;
        if (hwnd != session.hwnd) return orig(hwnd, dc, rect);
        RECT clear = session.rect;
        if (rect && !IntersectRect(&clear, &session.rect, rect)) return S_OK;
        const BOOL ok = PatBlt(dc, clear.left, clear.top, clear.right - clear.left, clear.bottom - clear.top, BLACKNESS);
#if defined(_DEBUG)
        DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBitBtn] DrawThemeParentBackground hwnd=0x%p dc=0x%p cleared=%d", hwnd, dc, ok);
#endif
        return ok ? S_OK : orig(hwnd, dc, rect);
    }

    void ClearDelphiBitBtnDraw() noexcept { g_delphiBitBtnDrawStack.clear(); }
}

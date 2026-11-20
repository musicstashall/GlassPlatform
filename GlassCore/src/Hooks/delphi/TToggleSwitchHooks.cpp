// VCL toggle paint safety and debug-only DC lifecycle tracing.
#include "CorePch.h"
#include <Hooks/delphi/TToggleSwitchHooks.h>
#include <DebugLog.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassBackdropMark.h>

namespace GlassCore::DelphiToggle
{
    bool PreservePaintDcBkColor(HWND hwnd, HDC dc, HMODULE caller) noexcept
    {
        if (!hwnd || !dc || caller != GlassCore::ModuleHandler::g_entryPointModule
            || !GlassCore::Root::IsFullInjectionMode()
            || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
            || WindowFromDC(dc) != hwnd
            || !GlassCore::PaintDispatcher::IsCurrentPaintDcForHwnd(hwnd, dc))
            return false;
        wchar_t cls[64]{};
        return GetClassNameW(hwnd, cls, _countof(cls))
            && _wcsicmp(cls, L"TToggleSwitch") == 0;
    }
#if defined(_DEBUG)
    // Контекст TToggleSwitch — только реально открытая BeginPaint-сессия,
    // а не HWND, унаследованный memory DC из ближайшей записи стека. Сам
    // факт нахождения в paint НЕ доказывает, кому принадлежит удаляемый DC.
    static thread_local bool g_loggingDelphiToggleDelete = false;
    bool IsToggleSwitch(HWND hwnd) noexcept
    {
        wchar_t cls[64]{};
        return hwnd && GetClassNameW(hwnd, cls, _countof(cls))
            && _wcsicmp(cls, L"TToggleSwitch") == 0;
    }

    DelphiToggleDeleteTrace CaptureDelphiToggleDelete(HWND paintHwnd, HDC dc) noexcept
    {
        DelphiToggleDeleteTrace trace{};
        if (!dc || !paintHwnd || g_loggingDelphiToggleDelete) return trace;
        trace.paintHwnd = paintHwnd;
        trace.dcType = GetObjectType(dc);
        if (trace.dcType != OBJ_MEMDC) { trace.paintHwnd = nullptr; return trace; }
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        static thread_local unsigned total = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 32; }
        if (!budget || total >= 128) { trace.paintHwnd = nullptr; return trace; }
        --budget;
        trace.serial = ++total;
        trace.bitmap = GetCurrentObject(dc, OBJ_BITMAP);
        trace.count = CaptureStackBackTrace(0, _countof(trace.frames), trace.frames, nullptr);
        return trace;
    }

    void LogDelphiToggleDelete(const DelphiToggleDeleteTrace& trace,
        HDC dc, HMODULE caller, BOOL result) noexcept
    {
        if (!trace.paintHwnd || g_loggingDelphiToggleDelete) return;
        g_loggingDelphiToggleDelete = true;
        auto resetLogging = wil::scope_exit([]() noexcept { g_loggingDelphiToggleDelete = false; });
        DebugLog(DBG_NORMAL,
            L"[GlassCore][DelphiToggleDC] DeleteDC seq=%u paintHwnd=0x%p dc=0x%p type=%u bitmap=0x%p caller=0x%p result=%d frames=%u (paint context, not DC ownership)",
            trace.serial, trace.paintHwnd, dc, static_cast<unsigned>(trace.dcType),
            trace.bitmap, caller, result, static_cast<unsigned>(trace.count));
        for (USHORT i = 0; i < trace.count; ++i)
        {
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(trace.frames[i]), &module);
            const auto address = reinterpret_cast<ULONG_PTR>(trace.frames[i]);
            const auto base = reinterpret_cast<ULONG_PTR>(module);
            wchar_t path[MAX_PATH]{};
            if (module) GetModuleFileNameW(module, path, _countof(path));
            const wchar_t* name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiToggleDC] frame seq=%u index=%u address=0x%p module=0x%p name=%s offset=0x%llX",
                trace.serial, static_cast<unsigned>(i), trace.frames[i], module, name,
                static_cast<unsigned long long>(module ? address - base : 0));
        }
    }
#endif

}

#pragma once
#include <windows.h>

namespace GlassCore::DirectUI
{
    // ── Lifecycle ────────────────────────────────────────────────────────────
    void Prepare();
    void Startup();
    void Shutdown();

    // ── Module notifications (вызываются из ModuleHandler) ───────────────────
    void OnDuiModuleLoaded(HMODULE hModule, const wchar_t* baseName);
    void OnDuiModuleUnloaded(HMODULE hModule, const wchar_t* baseName);

    // ── Provider callbacks (вызываются из WndCompat через GlassCoreProviders) ─
    // Public interface expected by Export table (HWND only)
    void OnDuiPaintBegin(HWND hWnd, HMODULE callerModule);
    void OnDuiPaintEnd(HWND hWnd, HMODULE callerModule);

    // Internal implementation with caller module info
    //void OnDuiPaintBeginImpl(HWND hWnd, HMODULE callerModule);
    //void OnDuiPaintEndImpl(HWND hWnd, HMODULE callerModule);

    // DirectUI root attach
    void OnDirectUIRootHooksAttach(HWND hDuiWnd) noexcept;

    // Internal guard (для хука DrawThemeBackgroundEx) ───────────────────────
    HWND GetPaintingDUI() noexcept;
    [[nodiscard]] bool IsShuttingDown() noexcept;
}

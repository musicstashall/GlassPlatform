#pragma once
#include <windows.h>

namespace GlassCore::DelphiToggle
{
    void Startup() noexcept;
    void Shutdown() noexcept;
#if defined(_DEBUG)
    struct DelphiToggleDeleteTrace
    {
        HWND paintHwnd = nullptr;
        HGDIOBJ bitmap = nullptr;
        DWORD dcType = 0;
        unsigned serial = 0;
        USHORT count = 0;
        void* frames[16]{};
    };
    bool IsToggleSwitch(HWND hwnd) noexcept;
    DelphiToggleDeleteTrace CaptureDelphiToggleDelete(HWND paintHwnd, HDC dc) noexcept;
    void LogDelphiToggleDelete(const DelphiToggleDeleteTrace& trace, HDC dc,
        HMODULE caller, BOOL result) noexcept;
#endif
}

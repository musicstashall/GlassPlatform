#pragma once
#include <Windows.h>

// Runtime-only ABI. Optional GlassSettings.Native; no import-library dependency.
namespace Shared::ReflectionProfiles
{
    inline constexpr DWORD Version = 1;
    inline constexpr wchar_t RegistryRoot[] = L"Software\\StanislavKa\\GlassPlatform\\GlassSettings\\Textured";
    inline constexpr wchar_t MutexName[] = L"Local\\GlassPlatform.ReflectionProfiles";
    struct Monitor { LONG x, y, width, height; UINT dpiX, dpiY; };
    struct Snapshot
    {
        DWORD size;
        DWORD version;
        LONG width, height;
        UINT count;
        Monitor monitors[32];
        wchar_t key[128];
    };
    using Capture = HRESULT (WINAPI*)(Snapshot*);
    using Signal = void (WINAPI*)();
    using Tick = HRESULT (WINAPI*)();
}

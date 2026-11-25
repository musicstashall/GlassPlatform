#pragma once
#include <Windows.h>
#include <ReflectionProfileApi.h>
#include <string>

// Only called by EntryRundll's host loop, never injected GlassCompat instances.
namespace ReflectionProfileHost
{
    inline HMODULE module = nullptr;
    inline Shared::ReflectionProfiles::Signal signal = nullptr;
    inline Shared::ReflectionProfiles::Tick tick = nullptr;
    inline void Stop() noexcept
    {
        signal = nullptr; tick = nullptr;
        if (module) FreeLibrary(module);
        module = nullptr;
    }
    inline void Start(HMODULE host) noexcept
    {
#ifndef _WIN64
        BOOL wow = FALSE;
        if (!IsWow64Process(GetCurrentProcess(), &wow) || wow) return; // one owner on x64 OS
#endif
        try
        {
            wchar_t path[32768]{};
            DWORD length = GetModuleFileNameW(host, path, ARRAYSIZE(path));
            if (!length || length >= ARRAYSIZE(path)) return;
            std::wstring directory(path, length);
            auto slash = directory.find_last_of(L"\\/");
            if (slash == std::wstring::npos) return;
            directory.resize(slash);
            // Optional installed Settings payload beside the already loaded platform.
            // No registry/CWD/PATH fallback and no installer policy change.
            std::wstring target = directory + L"\\GlassSettings\\GlassSettings.Native.dll";
            std::wstring probe = target;
            while (!probe.empty())
            {
                DWORD attributes = GetFileAttributesW(probe.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
                auto cut = probe.find_last_of(L"\\/");
                if (cut == std::wstring::npos || cut <= 2) break;
                probe.resize(cut);
            }
            module = LoadLibraryExW(target.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) return;
            length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
            if (!length || length >= ARRAYSIZE(path) || _wcsicmp(path, target.c_str())) { Stop(); return; }
            auto capture = GetProcAddress(module, "GlassSettings_CaptureReflectionProfileV1");
            signal = reinterpret_cast<Shared::ReflectionProfiles::Signal>(GetProcAddress(module, "GlassSettings_SignalReflectionDisplayChangeV1"));
            tick = reinterpret_cast<Shared::ReflectionProfiles::Tick>(GetProcAddress(module, "GlassSettings_TickReflectionProfilesV1"));
#ifndef _WIN64
            if (!capture) capture = GetProcAddress(module, "_GlassSettings_CaptureReflectionProfileV1@4");
            if (!signal) signal = reinterpret_cast<Shared::ReflectionProfiles::Signal>(GetProcAddress(module, "_GlassSettings_SignalReflectionDisplayChangeV1@0"));
            if (!tick) tick = reinterpret_cast<Shared::ReflectionProfiles::Tick>(GetProcAddress(module, "_GlassSettings_TickReflectionProfilesV1@0"));
#endif
            if (!capture || !signal || !tick) { Stop(); return; }
            Shared::ReflectionProfiles::Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
            if (FAILED(reinterpret_cast<Shared::ReflectionProfiles::Capture>(capture)(&snapshot))
                || snapshot.version != Shared::ReflectionProfiles::Version) { Stop(); return; }
            signal(); // Initial reconcile also when OG was already running.
        }
        catch (...) { Stop(); }
    }
    inline void OnMessage(UINT message) noexcept
    {
        if (signal && (message == WM_DISPLAYCHANGE || message == WM_DPICHANGED || message == WM_SETTINGCHANGE || message == WM_DEVICECHANGE
            || message == RegisterWindowMessageW(L"GlassPlatform.ReflectionProfileChanged.V1"))) signal();
    }
    inline void Poll() noexcept { if (tick) (void)tick(); }
}

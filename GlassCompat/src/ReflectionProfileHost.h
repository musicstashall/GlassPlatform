#pragma once
#include <Windows.h>
#include <ReflectionProfileApi.h>
#include <string>
#include <DebugLog.h>

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
        if (!IsWow64Process(GetCurrentProcess(), &wow) || wow)
        {
            DebugLog(DBG_NORMAL, L"[GlassCompat][ReflectionProfiles] optional helper skipped: not native OS architecture");
            return;
        } // one owner on x64 OS
#endif
        try
        {
            wchar_t path[32768]{};
            DWORD length = GetModuleFileNameW(host, path, ARRAYSIZE(path));
            if (!length || length >= ARRAYSIZE(path))
            {
                DebugLog(DBG_WARNING, L"[GlassCompat][ReflectionProfiles] host module path unavailable, error=%lu", GetLastError());
                return;
            }
            std::wstring directory(path, length);
            auto slash = directory.find_last_of(L"\\/");
            if (slash == std::wstring::npos) return;
            directory.resize(slash);
            // Optional installed Settings payload beside the already loaded platform.
            // No registry/CWD/PATH fallback and no installer policy change.
            std::wstring target = directory + L"\\GlassSettings\\GlassSettings.Native.dll";
#ifdef _DEBUG
            // Native.vcxproj's authoritative Debug output is flat. Prefer it
            // over a potentially stale copy beside the Settings executable.
            const std::wstring buildTarget = directory + L"\\GlassSettings.Native.dll";
            const DWORD buildAttributes = GetFileAttributesW(buildTarget.c_str());
            if (buildAttributes != INVALID_FILE_ATTRIBUTES)
                target = buildTarget;
            else
            {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                    return; // Don't bypass access errors with another payload.
            }
#endif
            std::wstring probe = target;
            while (!probe.empty())
            {
                DWORD attributes = GetFileAttributesW(probe.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                {
                    DebugLog(DBG_NORMAL, L"[GlassCompat][ReflectionProfiles] optional helper unavailable/unsafe: %ls attributes=0x%08lX error=%lu",
                        probe.c_str(), attributes, attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS);
                    return;
                }
                auto cut = probe.find_last_of(L"\\/");
                if (cut == std::wstring::npos || cut <= 2) break;
                probe.resize(cut);
            }
            module = LoadLibraryExW(target.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module)
            {
                DebugLog(DBG_WARNING, L"[GlassCompat][ReflectionProfiles] optional helper load failed: %ls error=%lu", target.c_str(), GetLastError());
                return;
            }
            length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
            if (!length || length >= ARRAYSIZE(path) || _wcsicmp(path, target.c_str())) { DebugLog(DBG_WARNING, L"[GlassCompat][ReflectionProfiles] optional helper rejected: loaded path mismatch"); Stop(); return; }
            auto capture = GetProcAddress(module, "GlassSettings_CaptureReflectionProfileV1");
            signal = reinterpret_cast<Shared::ReflectionProfiles::Signal>(GetProcAddress(module, "GlassSettings_SignalReflectionDisplayChangeV1"));
            tick = reinterpret_cast<Shared::ReflectionProfiles::Tick>(GetProcAddress(module, "GlassSettings_TickReflectionProfilesV1"));
#ifndef _WIN64
            if (!capture) capture = GetProcAddress(module, "_GlassSettings_CaptureReflectionProfileV1@4");
            if (!signal) signal = reinterpret_cast<Shared::ReflectionProfiles::Signal>(GetProcAddress(module, "_GlassSettings_SignalReflectionDisplayChangeV1@0"));
            if (!tick) tick = reinterpret_cast<Shared::ReflectionProfiles::Tick>(GetProcAddress(module, "_GlassSettings_TickReflectionProfilesV1@0"));
#endif
            if (!capture || !signal || !tick)
            {
                DebugLog(DBG_WARNING, L"[GlassCompat][ReflectionProfiles] optional helper rejected: missing V1 exports path=%ls capture=%d signal=%d tick=%d",
                    target.c_str(), capture != nullptr, signal != nullptr, tick != nullptr);
                Stop(); return;
            }
            Shared::ReflectionProfiles::Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
            const HRESULT hr = reinterpret_cast<Shared::ReflectionProfiles::Capture>(capture)(&snapshot);
            if (FAILED(hr) || snapshot.version != Shared::ReflectionProfiles::Version)
            {
                DebugLog(DBG_WARNING, L"[GlassCompat][ReflectionProfiles] optional helper rejected: capture=0x%08lX version=%lu", hr, snapshot.version);
                Stop(); return;
            }
            DebugLog(DBG_IMPORTANT, L"[GlassCompat][ReflectionProfiles] helper ready: %ls profile=%ls monitors=%lu; startup reconcile scheduled",
                target.c_str(), snapshot.key, snapshot.count);
            signal(); // Initial reconcile also when OG was already running.
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[GlassCompat][ReflectionProfiles] optional helper skipped: unexpected loader failure");
            Stop();
        }
    }
    inline void OnMessage(UINT message) noexcept
    {
        if (signal && (message == WM_DISPLAYCHANGE || message == WM_DPICHANGED || message == WM_SETTINGCHANGE || message == WM_DEVICECHANGE
            || message == RegisterWindowMessageW(L"GlassPlatform.ReflectionProfileChanged.V1")))
        {
            DebugLog(DBG_NORMAL, L"[GlassCompat][ReflectionProfiles] signal message=0x%04X; reconcile debounced", message);
            signal();
        }
    }
    inline void Poll() noexcept
    {
#if defined(_DEBUG) && defined(GLASS_ENABLE_LOGGING)
        // Startup diagnostics can precede LogViewer capture. Keep a bounded
        // host-thread heartbeat; no extra observer, timer or DLL load attempt.
        static ULONGLONG nextStatus = 0;
        const ULONGLONG now = GetTickCount64();
        if (now >= nextStatus)
        {
            DebugLog(DBG_NORMAL, L"[GlassCompat][ReflectionProfiles] host loop alive; optional helper=%ls",
                tick ? L"ready" : L"inactive (absent/incompatible or non-native architecture); see startup log");
            nextStatus = now + 30000;
        }
#endif
        if (tick) (void)tick();
    }
}

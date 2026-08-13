#pragma once
#include <windows.h>
#include <ReloadRenderFlags.h>

// Имена объектов — суффикс .x64 / .x86 по битности модуля (паритет GlassPlatformTask.h).
#ifdef _WIN64
inline constexpr wchar_t kHostPidMappingName[] = L"Local\\GlassHostPid.x64";
inline constexpr wchar_t kInstanceMutexName[] = L"GlassPlatform.Instance.Mutex.x64";
inline constexpr wchar_t kStopEventName[] = L"GlassPlatform.StopEvent.x64";
#else
inline constexpr wchar_t kHostPidMappingName[] = L"Local\\GlassHostPid.x86";
inline constexpr wchar_t kInstanceMutexName[] = L"GlassPlatform.Instance.Mutex.x86";
inline constexpr wchar_t kStopEventName[] = L"GlassPlatform.StopEvent.x86";
#endif
// kHostShutdownEventName — GlassShared/include/GlassPlatformTask.h
// kReloadRenderSemaphoreName, kReloadGenerationMappingName — ReloadRenderFlags.h

[[nodiscard]] inline DWORD ReadHostPid() noexcept
{
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kHostPidMappingName);
    if (!hMap)
        return 0;
    DWORD pid = 0;
    void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(DWORD));
    if (pView)
    {
        pid = *static_cast<const DWORD*>(pView);
        UnmapViewOfFile(pView);
    }
    CloseHandle(hMap);
    return pid;
}

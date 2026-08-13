#include "WndPch.h"
#include "Root/RootBackdropRefreshThrottle.h"

#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex g_mutex;
    std::unordered_map<HWND, DWORD> g_lastApplyTick;
}

namespace WndCompat::RootBackdropRefreshThrottle
{
    void Erase(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        std::lock_guard lock(g_mutex);
        (void)g_lastApplyTick.erase(hRoot);
    }

    bool ShouldSkip(HWND hRoot, UINT throttleMs) noexcept
    {
        if (!hRoot || throttleMs == 0)
            return false;

        const DWORD now = GetTickCount();
        std::lock_guard lock(g_mutex);
        const auto it = g_lastApplyTick.find(hRoot);
        if (it != g_lastApplyTick.end())
        {
            const DWORD dt = now - it->second;
            if (dt < throttleMs)
                return true;
        }
        g_lastApplyTick[hRoot] = now;
        return false;
    }

    void Stamp(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        std::lock_guard lock(g_mutex);
        g_lastApplyTick[hRoot] = GetTickCount();
    }

    void SetRootSwcaApplyReasonForNextCall(const wchar_t* reason) noexcept
    {
        using Fn = void(WINAPI*)(const wchar_t*);
        static Fn pfn = nullptr;
        if (!pfn)
        {
            const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
            if (hCore)
            {
                pfn = reinterpret_cast<Fn>(
                    GetProcAddress(hCore, "GlassCore_SetNextRootSwcaApplyReason"));
            }
        }
        if (pfn)
            pfn(reason);
    }
}

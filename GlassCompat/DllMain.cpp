// DllMain.cpp (минимальный)
#include "CompatPch.h"
#include "GlassCompat.h"
#include <CrashGuard.h>
#include <ShortLivedAgent.h>
#include <Windows.h>

#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
#include <GlassLogClient.h>
#endif

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        if (!GlassCompat::IsShortLivedGlassCompatProcess())
        {
            GlassCompat::InstallProcessCrashGuard();
        }
        break;
    case DLL_PROCESS_DETACH:
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
        if (GlassCompat::IsShortLivedGlassCompatProcess())
            Shared::Log::GlassLogClient::Instance().Stop();
#endif
        if (GlassCompat::IsProcessCrashGuardInstalled())
            GlassCompat::UninstallProcessCrashGuard();
        break;
    }
    return TRUE;
}

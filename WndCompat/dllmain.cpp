#include "WndPch.h"

// Глобальный HMODULE для process‑wide CBT‑hook
HMODULE g_hModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
        return TRUE;

    case DLL_PROCESS_DETACH:
        // На всякий случай обнулим, чтобы не держать висячую ссылку
        g_hModule = nullptr;
        return TRUE;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        // Ничего не делаем — DisableThreadLibraryCalls уже вызван
        return TRUE;
    }

    return TRUE;
}

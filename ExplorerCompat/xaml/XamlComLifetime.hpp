#pragma once

#include <atomic>

// Счётчики для DllCanUnloadNow / LockServer (определение в DllGetClassObject.cpp).
namespace GlassExplorerCom
{
    extern std::atomic<long> g_objectCount;
    extern std::atomic<long> g_serverLocks;
    // Гейт новой работы после ExplorerCompat_StopXamlDiagnostics (SetSite/ретраи).
    // DllCanUnloadNow его не учитывает: живая ссылка, включая LVT-watcher,
    // обязана удерживать образ mapped до финального Release.
    extern std::atomic<bool> g_shutdownDone;
}

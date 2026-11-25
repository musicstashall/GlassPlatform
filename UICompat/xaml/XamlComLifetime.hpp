#pragma once
#include <atomic>

// Счётчики COM-объектов UICompat (изолированы от ExplorerCompat).
// Используются DllCanUnloadNow и Class Factory.
namespace UICompatCom
{
    extern std::atomic<long> g_objectCount;
    extern std::atomic<long> g_serverLocks;

    // Гейт новой работы после UICompat_StopXamlDiagnostics (SetSite/ретраи).
    // DllCanUnloadNow его не учитывает: живая ссылка, включая LVT-watcher,
    // обязана удерживать образ mapped до финального Release.
    extern std::atomic<bool> g_shutdownDone;
}

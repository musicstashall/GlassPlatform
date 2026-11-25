#pragma once
#include <atomic>

// Счётчики COM-объектов TaskbarCompat (изолированы от ExplorerCompat).
// Используются DllCanUnloadNow и Class Factory.
namespace TaskbarCompatCom
{
    extern std::atomic<long> g_objectCount;
    extern std::atomic<long> g_serverLocks;

    // Гейт новой работы после TaskbarCompat_Shutdown (SetSite/таймеры/ретраи)
    // и бит 32 TaskbarCompat_DebugComState. DllCanUnloadNow его НЕ учитывает:
    // живая COM-ссылка (в т.ч. watcher в LVT после неудачного Unadvise) держит
    // образ — выгрузка под ссылкой даёт AV (дамп explorer.exe.11524).
    extern std::atomic<bool> g_shutdownDone;
}

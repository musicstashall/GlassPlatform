#pragma once
#include <atomic>

// Счётчики COM-объектов UICompat (изолированы от ExplorerCompat).
// Используются DllCanUnloadNow и Class Factory.
namespace UICompatCom
{
    extern std::atomic<long> g_objectCount;
    extern std::atomic<long> g_serverLocks;

    // После корректного UICompat_Shutdown разрешаем COM выгрузить DLL,
    // даже если WinUI/UWP оболочка удерживает IUnknown на TAP-объекте
    // (все коллбэки нейтрализованы Unadvise + RestoreAll).
    extern std::atomic<bool> g_shutdownDone;
}

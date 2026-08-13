#pragma once

#include <atomic>

// Счётчики для DllCanUnloadNow / LockServer (определение в DllGetClassObject.cpp).
namespace GlassExplorerCom
{
    extern std::atomic<long> g_objectCount;
    extern std::atomic<long> g_serverLocks;
    // После корректного завершения XAML Diagnostics разрешаем DllCanUnloadNow=S_OK,
    // даже если WinUI удерживает IUnknown на TAP-объекте (все коллбэки нейтрализованы).
    extern std::atomic<bool> g_shutdownDone;
}

#pragma once
#include <cstdint>

// Битовые флаги результата выгрузки модулей из GlassCompat.
// Используется для диагностики последовательности shutdown внутри процесса.
enum class UnloadResult : uint32_t
{
    Success = 0,

    // Флаги post‑factum проверки со стороны GlassPlatform (Toolhelp).
    WndCompatLeak      = 1u << 0,
    GlassCoreLeak      = 1u << 1,
    ExplorerCompatLeak = 1u << 2,

    // Этапы ExecuteShutdown (внутри GlassCompat GUI‑потока).
    // WndCompat
    BeginWnd_NotLoaded  = 1u << 3,  // g_hWndCompat == nullptr
    BeginWnd_NotFound   = 1u << 4,  // экспорт WndCompat_BeginShutdown не найден
    BeginWnd_Called     = 1u << 5,  // вызов WndCompat_BeginShutdown начат
    BeginWnd_Returned   = 1u << 6,  // WndCompat_BeginShutdown вернулся

    // ExplorerCompat (только explorer.exe)
    ExplorerShutdown_Skipped  = 1u << 7,
    ExplorerShutdown_Called   = 1u << 8,
    ExplorerShutdown_Returned = 1u << 9,

    // GlassCore
    CoreShutdown_NotLoaded = 1u << 10, // g_hCore == nullptr
    CoreShutdown_NotFound  = 1u << 11, // экспорт GlassCore_Shutdown не найден
    CoreShutdown_Called    = 1u << 12,
    CoreShutdown_Returned  = 1u << 13,

    // FreeLibrary‑этапы (фиксируем, что поток вообще дошёл до этих точек).
    FreeCore_Called        = 1u << 14,
    FreeCore_Returned      = 1u << 15,
    FreeExplorer_Called    = 1u << 16,
    FreeExplorer_Returned  = 1u << 17,
    FreeWnd_Called         = 1u << 18,
    FreeWnd_Returned       = 1u << 19,
};

inline UnloadResult operator|(UnloadResult a, UnloadResult b)
{
    return static_cast<UnloadResult>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline UnloadResult& operator|=(UnloadResult& a, UnloadResult b)
{
    a = a | b;
    return a;
}

inline bool HasFlag(UnloadResult result, UnloadResult flag)
{
    return (static_cast<uint32_t>(result) & static_cast<uint32_t>(flag)) != 0;
}
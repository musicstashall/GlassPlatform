// Мост GlassSettings ↔ GlassShared::OpenGlassDwmGuard.
// Вся логика «сторожа» падений DWM живёт в GlassShared; здесь только C-экспорты для P/Invoke.
//
// Базовая линия dwm.exe снимается синхронно внутри Arm() — вызывать ДО старта OpenGlass.
// GlassSettings сам запускает Watched-цикл: Setup ждёт не более 33 с на странице «Готово»,
// поэтому параллельный сторож в GS обязателен — иначе после закрытия мастера никто не следит.
#include <OpenGlassDwmGuard.h>

#include <Windows.h>

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_OpenGlassDwmGuard_Arm(
    DWORD armedBy, const wchar_t* ogVersion) noexcept
{
    return Shared::OpenGlassDwmGuard::Arm(armedBy, ogVersion ? ogVersion : L"") ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void WINAPI GlassSettings_OpenGlassDwmGuard_Disarm() noexcept
{
    Shared::OpenGlassDwmGuard::Disarm();
}

extern "C" __declspec(dllexport) DWORD WINAPI GlassSettings_OpenGlassDwmGuard_QueryState() noexcept
{
    return Shared::OpenGlassDwmGuard::QueryState();
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_OpenGlassDwmGuard_IsWatching() noexcept
{
    return Shared::OpenGlassDwmGuard::IsWatching() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_OpenGlassDwmGuard_IsQuarantined() noexcept
{
    return Shared::OpenGlassDwmGuard::IsQuarantined() ? TRUE : FALSE;
}

/// true — автостарт OG запрещён (карантин активен). Побочный эффект: если установленная
/// версия OG СТРОГО НОВЕЕ записанной в маркере, карантин снимается автоматически
/// и функция возвращает false (новый шанс для свежей сборки).
extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_OpenGlassDwmGuard_ShouldSuppressOpenGlassStart(
    const wchar_t* installedOgVersion) noexcept
{
    return Shared::OpenGlassDwmGuard::ShouldSuppressOpenGlassStart(
        installedOgVersion ? installedOgVersion : L"") ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_OpenGlassDwmGuard_ClearQuarantine() noexcept
{
    return Shared::OpenGlassDwmGuard::ClearQuarantine() ? TRUE : FALSE;
}

/// Диагностика для UI: сколько крашей внутри окна и причина карантина (0 — нет карантина).
extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_OpenGlassDwmGuard_GetQuarantineInfo(
    DWORD* crashCountOut, DWORD* reasonOut) noexcept
{
    if (crashCountOut) *crashCountOut = 0;
    if (reasonOut) *reasonOut = 0;
    Shared::OpenGlassDwmGuard::MarkerInfo info{};
    if (!Shared::OpenGlassDwmGuard::ReadMarker(info))
        return FALSE;
    if (crashCountOut) *crashCountOut = info.crashCount;
    if (reasonOut) *reasonOut = info.quarantine != 0 ? info.reason : 0;
    return TRUE;
}

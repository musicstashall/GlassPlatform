#pragma once
#include <Windows.h>
#include <cstdint>

namespace Shared::DwmGlassOpacity
{
    /// Единая политика: проценты 0…100 в `HKCU\Software\Microsoft\Windows\DWM`; альфа тинта SWCA —
    /// `MergeGlassOpacityIntoArgb`. Запись из UI (LogViewer и др.) — только через Write*/Delete*/Restore* ниже.
    ///
    /// Рассылка `WM_DWMCOLORIZATIONCHANGED` (HWND_BROADCAST): вызывается после успешных Write*/Delete*/Restore*
    /// и после сохранения стиля backdrop Root (`…\BlackWindowManager\Root\EffectType`).
    /// При нескольких правках подряд — `BeginDwmColorizationNotifyBatch` / `EndDwmColorizationNotifyBatch`,
    /// чтобы отправить одно сообщение (например откат GlassOpacity + backdrop в диалоге параметров).

    /// Рассылка уведомления DWM (с учётом активного батча — см. Begin/End).
    void NotifyDwmColorizationChanged() noexcept;
    /// Host echo-guard: сообщение содержит wave-id, созданный этим экземпляром GlassShared.
    [[nodiscard]] bool IsGeneratedDwmColorizationWave(
        WPARAM colorizationWave,
        LPARAM messageLParam) noexcept;
    void BeginDwmColorizationNotifyBatch() noexcept;
    void EndDwmColorizationNotifyBatch() noexcept;

    /// Активное окно: значение реестра `HKCU\Software\Microsoft\Windows\DWM\GlassOpacity` (REG_DWORD), 0…100.
    /// Неактивное (если задано): `GlassOpacityInactive` — см. `MergeGlassOpacityIntoArgb`.
    /// При отсутствии ключа `GlassOpacity` чтение даёт 63 (как у системного поведения DWM).
    [[nodiscard]] std::uint32_t ReadGlassOpacityPercent0To100() noexcept;

    /// true, если в HKCU есть DWORD `GlassOpacity` (для снапшота отката диалога).
    [[nodiscard]] bool TryReadGlassOpacityRegistryValueIfPresent(std::uint32_t& out0_100) noexcept;

    /// GlassOpacityInactive; false — ключа нет.
    [[nodiscard]] bool TryReadGlassOpacityInactivePercent0To100(std::uint32_t& out0_100) noexcept;

    /// Подставляет альфу из GlassOpacity / GlassOpacityInactive в старший байт ARGB (RGB из `argb` сохраняются).
    [[nodiscard]] std::uint32_t MergeGlassOpacityIntoArgb(std::uint32_t argb, HWND hWnd) noexcept;

    void WriteGlassOpacityPercent0To100(int percent0_100) noexcept;
    void WriteGlassOpacityInactivePercent0To100(int percent0_100) noexcept;
    void DeleteGlassOpacityInactiveValue() noexcept;
    /// Удаляет только `GlassOpacity` (активное окно), если ключ нужно убрать при откате.
    void DeleteGlassOpacityValue() noexcept;

    /// Атомарно восстанавливает пару ключей DWM; уведомление — `NotifyDwmColorizationChanged`
    /// (внутри `BeginDwmColorizationNotifyBatch` … `End` — одна рассылка на группу с backdrop).
    void RestoreGlassOpacityRegistrySnapshot(
        bool glassOpacityKeyPresent,
        std::uint32_t glassOpacity0_100,
        bool glassOpacityInactiveKeyPresent,
        std::uint32_t glassOpacityInactive0_100) noexcept;
}

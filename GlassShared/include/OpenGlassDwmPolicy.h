#pragma once
#include <Windows.h>
#include <cstdint>

namespace Shared::OpenGlassDwmPolicy
{
    /// Порог сборки ОС: legacy MIL / классическое стекло OpenGlass в DWM недоступны (см. README OpenGlass).
    constexpr DWORD kBuildLegacyDwmGlassCutoff = 28000;

    enum class CapabilityTier : std::uint8_t
    {
        Unknown = 0,
        BlurSupported,
        EffectsWithoutLegacyBlur,
    };

    /// RtlGetVersion.dwBuildNumber vs kBuildLegacyDwmGlassCutoff.
    [[nodiscard]] CapabilityTier QueryOpenGlassCapabilityTier() noexcept;

    /// Окно класса Dwm (OpenGlass notification sink) в dwm.exe.
    [[nodiscard]] bool IsOpenGlassNotificationWindowPresent() noexcept;

    /// Pipeline: окно Dwm (+ опционально OpenGlass.dll в dwm.exe).
    [[nodiscard]] bool IsDwmGlassInjectedPipelinePresent() noexcept;

    /// Pipeline для политики Root backdrop: dll в dwm или окно Dwm при RUNNING orchestration.
    [[nodiscard]] bool IsOpenGlassBackdropPipelineActive() noexcept;

    /// Можно ли писать Composition/DWM policy: pipeline/модуль OG или зарегистрированы служба/задача.
    /// Без этого — no-op на любые записи в HKCU\Software\Microsoft\Windows\DWM.
    [[nodiscard]] bool ShouldApplyDwmCompositionPolicy() noexcept;

    /// Цепочка как в OpenGlass GlassKernel::Update (Backdrop): Override → ColorizationColor; Inactive → active.
    [[nodiscard]] bool TryReadEffectiveColorizationColors(DWORD& activeOut, DWORD& inactiveOut) noexcept;

    /// Platform defaults: Primary sync (gate OFF) + *Override refresh (gate ON). Batch notify внутри.
    /// Мягкий dark↔light flip (accent/waves). Возвращает true, если реестр менялся.
    [[nodiscard]] bool TryApplyPlatformCompositionDefaults() noexcept;

    /// Alias: TryApplyPlatformCompositionDefaults (устаревшее имя).
    [[nodiscard]] bool TrySyncColorizationWithWindowBackground() noexcept;

    /// Удаление парных *Inactive* / maximized из HKCU (кроме GlassOpacity / GlassOpacityInactive).
    void ResetInactiveDwmValuesOnVisualStyleChange() noexcept;

    /// Visual-style path: жёсткий Phase1 (delete Override) + Primary/Override + Notify.
    /// No-op если !ShouldApplyDwmCompositionPolicy().
    void ApplyDwmPolicyOnVisualStyleChange() noexcept;

    /// Реестр: отключить всю политику (HKCU …\GlassPlatform, значение OpenGlassDwmPolicyDisable = 1).
    [[nodiscard]] bool IsHostDwmPolicyDisabled() noexcept;

    /// Echo-guard: host policy пишет Override (GlassCore skip registry sync на colorization wave).
    [[nodiscard]] bool IsPolicyColorWriteActive() noexcept;
}

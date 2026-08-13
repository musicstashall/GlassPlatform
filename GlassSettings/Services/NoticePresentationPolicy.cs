using GlassSettings.Controls;

namespace GlassSettings.Services;

/// <summary>Политика показа notice: закрепление на странице vs плавающее зеркало.</summary>
internal static class NoticePresentationPolicy
{
    /// <summary>
    /// Зелёное уведомление без «Сохранить» и без action-кнопки — только плавающее зеркало, без полосы в layout.
    /// </summary>
    public static bool IsInformationalMirrorOnly(SettingsNoticeSeverity severity, bool showSave, bool showAction) =>
        severity == SettingsNoticeSeverity.Success && !showSave && !showAction;
}

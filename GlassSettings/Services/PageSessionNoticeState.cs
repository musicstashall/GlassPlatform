using GlassSettings.Core.Snapshot;

namespace GlassSettings.Services;

/// <summary>Dismiss notice на detail-страницах — сохраняется между входами; сброс только при новой мутации.</summary>
internal static class PageSessionNoticeState
{
    private static bool _dismissed;
    private static bool _dismissedWhileUnsaved;

    public static bool SaveSuccessNoticePending { get; private set; }

    public static SettingsCommitScope? SaveSuccessScope { get; private set; }

    /// <summary>Success показывается только после commit, не при ручном возврате к снимку.</summary>
    private static bool _pendingSuccessDisplay;

    public static bool IsSaveSuccessFor(SettingsCommitScope scope) =>
        SaveSuccessNoticePending
        && SaveSuccessScope is { } saved
        && SettingsCommitScopeComparer.Instance.Equals(saved, scope);

    public static bool ShouldHide(bool hasUnsavedInScope) =>
        _dismissed && _dismissedWhileUnsaved == hasUnsavedInScope;

    internal static string DescribeForTrace(bool hasUnsavedInScope) =>
        $"dismissed={_dismissed}, dismissedWhileUnsaved={_dismissedWhileUnsaved}, dirty={hasUnsavedInScope}, shouldHide={ShouldHide(hasUnsavedInScope)}";

    public static void Dismiss(bool hasUnsavedInScope)
    {
        _dismissed = true;
        _dismissedWhileUnsaved = hasUnsavedInScope;
    }

    public static void MarkSaveSucceeded(SettingsCommitScope scope)
    {
        SaveSuccessScope = scope;
        SaveSuccessNoticePending = true;
        _pendingSuccessDisplay = true;
        _dismissed = false;
    }

    /// <summary>Один раз после «Сохранить» — для ShowNoticeAsync / TryOpen.</summary>
    public static bool TryConsumePendingSuccessDisplay()
    {
        if (!_pendingSuccessDisplay)
            return false;

        _pendingSuccessDisplay = false;
        return true;
    }

    public static void ClearSaveSuccessPending()
    {
        SaveSuccessNoticePending = false;
        SaveSuccessScope = null;
        _pendingSuccessDisplay = false;
    }

    /// <summary>
    /// Вход на страницу: сброс только «Параметры сохранены» (не восстанавливать при возврате).
    /// Dismiss крестиком сохраняется.
    /// </summary>
    public static void ResetForNavigation() => ClearSaveSuccessPending();

    public static void OnUnsavedChanged(bool hasUnsavedInScope)
    {
        if (_dismissed && _dismissedWhileUnsaved != hasUnsavedInScope)
            _dismissed = false;
    }

    /// <summary>Сброс dismiss после новой мутации параметров при несохранённом scope.</summary>
    public static void InvalidateDismissOnMutation()
    {
        if (_dismissed && _dismissedWhileUnsaved)
            _dismissed = false;

        // Новая мутация = новый lifecycle ID зеркала (activity-dismiss старого ID не блокирует).
        NoticeViewportMirrorService.NotifyNewPinnedNoticeEpoch();
    }

    private static bool _animateNextOpen;

    public static void SuppressAnimateForNavigation() => _animateNextOpen = false;

    public static void RequestAnimateNextOpen() => _animateNextOpen = true;

    public static bool ConsumeAnimateNextOpen()
    {
        if (!_animateNextOpen)
            return false;

        _animateNextOpen = false;
        return true;
    }
}

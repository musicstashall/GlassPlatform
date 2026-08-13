using GlassSettings.Core.Snapshot;

namespace GlassSettings.Services;

/// <summary>Pending/dismiss зелёного external-sync notice (MirrorOnly, one-shot на странице).</summary>
internal static class PageExternalSyncNoticeState
{
    private static readonly HashSet<SettingsCommitScope> PendingScopes =
        new(SettingsCommitScopeComparer.Instance);

    private static readonly HashSet<SettingsCommitScope> DismissedScopes =
        new(SettingsCommitScopeComparer.Instance);

    private static bool _animateNextOpen;

    public static void MarkScopesSynced(IEnumerable<SettingsCommitScope> scopes)
    {
        foreach (var scope in scopes)
        {
            PendingScopes.Add(scope);
            DismissedScopes.Remove(scope);
        }

        _animateNextOpen = true;
    }

    public static bool ShouldShow(SettingsCommitScope scope) =>
        PendingScopes.Contains(scope) && !DismissedScopes.Contains(scope);

    public static bool IsPending(SettingsCommitScope scope) => PendingScopes.Contains(scope);

    /// <summary>Только ручной dismiss (крестик). Policy-hide не сбрасывает pending.</summary>
    public static void Dismiss(SettingsCommitScope scope)
    {
        DismissedScopes.Add(scope);
        PendingScopes.Remove(scope);
    }

    /// <summary>Вход на страницу: green one-shot — не помнить pending между страницами.</summary>
    public static void ClearPending()
    {
        PendingScopes.Clear();
        _animateNextOpen = false;
    }

    public static void RequestAnimateNextOpen() => _animateNextOpen = true;

    public static bool ConsumeAnimateNextOpen()
    {
        if (!_animateNextOpen)
            return false;

        _animateNextOpen = false;
        return true;
    }
}

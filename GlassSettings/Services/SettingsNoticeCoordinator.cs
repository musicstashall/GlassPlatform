using GlassSettings.Controls;
using GlassSettings.Core.Snapshot;
using GlassSettings.Pages;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Services;

/// <summary>Единый центр политики notice: слоты, бэклог, проекция на полосы и зеркало.</summary>
internal static class SettingsNoticeCoordinator
{
    private const int MaxVisibleSlots = 3;

    private static long _globalSequence;
    private static readonly Dictionary<object, HostNoticeState> _hosts = new(ReferenceEqualityComparer.Instance);

    internal enum NoticeSlotColor
    {
        Green,
        Yellow,
        Action,
        Error
    }

    internal enum NoticeEntryState
    {
        Visible,
        Hidden,
        Dismissed
    }

    internal enum NoticePresentMode
    {
        PinnedLayout,
        MirrorOnly
    }

    internal interface INoticePageHost
    {
        object HostKey { get; }
        bool IsHomeOverview { get; }
        ScrollViewer? ScrollViewer { get; }
        FrameworkElement? ViewportAnchor { get; }
        SettingsPageNoticeStack? NoticeStack { get; }
    }

    internal sealed class ShellNoticePageHost : INoticePageHost
    {
        public required SettingsPageNoticeStack Stack { get; init; }
        public required ScrollViewer? ScrollViewer { get; init; }
        public object HostKey => Stack;
        public bool IsHomeOverview => false;
        public FrameworkElement? ViewportAnchor => Stack.ViewportAnchorElement;
        SettingsPageNoticeStack? INoticePageHost.NoticeStack => Stack;
    }

    internal sealed class HomeNoticePageHost : INoticePageHost
    {
        private readonly HomePage _page;

        public HomeNoticePageHost(HomePage page) => _page = page;

        public object HostKey => _page;
        public bool IsHomeOverview => true;
        public ScrollViewer? ScrollViewer => _page.PageNoticeHost.PageScrollViewer;
        public FrameworkElement? ViewportAnchor => _page.HomeNoticeViewportAnchorElement;
        public SettingsPageNoticeStack? NoticeStack => null;
    }

    internal sealed class LegacyNoticePageHost : INoticePageHost
    {
        public required object HostKey { get; init; }
        public required ScrollViewer? ScrollViewer { get; init; }
        public required FrameworkElement? ViewportAnchor { get; init; }
        public bool IsHomeOverview => false;
        public SettingsPageNoticeStack? NoticeStack => null;
    }

    internal sealed class NoticePresentRequest
    {
        public required SettingsUnsavedNoticeStrip Strip { get; init; }
        public required SettingsNoticeSeverity Severity { get; init; }
        public required string Message { get; init; }
        public bool ShowSave { get; init; }
        public bool ShowAction { get; init; }
        public string? ActionButtonText { get; init; }
        public string? MirrorSuppressKey { get; init; }
        public bool Animate { get; init; } = true;
    }

    internal sealed class HomeNoticeRefreshRequest
    {
        public required HomeNoticePageHost Host { get; init; }
        public required SettingsUnsavedNoticeStrip GreenStrip { get; init; }
        public required SettingsUnsavedNoticeStrip YellowStrip { get; init; }
        public required SettingsUnsavedNoticeStrip UpdateStrip { get; init; }
        public bool SaveInProgress { get; init; }
        public bool ForceAnimateGreen { get; init; }
        public bool AnimateUpdate { get; init; }
        public string? GreenMessage { get; init; }
        public string? YellowMessage { get; init; }
        public string? UpdateMessage { get; init; }
        public bool ShowGreen { get; init; }
        public bool ShowYellow { get; init; }
        public bool ShowUpdate { get; init; }
        public string? UpdateActionText { get; init; }
    }

    private sealed class NoticeEntry
    {
        public Guid Id { get; } = Guid.NewGuid();
        public long Sequence { get; set; }
        public NoticeSlotColor Color { get; init; }
        public NoticeEntryState State { get; set; } = NoticeEntryState.Hidden;
        public required SettingsUnsavedNoticeStrip Strip { get; init; }
        public SettingsNoticeSeverity Severity { get; set; }
        public string Message { get; set; } = string.Empty;
        public bool ShowSave { get; set; }
        public bool ShowAction { get; set; }
        public string? ActionButtonText { get; set; }
        public string? MirrorSuppressKey { get; set; }
        public NoticePresentMode PresentMode { get; set; }
        public bool AnimateOnPresent { get; set; } = true;
        public bool SaveEnabled { get; set; } = true;
    }

    private sealed class HostNoticeState
    {
        public int RefreshGeneration;
        public readonly List<NoticeEntry> Entries = new();
    }

    public static HomeNoticePageHost CreateHomeHost(HomePage page) => new(page);

    public static ShellNoticePageHost CreateShellHost(SettingsPageNoticeStack stack, ScrollViewer? scroll) =>
        new() { Stack = stack, ScrollViewer = scroll };

    public static LegacyNoticePageHost CreateLegacyHost(
        object hostKey,
        ScrollViewer? scroll,
        FrameworkElement? viewportAnchor) =>
        new() { HostKey = hostKey, ScrollViewer = scroll, ViewportAnchor = viewportAnchor };

    public static NoticeSlotColor ClassifyColor(SettingsNoticeSeverity severity, bool showSave, bool showAction)
    {
        if (severity == SettingsNoticeSeverity.Success && !showSave && !showAction)
            return NoticeSlotColor.Green;
        if (severity == SettingsNoticeSeverity.Error)
            return NoticeSlotColor.Error;
        if (showAction)
            return NoticeSlotColor.Action;
        return NoticeSlotColor.Yellow;
    }

    public static async Task PresentAsync(INoticePageHost host, NoticePresentRequest request)
    {
        var state = GetState(host.HostKey);
        var color = ClassifyColor(request.Severity, request.ShowSave, request.ShowAction);
        var entry = UpsertEntry(state, host, new NoticeUpsertArgs
        {
            Strip = request.Strip,
            Color = color,
            Severity = request.Severity,
            Message = request.Message,
            ShowSave = request.ShowSave,
            ShowAction = request.ShowAction,
            ActionButtonText = request.ActionButtonText,
            MirrorSuppressKey = request.MirrorSuppressKey,
            AnimateOnPresent = request.Animate
        });

        if (entry.State != NoticeEntryState.Visible)
            TryPromoteEntry(host, state, entry);

        var gen = ++state.RefreshGeneration;
        NoticeMirrorTrace.WriteCoord(
            $"Present: color={color}, severity={request.Severity}, gen={gen}, visible={state.Entries.Count(e => e.State == NoticeEntryState.Visible)}");
        await ProjectHostAsync(host, state, gen).ConfigureAwait(true);
    }

    /// <summary>
    /// Вход на shell-страницу: восстановить видимые notice coordinator, кроме «Параметры сохранены» (MirrorOnly green на session strip).
    /// </summary>
    public static async Task RestoreShellAfterNavigationAsync(
        SettingsPageNoticeStack stack,
        ScrollViewer? scrollViewer)
    {
        var host = CreateShellHost(stack, scrollViewer);
        var state = GetState(host.HostKey);
        var sessionStrip = stack.SessionNotice.NoticeStripControl;

        // Green (session save-success, ExternalSync) не восстанавливаем — одноразовые.
        state.Entries.RemoveAll(e =>
            e.State == NoticeEntryState.Visible
            && e.Color == NoticeSlotColor.Green);

        if (sessionStrip.IsVisuallyOpen)
            await HideStripAsync(sessionStrip, animate: false).ConfigureAwait(true);

        if (stack.ExternalSyncNotice.IsVisuallyOpen
            || stack.ExternalSyncNotice.Visibility == Visibility.Visible)
        {
            await HideStripAsync(stack.ExternalSyncNotice, animate: false).ConfigureAwait(true);
            stack.ExternalSyncNotice.Visibility = Visibility.Collapsed;
        }

        if (!state.Entries.Any(e => e.State == NoticeEntryState.Visible))
            return;

        foreach (var entry in state.Entries.Where(e => e.State == NoticeEntryState.Visible))
            entry.AnimateOnPresent = false;

        var gen = ++state.RefreshGeneration;
        await ProjectHostAsync(host, state, gen).ConfigureAwait(true);
    }

    public static async Task CloseStripAsync(INoticePageHost host, SettingsUnsavedNoticeStrip strip, bool animate = true)
    {
        var state = GetState(host.HostKey);
        PolicyRetractStrip(state, strip);
        var gen = ++state.RefreshGeneration;
        await HideStripAsync(strip, animate).ConfigureAwait(true);
        await FinalizeMirrorAsync(host, gen, state).ConfigureAwait(true);
    }

    public static async Task OnUserDismissedAsync(INoticePageHost host, SettingsUnsavedNoticeStrip strip)
    {
        var state = GetState(host.HostKey);
        foreach (var entry in state.Entries.Where(e => ReferenceEquals(e.Strip, strip)).ToList())
            entry.State = NoticeEntryState.Dismissed;

        state.Entries.RemoveAll(e => e.State == NoticeEntryState.Dismissed);
        PromoteFromBacklog(host, state);
        NoticeViewportMirrorService.ClearMirrorSuppressForSource(strip);

        var gen = ++state.RefreshGeneration;
        NoticeMirrorTrace.WriteCoord($"OnUserDismissed: gen={gen}, remaining={state.Entries.Count}");
        await ProjectHostAsync(host, state, gen).ConfigureAwait(true);
    }

    /// <summary>
    /// Ephemeral dismiss mirror-only success: снять Green entry без ApplyDismissState / page dismiss state.
    /// </summary>
    public static async Task RetractInformationalSuccessAsync(INoticePageHost host, SettingsUnsavedNoticeStrip strip)
    {
        var state = GetState(host.HostKey);
        if (!state.Entries.Any(e =>
                e.Color == NoticeSlotColor.Green
                && ReferenceEquals(e.Strip, strip)))
            return;

        PolicyRetractColor(state, NoticeSlotColor.Green, strip);
        if (strip.IsVisuallyOpen)
            await HideStripAsync(strip, animate: false).ConfigureAwait(true);

        var gen = ++state.RefreshGeneration;
        NoticeMirrorTrace.WriteCoord($"RetractInformationalSuccess: gen={gen}, informational={NoticeViewportMirrorService.IsInformationalMirrorActive}");
        await NoticeViewportMirrorService.EndInformationalEpochIfActiveAsync(strip).ConfigureAwait(true);
        await FinalizeMirrorAsync(host, gen, state).ConfigureAwait(true);
    }

    /// <summary>Policy-hide при «Сохранить»: retract слота без promote бэклога.</summary>
    public static async Task RetractOnSaveAsync(
        INoticePageHost host,
        SettingsUnsavedNoticeStrip strip,
        NoticeSlotColor colorToRetract)
    {
        var state = GetState(host.HostKey);
        PolicyRetractColor(state, colorToRetract, strip);
        var gen = ++state.RefreshGeneration;
        await HideStripAsync(strip, animate: false).ConfigureAwait(true);
        await FinalizeMirrorAsync(host, gen, state).ConfigureAwait(true);
    }

    public static async Task RefreshHomeAsync(HomeNoticeRefreshRequest request)
    {
        var host = request.Host;
        var state = GetState(host.HostKey);
        var gen = ++state.RefreshGeneration;

        if (request.ShowGreen && request.GreenMessage is not null)
        {
            if (request.ForceAnimateGreen)
                HomeSessionNoticeState.ArmStartupGreenFooterDeposit();

            UpsertEntry(state, host, new NoticeUpsertArgs
            {
                Strip = request.GreenStrip,
                Color = NoticeSlotColor.Green,
                Severity = SettingsNoticeSeverity.Success,
                Message = request.GreenMessage,
                ShowSave = false,
                ShowAction = false,
                AnimateOnPresent = request.ForceAnimateGreen || HomeSessionNoticeState.ConsumeAnimateNextOpen()
            });
        }
        else
            PolicyRetractColor(state, NoticeSlotColor.Green, request.GreenStrip);

        if (request.ShowYellow && request.YellowMessage is not null)
        {
            UpsertEntry(state, host, new NoticeUpsertArgs
            {
                Strip = request.YellowStrip,
                Color = NoticeSlotColor.Yellow,
                Severity = SettingsNoticeSeverity.Warning,
                Message = request.YellowMessage,
                ShowSave = true,
                ShowAction = false,
                AnimateOnPresent = HomeSessionNoticeState.ConsumeAnimateNextOpen(),
                SaveEnabled = !request.SaveInProgress
            });
        }
        else
            PolicyRetractColor(state, NoticeSlotColor.Yellow, request.YellowStrip);

        if (request.ShowUpdate && request.UpdateMessage is not null)
        {
            request.UpdateStrip.ShowSaveButton = false;
            request.UpdateStrip.ShowActionButton = true;
            if (!string.IsNullOrEmpty(request.UpdateActionText))
                request.UpdateStrip.ActionButtonText = request.UpdateActionText;

            UpsertEntry(state, host, new NoticeUpsertArgs
            {
                Strip = request.UpdateStrip,
                Color = NoticeSlotColor.Action,
                Severity = SettingsNoticeSeverity.Warning,
                Message = request.UpdateMessage,
                ShowSave = false,
                ShowAction = true,
                ActionButtonText = request.UpdateActionText,
                AnimateOnPresent = request.AnimateUpdate
            });
        }
        else
            PolicyRetractColor(state, NoticeSlotColor.Action, request.UpdateStrip);

        ReconcilePromotion(host, state);
        await ProjectHostAsync(host, state, gen).ConfigureAwait(true);
    }

    private static void ReconcilePromotion(INoticePageHost host, HostNoticeState state)
    {
        foreach (var hidden in state.Entries
                     .Where(e => e.State == NoticeEntryState.Hidden)
                     .OrderByDescending(e => e.Sequence)
                     .ToList())
            TryPromoteEntry(host, state, hidden);
    }

    private sealed class NoticeUpsertArgs
    {
        public required SettingsUnsavedNoticeStrip Strip { get; init; }
        public required NoticeSlotColor Color { get; init; }
        public required SettingsNoticeSeverity Severity { get; init; }
        public required string Message { get; init; }
        public bool ShowSave { get; init; }
        public bool ShowAction { get; init; }
        public string? ActionButtonText { get; init; }
        public string? MirrorSuppressKey { get; init; }
        public bool AnimateOnPresent { get; init; } = true;
        public bool SaveEnabled { get; init; } = true;
    }

    private static NoticeEntry UpsertEntry(HostNoticeState state, INoticePageHost host, NoticeUpsertArgs args)
    {
        ApplyStripSlotExclusivity(state, host, args);

        var existing = state.Entries
            .Where(e => e.State != NoticeEntryState.Dismissed
                        && e.Color == args.Color
                        && ReferenceEquals(e.Strip, args.Strip))
            .OrderByDescending(e => e.Sequence)
            .FirstOrDefault();

        if (existing is not null)
        {
            existing.Severity = args.Severity;
            existing.Message = args.Message;
            existing.ShowSave = args.ShowSave;
            existing.ShowAction = args.ShowAction;
            existing.ActionButtonText = args.ActionButtonText;
            existing.MirrorSuppressKey = args.MirrorSuppressKey;
            existing.AnimateOnPresent = args.AnimateOnPresent;
            existing.SaveEnabled = args.SaveEnabled;
            existing.PresentMode = ResolvePresentMode(host, args.Color, args.Strip);
            if (existing.State == NoticeEntryState.Hidden)
                TryPromoteEntry(host, state, existing);
            NoticeMirrorTrace.WriteCoord(
                $"Upsert: update color={args.Color}, state={existing.State}, severity={args.Severity}, stripOpen={args.Strip.IsVisuallyOpen}");
            return existing;
        }

        var entry = new NoticeEntry
        {
            Strip = args.Strip,
            Color = args.Color,
            Severity = args.Severity,
            Message = args.Message,
            ShowSave = args.ShowSave,
            ShowAction = args.ShowAction,
            ActionButtonText = args.ActionButtonText,
            MirrorSuppressKey = args.MirrorSuppressKey,
            AnimateOnPresent = args.AnimateOnPresent,
            SaveEnabled = args.SaveEnabled,
            PresentMode = ResolvePresentMode(host, args.Color, args.Strip),
            Sequence = Interlocked.Increment(ref _globalSequence)
        };

        state.Entries.Add(entry);
        TryPromoteEntry(host, state, entry);
        NoticeMirrorTrace.WriteCoord(
            $"Upsert: color={args.Color}, state={entry.State}, severity={args.Severity}, stripOpen={args.Strip.IsVisuallyOpen}, mode={entry.PresentMode}");
        return entry;
    }

    private static bool TryPromoteEntry(INoticePageHost host, HostNoticeState state, NoticeEntry entry)
    {
        if (entry.State == NoticeEntryState.Dismissed)
            return false;

        var visible = state.Entries.Where(e => e.State == NoticeEntryState.Visible).ToList();

        if (visible.Count < MaxVisibleSlots)
        {
            if (host.IsHomeOverview)
            {
                foreach (var old in visible.Where(v => v.Color == entry.Color).ToList())
                {
                    old.State = NoticeEntryState.Hidden;
                    visible.Remove(old);
                }
            }

            entry.State = NoticeEntryState.Visible;
            return true;
        }

        if (host.IsHomeOverview && visible.Count(v => v.Color == entry.Color) == 1)
        {
            var colorCounts = visible.GroupBy(v => v.Color).ToDictionary(g => g.Key, g => g.Count());
            if (colorCounts.Count == 3 && colorCounts.Values.All(c => c == 1))
            {
                var old = visible.First(v => v.Color == entry.Color);
                old.State = NoticeEntryState.Hidden;
                entry.State = NoticeEntryState.Visible;
                return true;
            }
        }

        var groups = visible.GroupBy(v => v.Color).ToDictionary(g => g.Key, g => g.Count());
        var allSameColor = groups.Count == 1;
        var hasDuplicateColor = groups.Values.Any(c => c >= 2);

        if (allSameColor || hasDuplicateColor)
        {
            var overloadColor = allSameColor
                ? visible[0].Color
                : groups.First(kv => kv.Value >= 2).Key;
            var earliest = visible
                .Where(v => v.Color == overloadColor)
                .OrderBy(v => v.Sequence)
                .First();
            earliest.State = NoticeEntryState.Hidden;
            entry.State = NoticeEntryState.Visible;
            return true;
        }

        entry.State = NoticeEntryState.Hidden;
        return false;
    }

    private static void PromoteFromBacklog(INoticePageHost host, HostNoticeState state)
    {
        while (state.Entries.Count(e => e.State == NoticeEntryState.Visible) < MaxVisibleSlots)
        {
            var next = state.Entries
                .Where(e => e.State == NoticeEntryState.Hidden)
                .OrderBy(e => e.Sequence)
                .FirstOrDefault();
            if (next is null)
                break;

            if (!TryPromoteEntry(host, state, next))
            {
                if (next.State == NoticeEntryState.Hidden)
                    break;
            }
        }
    }

    private static void PolicyRetractColor(
        HostNoticeState state,
        NoticeSlotColor color,
        SettingsUnsavedNoticeStrip strip)
    {
        state.Entries.RemoveAll(e => e.Color == color && ReferenceEquals(e.Strip, strip));
    }

    /// <summary>На одном strip одновременно только один слот Green MirrorOnly или Yellow.</summary>
    private static void ApplyStripSlotExclusivity(HostNoticeState state, INoticePageHost host, NoticeUpsertArgs args)
    {
        if (args.Color == NoticeSlotColor.Yellow)
            PolicyRetractColor(state, NoticeSlotColor.Green, args.Strip);

        // Green всегда MirrorOnly — на том же strip не держать Yellow одновременно.
        if (args.Color == NoticeSlotColor.Green)
            PolicyRetractColor(state, NoticeSlotColor.Yellow, args.Strip);
    }

    private static void PolicyRetractStrip(HostNoticeState state, SettingsUnsavedNoticeStrip strip)
    {
        state.Entries.RemoveAll(e => ReferenceEquals(e.Strip, strip));
    }

    private static NoticePresentMode ResolvePresentMode(
        INoticePageHost host,
        NoticeSlotColor color,
        SettingsUnsavedNoticeStrip strip) =>
        color == NoticeSlotColor.Green
            ? NoticePresentMode.MirrorOnly
            : NoticePresentMode.PinnedLayout;

    private static async Task ProjectHostAsync(INoticePageHost host, HostNoticeState state, int generation)
    {
        var visibleEntries = state.Entries.Where(e => e.State == NoticeEntryState.Visible).ToList();
        NoticeMirrorTrace.WriteCoord(
            $"ProjectHost: gen={generation}, visible={visibleEntries.Count}, colors=[{string.Join(',', visibleEntries.Select(e => e.Color))}]");
        var stripsWithYellow = visibleEntries
            .Where(e => e.Color == NoticeSlotColor.Yellow)
            .Select(e => e.Strip)
            .ToHashSet(ReferenceEqualityComparer.Instance);

        var visibleStrips = new HashSet<SettingsUnsavedNoticeStrip>(
            visibleEntries.Select(e => e.Strip));

        foreach (var entry in visibleEntries)
        {
            if (entry.Color == NoticeSlotColor.Green
                && entry.PresentMode == NoticePresentMode.MirrorOnly
                && stripsWithYellow.Contains(entry.Strip))
                continue;

            await PresentEntryAsync(host, entry).ConfigureAwait(true);
        }

        foreach (var entry in state.Entries
                     .Where(e => e.State != NoticeEntryState.Visible)
                     .DistinctBy(e => e.Strip))
        {
            if (!visibleStrips.Contains(entry.Strip) && entry.Strip.IsVisuallyOpen)
                await HideStripAsync(entry.Strip, animate: false).ConfigureAwait(true);
        }

        if (generation != state.RefreshGeneration)
            return;

        await FinalizeMirrorAsync(host, generation, state).ConfigureAwait(true);
    }

    private static async Task PresentEntryAsync(INoticePageHost host, NoticeEntry entry)
    {
        var strip = entry.Strip;
        strip.ShowSaveButton = entry.ShowSave;
        strip.ShowActionButton = entry.ShowAction;
        strip.MirrorSuppressKey = entry.MirrorSuppressKey;
        if (!string.IsNullOrEmpty(entry.ActionButtonText))
            strip.ActionButtonText = entry.ActionButtonText;

        if (entry.PresentMode == NoticePresentMode.MirrorOnly)
            strip.Visibility = Visibility.Collapsed;

        var scroll = host.ScrollViewer;
        var animate = entry.AnimateOnPresent;

        if (entry.ShowSave)
            strip.IsSaveEnabled = entry.SaveEnabled;

        if (strip.IsVisuallyOpen
            && entry.PresentMode == NoticePresentMode.PinnedLayout
            && !NoticePresentationPolicy.IsInformationalMirrorOnly(entry.Severity, entry.ShowSave, entry.ShowAction))
        {
            // Не обрывать slide-in повторным present (Home Refresh сразу после startup green).
            if (!strip.IsAnimating)
            {
                strip.OpenImmediate(
                    entry.Severity,
                    entry.Message,
                    entry.ShowSave,
                    entry.ShowAction,
                    entry.ActionButtonText);
            }
            else
            {
                strip.ApplyContent(
                    entry.Severity,
                    entry.Message,
                    entry.ShowSave,
                    entry.ShowAction,
                    entry.ActionButtonText);
            }

            await NoticeViewportMirrorService.EnsurePinnedMirrorScrollUnblockedAsync().ConfigureAwait(true);
            if (host.NoticeStack is { } openStack)
                await NoticeViewportMirrorService.SyncMirrorForStackQuietAsync(openStack, scroll).ConfigureAwait(true);
            else
                await NoticeViewportMirrorService.SyncLegacyMirrorQuietAsync(scroll).ConfigureAwait(true);
            if (entry.ShowSave)
                NoticeViewportMirrorService.SyncSaveEnabled(entry.SaveEnabled);
            return;
        }

        if (host.NoticeStack is { } stack)
        {
            await NoticeViewportMirrorService.PresentStackItemAsync(
                stack,
                strip,
                scroll,
                entry.Severity,
                entry.Message,
                entry.ShowSave,
                animate).ConfigureAwait(true);
        }
        else
        {
            await NoticeViewportMirrorService.PresentAsync(
                strip,
                scroll,
                entry.Severity,
                entry.Message,
                entry.ShowSave,
                animate,
                viewportAnchor: host.ViewportAnchor).ConfigureAwait(true);
        }
    }

    private static async Task HideStripAsync(SettingsUnsavedNoticeStrip strip, bool animate)
    {
        NoticeViewportMirrorService.ClearMirrorSuppressForSource(strip);

        if (!strip.IsVisuallyOpen)
        {
            NoticeViewportMirrorService.ReleaseLegacySourceAndSyncQuiet(strip);
            return;
        }

        await NoticeViewportMirrorService.ClosePinnedSourcePreservingScrollAsync(strip, animate)
            .ConfigureAwait(true);

        NoticeViewportMirrorService.ReleaseLegacySourceAndSyncQuiet(strip);
    }

    private static async Task FinalizeMirrorAsync(INoticePageHost host, int generation, HostNoticeState state)
    {
        if (generation != state.RefreshGeneration)
            return;

        if (host.NoticeStack is { } stack)
            await NoticeViewportMirrorService.ReattachPageAsync(stack, host.ScrollViewer).ConfigureAwait(true);
        else if (host.ViewportAnchor is not null)
            await NoticeViewportMirrorService.ReattachLegacyAsync(host.ScrollViewer, host.ViewportAnchor).ConfigureAwait(true);
        else
            await NoticeViewportMirrorService.SyncLegacyMirrorQuietAsync(host.ScrollViewer).ConfigureAwait(true);
    }

    private static HostNoticeState GetState(object hostKey)
    {
        if (!_hosts.TryGetValue(hostKey, out var state))
        {
            state = new HostNoticeState();
            _hosts[hostKey] = state;
        }

        return state;
    }

    public static void ClearHost(object hostKey) => _hosts.Remove(hostKey);
}

using GlassSettings.Core.Layout;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

/// <summary>Стек notice-полос страницы (Session / ExternalSync / Action). См. GlassSettings/docs/GlassSettings-notices.md.</summary>
public sealed partial class SettingsPageNoticeStack : UserControl
{
    private ScrollViewer? _scrollViewer;

    public SettingsPageNoticeStack()
    {
        InitializeComponent();
        ActionNoticeStrip.DismissRequested += (_, _) => _ = CloseActionNoticeAsync();
        ExternalSyncNoticeStrip.DismissStarting += (_, _) =>
        {
            if (CommitScope is { } scope)
                PageExternalSyncNoticeState.Dismiss(scope);
        };
        ExternalSyncNoticeStrip.DismissRequested += (_, _) => _ = OnExternalSyncDismissRequestedAsync();
        Loaded += (_, _) => NoticeViewportMirrorService.RegisterNoticeStack(this);
        Unloaded += (_, _) => NoticeViewportMirrorService.UnregisterNoticeStack(this);
    }

    public SettingsSessionNoticeBar SessionNotice => SessionNoticeBar;

    public SettingsUnsavedNoticeStrip ActionNotice => ActionNoticeStrip;

    public SettingsUnsavedNoticeStrip ExternalSyncNotice => ExternalSyncNoticeStrip;

    internal FrameworkElement ViewportAnchorElement => ViewportAnchor;

    public void BindScrollViewer(ScrollViewer scrollViewer) => _scrollViewer = scrollViewer;

    private ScrollViewer? EffectiveScrollViewer =>
        _scrollViewer ?? SettingsContentColumnLayout.GetPageContentScroll();

    public SettingsCommitScope? CommitScope
    {
        get => SessionNoticeBar.CommitScope;
        set => SessionNoticeBar.CommitScope = value;
    }

    public void RefreshForNavigation()
    {
        SessionNoticeBar.RefreshForNavigation();
        _ = RestoreNoticesForNavigationAsync();
    }

    internal async Task RestoreNoticesForNavigationAsync()
    {
        // ExternalSync green не restore при входе — одноразовый MirrorOnly.
        await SettingsNoticeCoordinator.RestoreShellAfterNavigationAsync(this, EffectiveScrollViewer)
            .ConfigureAwait(true);
    }

    public void Refresh() => SessionNoticeBar.Refresh();

    public void RefreshProgrammatic() => SessionNoticeBar.RefreshProgrammatic();

    internal void FlushPendingNoticeSync() => SessionNoticeBar.FlushPendingNoticeSync();

    public void OnDirtyChanged() => SessionNoticeBar.OnDirtyChanged();

    public void OnInteractionCompleted() => SessionNoticeBar.OnInteractionCompleted();

    public async Task TryShowExternalSyncNoticeAsync(bool animate)
    {
        if (CommitScope is not { } scope || !PageExternalSyncNoticeState.ShouldShow(scope))
        {
            if (ExternalSyncNoticeStrip.IsVisuallyOpen
                || ExternalSyncNoticeStrip.Visibility == Visibility.Visible)
                await RetractExternalSyncNoticeAsync().ConfigureAwait(true);
            return;
        }

        ExternalSyncNoticeStrip.Visibility = Visibility.Visible;
        var host = SettingsNoticeCoordinator.CreateShellHost(this, EffectiveScrollViewer);
        await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
        {
            Strip = ExternalSyncNoticeStrip,
            Severity = SettingsNoticeSeverity.Success,
            Message = GlassSettingsResources.Get("Notice_ExternalSectionSynced"),
            ShowSave = false,
            ShowAction = false,
            Animate = animate
        }).ConfigureAwait(true);
    }

    /// <summary>Policy-hide external-sync без сброса pending (навигация / !ShouldShow).</summary>
    private async Task RetractExternalSyncNoticeAsync()
    {
        var host = SettingsNoticeCoordinator.CreateShellHost(this, EffectiveScrollViewer);
        if (ExternalSyncNoticeStrip.IsVisuallyOpen)
        {
            await NoticeViewportMirrorService.ClosePinnedSourcePreservingScrollAsync(
                    ExternalSyncNoticeStrip,
                    animate: true)
                .ConfigureAwait(true);
        }

        ExternalSyncNoticeStrip.Visibility = Visibility.Collapsed;
        await SettingsNoticeCoordinator.CloseStripAsync(host, ExternalSyncNoticeStrip, animate: false)
            .ConfigureAwait(true);
    }

    private async Task OnExternalSyncDismissRequestedAsync()
    {
        var host = SettingsNoticeCoordinator.CreateShellHost(this, EffectiveScrollViewer);
        await SettingsNoticeCoordinator.OnUserDismissedAsync(host, ExternalSyncNoticeStrip).ConfigureAwait(true);
        ExternalSyncNoticeStrip.Visibility = Visibility.Collapsed;
    }

    public async Task ShowActionNoticeAsync(
        SettingsNoticeSeverity severity,
        string message,
        string actionButtonText,
        bool animate = true)
    {
        if (ActionNoticeStrip.IsVisuallyOpen)
            NoticeViewportMirrorService.ClosePinnedSourcePreservingScroll(ActionNoticeStrip);

        ActionNoticeStrip.ShowSaveButton = false;
        ActionNoticeStrip.ShowActionButton = true;
        ActionNoticeStrip.ActionButtonText = actionButtonText;

        if (!NoticePresentationPolicy.IsInformationalMirrorOnly(severity, showSave: false, ActionNoticeStrip.ShowActionButton))
            ActionNoticeStrip.Visibility = Visibility.Visible;

        var host = SettingsNoticeCoordinator.CreateShellHost(this, EffectiveScrollViewer);
        await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
        {
            Strip = ActionNoticeStrip,
            Severity = severity,
            Message = message,
            ShowSave = false,
            ShowAction = true,
            ActionButtonText = actionButtonText,
            Animate = animate
        }).ConfigureAwait(true);
    }

    public async Task CloseActionNoticeAsync()
    {
        var host = SettingsNoticeCoordinator.CreateShellHost(this, EffectiveScrollViewer);
        if (ActionNoticeStrip.IsVisuallyOpen)
        {
            await NoticeViewportMirrorService.ClosePinnedSourcePreservingScrollAsync(
                    ActionNoticeStrip,
                    animate: true)
                .ConfigureAwait(true);
        }

        ActionNoticeStrip.Visibility = Visibility.Collapsed;
        await SettingsNoticeCoordinator.CloseStripAsync(host, ActionNoticeStrip, animate: false).ConfigureAwait(true);
    }

    internal IReadOnlyList<SettingsUnsavedNoticeStrip> GetOpenStrips()
    {
        var list = new List<SettingsUnsavedNoticeStrip>();
        if (SessionNoticeBar.NoticeStripControl.IsVisuallyOpen)
            list.Add(SessionNoticeBar.NoticeStripControl);
        if (ExternalSyncNoticeStrip.IsVisuallyOpen)
            list.Add(ExternalSyncNoticeStrip);
        if (ActionNoticeStrip.IsVisuallyOpen)
            list.Add(ActionNoticeStrip);
        return list;
    }
}

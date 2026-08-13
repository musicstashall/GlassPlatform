using GlassSettings.Core.Layout;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;



namespace GlassSettings.Controls;



public sealed partial class SettingsSessionNoticeBar : UserControl

{

    public static readonly DependencyProperty CommitScopeProperty =

        DependencyProperty.Register(nameof(CommitScope), typeof(SettingsCommitScope?), typeof(SettingsSessionNoticeBar),

            new PropertyMetadata(null, (d, _) => ((SettingsSessionNoticeBar)d).Refresh()));



    private const int ProcedureDebounceMs = 120;



    private bool _commitInProgress;

    private bool _navigationRestore;

    private bool _dirtyPending;

    private bool _pointerReleased;

    private CancellationTokenSource? _procedureDebounceCts;
    private bool _deferNoticeUntilOverlayIdle;

    internal SettingsUnsavedNoticeStrip NoticeStripControl => NoticeStrip;

    public SettingsSessionNoticeBar()

    {

        InitializeComponent();

        NoticeStrip.SaveRequested += (_, _) => _ = SaveAsync();

        NoticeStrip.DismissStarting += (_, _) =>
        {
            // Success mirror-only на collapsed strip: не помечать page dismiss при unsaved.
            if (!NoticeStrip.IsVisuallyOpen
                && NoticeStrip.Severity == SettingsNoticeSeverity.Success
                && CommitScope is { } scope
                && SettingsSession.Current.HasUnsavedInScope(scope))
                return;

            ApplyDismissState();
        };

        NoticeStrip.DismissRequested += (_, _) =>
        {
            if (FindNoticeStack() is { } stack)
            {
                var host = SettingsNoticeCoordinator.CreateShellHost(stack, FindPageScrollViewer());
                if (!NoticeStrip.IsVisuallyOpen)
                {
                    if (CommitScope is { } dismissScope
                        && SettingsSession.Current.HasUnsavedInScope(dismissScope)
                        && NoticeStrip.Severity == SettingsNoticeSeverity.Warning)
                    {
                        ApplyDismissState();
                    }

                    var dirtyEphemeral = CommitScope is { } epScope && SettingsSession.Current.HasUnsavedInScope(epScope);
                    NoticeMirrorTrace.WriteSession(
                        $"DismissRequested: ephemeral → RetractInformationalSuccess ({PageSessionNoticeState.DescribeForTrace(dirtyEphemeral)})");
                    _ = SettingsNoticeCoordinator.RetractInformationalSuccessAsync(host, NoticeStrip);
                    return;
                }

                _ = SettingsNoticeCoordinator.OnUserDismissedAsync(host, NoticeStrip);
            }
            else
                NoticeViewportMirrorService.ReleaseSource(NoticeStrip);
        };

        Loaded += (_, _) =>
        {
            EnsureDirtySubscription();
            WindowBusyOverlayService.OverlayBecameIdle += OnOverlayBecameIdle;
            RefreshForNavigation();
        };

        Unloaded += (_, _) =>
        {
            SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
            WindowBusyOverlayService.OverlayBecameIdle -= OnOverlayBecameIdle;
            _procedureDebounceCts?.Cancel();
        };

    }



    public SettingsCommitScope? CommitScope

    {

        get => (SettingsCommitScope?)GetValue(CommitScopeProperty);

        set => SetValue(CommitScopeProperty, value);

    }



    public void RefreshForNavigation()

    {

        EnsureDirtySubscription();

        _navigationRestore = true;

        _dirtyPending = false;

        _pointerReleased = true;

        _procedureDebounceCts?.Cancel();

        if (_deferNoticeUntilOverlayIdle && !WindowBusyOverlayService.IsActive)
            _deferNoticeUntilOverlayIdle = false;

        Refresh();

    }



    private static bool IsExternalSyncCommitPending =>
        RegistrySnapshotWatchService.Current?.IsExternalSyncCommitPending == true;

    public void OnDirtyChanged()
    {
        if (SettingsSession.Current.IsShuttingDown)
            return;

        if (_commitInProgress || CommitScope is not { } scope)
            return;

        // Кэш Frame: DirtyChanged видят все Loaded-страницы. Жёлтый notice только на активной.
        if (!BelongsToActivePage())
        {
            if (!SettingsSession.Current.HasUnsavedInScope(scope)
                && NoticeStrip.IsVisuallyOpen
                && !NoticeViewportMirrorService.IsInformationalMirrorActive)
            {
                CloseNoticeImmediate();
            }

            return;
        }

        if (WindowBusyOverlayService.IsActive)
        {
            _deferNoticeUntilOverlayIdle = true;
            return;
        }

        if (_deferNoticeUntilOverlayIdle)
            _deferNoticeUntilOverlayIdle = false;

        if (SettingsPageOperationGate.Current?.ShouldDeferNotices == true)
            return;

        var dirty = SettingsSession.Current.HasUnsavedInScope(scope);

        NoticeMirrorTrace.WriteSession(
            $"OnDirtyChanged: dirty={dirty}, deferGate={SettingsPageOperationGate.Current?.ShouldDeferNotices}, {PageSessionNoticeState.DescribeForTrace(dirty)}");

        if (!dirty)
        {
            if (NoticeViewportMirrorService.IsInformationalMirrorActive)
                return;

            _dirtyPending = false;
            _pointerReleased = false;
            _procedureDebounceCts?.Cancel();
            PageSessionNoticeState.ClearSaveSuccessPending();
            CloseNoticeImmediate();
            return;
        }

        if (IsExternalSyncCommitPending)
        {
            _dirtyPending = false;
            _procedureDebounceCts?.Cancel();
            return;
        }

        PageSessionNoticeState.ClearSaveSuccessPending();
        PageSessionNoticeState.InvalidateDismissOnMutation();

        if (SettingsInteractionDeferral.IsSliderGestureActive)
        {
            _dirtyPending = true;
            return;
        }

        if (NoticeStrip.IsVisuallyOpen)
        {
            if (CommitScope is { } openScope && !SettingsSession.Current.HasUnsavedInScope(openScope))
            {
                if (!NoticeViewportMirrorService.IsInformationalMirrorActive)
                    CloseNoticeImmediate();
                _navigationRestore = false;
                _dirtyPending = false;
                return;
            }

            _ = SyncOpenNoticeForDirtyAsync(dirty);
            return;
        }



        _dirtyPending = true;

        ScheduleTryOpenAfterProcedures();

    }



    public void OnInteractionCompleted()
    {
        if (IsExternalSyncCommitPending)
            return;

        if (!BelongsToActivePage())
            return;

        if (CommitScope is { } scope)
        {
            var dirty = SettingsSession.Current.HasUnsavedInScope(scope);
            if (dirty && PageSessionNoticeState.ShouldHide(dirty))
            {
                NoticeMirrorTrace.WriteSession(
                    $"OnInteractionCompleted: skip dismissed ({PageSessionNoticeState.DescribeForTrace(dirty)})");
                return;
            }
        }

        _pointerReleased = true;
        NoticeMirrorTrace.WriteSession("OnInteractionCompleted: schedule TryOpen");
        ScheduleTryOpenAfterProcedures();
    }



    public void Refresh() => _ = RefreshAsync();

    /// <summary>После программной записи в реестр (pivot, toggle) — без PointerReleased на shell.</summary>
    public void RefreshProgrammatic()
    {
        _deferNoticeUntilOverlayIdle = false;
        _procedureDebounceCts?.Cancel();

        if (CommitScope is not { } scope)
        {
            CloseNoticeImmediate();
            _dirtyPending = false;
            _pointerReleased = false;
            return;
        }

        var dirty = SettingsSession.Current.HasUnsavedInScope(scope);

        if (!dirty)
        {
            if (NoticeViewportMirrorService.IsInformationalMirrorActive)
            {
                _dirtyPending = false;
                _pointerReleased = false;
                return;
            }

            _dirtyPending = false;
            _pointerReleased = false;
            PageSessionNoticeState.ClearSaveSuccessPending();
            CloseNoticeImmediate();
            return;
        }

        PageSessionNoticeState.ClearSaveSuccessPending();
        PageSessionNoticeState.InvalidateDismissOnMutation();
        _dirtyPending = true;
        _pointerReleased = true;

        if (NoticeStrip.IsVisuallyOpen)
        {
            _ = SyncOpenNoticeForDirtyAsync(dirty, animateReplacement: PageSessionNoticeState.ConsumeAnimateNextOpen());
            return;
        }

        _ = TryOpenAfterInteractionAsync();
    }

    /// <summary>NoticeSync-фаза конвейера: один sync по финальному dirty.</summary>
    internal void FlushPendingNoticeSync() => RefreshProgrammatic();

    private void ScheduleTryOpenAfterProcedures()

    {

        _procedureDebounceCts?.Cancel();

        _procedureDebounceCts = new CancellationTokenSource();

        var token = _procedureDebounceCts.Token;

        _ = WaitAndTryOpenAsync(token);

    }



    private async Task WaitAndTryOpenAsync(CancellationToken token)

    {

        try

        {

            await Task.Delay(ProcedureDebounceMs, token);

        }

        catch (OperationCanceledException)

        {

            return;

        }



        await TryOpenAfterInteractionAsync();

    }



    private async Task TryOpenAfterInteractionAsync()

    {

        if (SettingsSession.Current.IsShuttingDown)
            return;

        if (_commitInProgress || CommitScope is not { } scope)
            return;

        if (!BelongsToActivePage())
            return;

        if (IsExternalSyncCommitPending)
        {
            _navigationRestore = false;
            _dirtyPending = false;
            return;
        }

        var dirty = SettingsSession.Current.HasUnsavedInScope(scope);

        PageSessionNoticeState.OnUnsavedChanged(dirty);

        NoticeMirrorTrace.WriteSession(
            $"TryOpen: dirty={dirty}, dirtyPending={_dirtyPending}, navRestore={_navigationRestore}, {PageSessionNoticeState.DescribeForTrace(dirty)}");

        if (!dirty && NoticeViewportMirrorService.IsInformationalMirrorActive)
            return;

        if (!dirty && PageSessionNoticeState.IsSaveSuccessFor(scope))

        {

            if (!PageSessionNoticeState.TryConsumePendingSuccessDisplay())
            {
                if (NoticeViewportMirrorService.IsInformationalMirrorActive)
                {
                    _navigationRestore = false;
                    _dirtyPending = false;
                    return;
                }

                PageSessionNoticeState.ClearSaveSuccessPending();
                CloseNoticeImmediate();
                _navigationRestore = false;
                _dirtyPending = false;
                return;
            }

            if (PageSessionNoticeState.ShouldHide(false))

            {

                PageSessionNoticeState.ClearSaveSuccessPending();
                CloseNoticeImmediate();
                _navigationRestore = false;
                return;
            }

            var animateSuccess = !_navigationRestore && PageSessionNoticeState.ConsumeAnimateNextOpen();

            await ShowNoticeAsync(

                SettingsNoticeSeverity.Success,

                GlassSettingsResources.Get("Notice_SavedSuccess"),

                showSave: false,

                animate: animateSuccess);

            PageSessionNoticeState.ClearSaveSuccessPending();
            _navigationRestore = false;

            return;

        }



        if (_navigationRestore)
        {
            _navigationRestore = false;
            PageSessionNoticeState.SuppressAnimateForNavigation();

            if (!dirty || PageSessionNoticeState.ShouldHide(dirty))
            {
                if (!NoticeViewportMirrorService.IsInformationalMirrorActive)
                    CloseNoticeImmediate();
                _dirtyPending = false;
                return;
            }

            await ShowNoticeAsync(
                SettingsNoticeSeverity.Warning,
                UnsavedMessage(),
                showSave: true,
                animate: false);

            _dirtyPending = false;
            return;
        }

        if (!dirty || PageSessionNoticeState.ShouldHide(dirty))
        {
            if (NoticeViewportMirrorService.IsInformationalMirrorActive)
            {
                _dirtyPending = false;
                return;
            }

            CloseNoticeImmediate();
            _dirtyPending = false;
            return;
        }

        if (NoticeStrip.IsVisuallyOpen)
        {
            if (!dirty)
            {
                if (!NoticeViewportMirrorService.IsInformationalMirrorActive)
                    CloseNoticeImmediate();
                _dirtyPending = false;
                return;
            }

            await SyncOpenNoticeForDirtyAsync(dirty).ConfigureAwait(true);
            _dirtyPending = false;
            return;
        }

        if (!_dirtyPending)
        {
            var animateProgrammatic = PageSessionNoticeState.ConsumeAnimateNextOpen();
            await ShowNoticeAsync(
                SettingsNoticeSeverity.Warning,
                UnsavedMessage(),
                showSave: true,
                animate: animateProgrammatic);
            _dirtyPending = false;
            _pointerReleased = false;
            return;
        }

        if (!_pointerReleased)
            return;

        _dirtyPending = false;

        _pointerReleased = false;

        PageSessionNoticeState.RequestAnimateNextOpen();

        await ShowNoticeAsync(

            SettingsNoticeSeverity.Warning,

            UnsavedMessage(),

            showSave: true,

            animate: true);

    }



    private async Task RefreshAsync()

    {

        if (SettingsSession.Current.IsShuttingDown)
            return;

        if (_commitInProgress)

            return;



        if (CommitScope is not { } scope)

        {

            if (NoticeStrip.IsVisuallyOpen)
                NoticeViewportMirrorService.ClosePinnedSourcePreservingScroll(NoticeStrip);

            return;

        }



        await TryOpenAfterInteractionAsync();

    }



    private void ApplyDismissState()
    {
        if (CommitScope is not { } scope)
            return;

        var dirty = SettingsSession.Current.HasUnsavedInScope(scope);
        if (PageSessionNoticeState.IsSaveSuccessFor(scope) && !dirty)
            PageSessionNoticeState.ClearSaveSuccessPending();

        PageSessionNoticeState.Dismiss(dirty);
        PageSessionNoticeState.RequestAnimateNextOpen();
    }

    private static string UnsavedMessage() =>

        GlassSettingsResources.Get("Home_FooterUnsavedPrefix")

        + GlassSettingsResources.Get("Home_FooterUnsavedPlain");

    private static bool ShouldUpgradeSuccessToUnsaved(SettingsUnsavedNoticeStrip strip, bool dirty) =>
        dirty && strip.IsVisuallyOpen && strip.Severity == SettingsNoticeSeverity.Success;

    /// <summary>Открытый notice при dirty: Success → Warning или тихий sync.</summary>
    private async Task SyncOpenNoticeForDirtyAsync(bool dirty, bool animateReplacement = false)
    {
        if (!dirty)
            return;

        if (ShouldUpgradeSuccessToUnsaved(NoticeStrip, dirty))
        {
            await ShowNoticeAsync(
                SettingsNoticeSeverity.Warning,
                UnsavedMessage(),
                showSave: true,
                animate: animateReplacement).ConfigureAwait(true);
            return;
        }

        if (!NoticeStrip.IsVisuallyOpen)
            return;

        NoticeStrip.IsSaveEnabled = !_commitInProgress;
        NoticeViewportMirrorService.SyncSaveEnabled(!_commitInProgress);
        if (FindNoticeStack() is { } openStack)
            await NoticeViewportMirrorService.SyncMirrorForStackQuietAsync(openStack, FindPageScrollViewer())
                .ConfigureAwait(true);
    }

    private async Task ShowNoticeAsync(SettingsNoticeSeverity severity, string message, bool showSave, bool animate)
    {
        if (severity == SettingsNoticeSeverity.Warning && showSave && CommitScope is { } scope)
        {
            var dirty = SettingsSession.Current.HasUnsavedInScope(scope);
            if (PageSessionNoticeState.ShouldHide(dirty))
            {
                NoticeMirrorTrace.WriteSession(
                    $"ShowNoticeAsync: skip dismissed warning ({PageSessionNoticeState.DescribeForTrace(dirty)})");
                return;
            }

            await RetractSessionInformationalSuccessAsync().ConfigureAwait(true);
        }

        NoticeStrip.IsSaveEnabled = !_commitInProgress;
        var scroll = FindPageScrollViewer();
        if (FindNoticeStack() is { } noticeStack)
        {
            var host = SettingsNoticeCoordinator.CreateShellHost(noticeStack, scroll);
            await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
            {
                Strip = NoticeStrip,
                Severity = severity,
                Message = message,
                ShowSave = showSave,
                ShowAction = NoticeStrip.ShowActionButton,
                ActionButtonText = NoticeStrip.ActionButtonText,
                Animate = animate
            }).ConfigureAwait(true);
        }
        else
        {
            await NoticeViewportMirrorService.PresentAsync(
                NoticeStrip,
                scroll,
                severity,
                message,
                showSave,
                animate,
                viewportAnchor: this).ConfigureAwait(true);
        }

        NoticeViewportMirrorService.SyncSaveEnabled(!_commitInProgress);
    }

    private async Task RetractSessionInformationalSuccessAsync()
    {
        if (FindNoticeStack() is not { } stack)
            return;

        var host = SettingsNoticeCoordinator.CreateShellHost(stack, FindPageScrollViewer());
        await SettingsNoticeCoordinator.RetractInformationalSuccessAsync(host, NoticeStrip).ConfigureAwait(true);
    }

    private SettingsPageNoticeStack? FindNoticeStack()
    {
        DependencyObject? current = this;
        while (current is not null)
        {
            if (current is SettingsPageNoticeStack stack)
                return stack;
            current = VisualTreeHelper.GetParent(current);
        }

        return null;
    }

    private void CloseNoticeImmediate()
    {
        if (NoticeStrip.IsVisuallyOpen)
            NoticeViewportMirrorService.ClosePinnedSourcePreservingScroll(NoticeStrip);
        NoticeViewportMirrorService.ReleaseSource(NoticeStrip);
    }

    private void OnOverlayBecameIdle(object? sender, EventArgs e)
    {
        if (!_deferNoticeUntilOverlayIdle)
            return;

        _deferNoticeUntilOverlayIdle = false;

        if (CommitScope is { } scope && SettingsSession.Current.HasUnsavedInScope(scope))
        {
            _dirtyPending = true;
            _pointerReleased = true;
            Refresh();
            return;
        }

        _dirtyPending = false;
        _pointerReleased = false;
        _procedureDebounceCts?.Cancel();
        PageSessionNoticeState.ClearSaveSuccessPending();
        if (!NoticeViewportMirrorService.IsInformationalMirrorActive)
            CloseNoticeImmediate();
    }

    private ScrollViewer? FindPageScrollViewer()
    {
        DependencyObject? current = this;
        while (current is not null)
        {
            if (current is SettingsPageShell shell)
                return shell.PageScrollViewer;
            current = VisualTreeHelper.GetParent(current);
        }

        return SettingsContentColumnLayout.GetPageContentScroll();
    }

    /// <summary>
    /// Session notice только на текущем NavFrame.Content — иначе кэш промежуточных страниц
    /// перехватывает DirtyChanged и закрепляет жёлтый notice не на detail.
    /// </summary>
    private bool BelongsToActivePage()
    {
        if (!IsLoaded)
            return false;

        Page? page = null;
        DependencyObject? current = this;
        while (current is not null)
        {
            if (current is Page p)
            {
                page = p;
                break;
            }

            current = VisualTreeHelper.GetParent(current);
        }

        if (page is null)
            return false;

        return ReferenceEquals(App.ShellWindow?.ActivePageRoot, page);
    }



    private void EnsureDirtySubscription()
    {
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
        SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e) => OnDirtyChanged();



    private async Task SaveAsync()

    {

        if (CommitScope is not { } scope || _commitInProgress)

            return;



        _commitInProgress = true;

        _dirtyPending = false;

        _pointerReleased = false;

        _procedureDebounceCts?.Cancel();

        NoticeStrip.IsSaveEnabled = false;

        try

        {

            if (FindNoticeStack() is { } saveStack)
            {
                var host = SettingsNoticeCoordinator.CreateShellHost(saveStack, FindPageScrollViewer());
                await SettingsNoticeCoordinator.RetractOnSaveAsync(
                    host,
                    NoticeStrip,
                    SettingsNoticeCoordinator.NoticeSlotColor.Yellow).ConfigureAwait(true);
            }
            else
                CloseNoticeImmediate();

            await SettingsSession.Current.ApplyCommitScopeAsync(scope).ConfigureAwait(true);

            PageSessionNoticeState.MarkSaveSucceeded(scope);

            if (!PageSessionNoticeState.ShouldHide(false)
                && PageSessionNoticeState.TryConsumePendingSuccessDisplay())
            {
                await SettingsInteractionDeferral.RunAfterUiSettledAsync(DispatcherQueue).ConfigureAwait(true);
                await SettingsInteractionDeferral.RunAfterUiSettledAsync(DispatcherQueue).ConfigureAwait(true);
                await Task.Delay(NoticeViewportMirrorService.InformationalLayoutSettleMs).ConfigureAwait(true);

                NoticeStrip.IsSaveEnabled = false;

                PageSessionNoticeState.RequestAnimateNextOpen();

                await ShowNoticeAsync(
                    SettingsNoticeSeverity.Success,
                    GlassSettingsResources.Get("Notice_SavedSuccess"),
                    showSave: false,
                    animate: true);

                PageSessionNoticeState.ClearSaveSuccessPending();
            }
            else
            {
                PageSessionNoticeState.ClearSaveSuccessPending();
            }

        }

        finally

        {

            _commitInProgress = false;

        }

    }

}



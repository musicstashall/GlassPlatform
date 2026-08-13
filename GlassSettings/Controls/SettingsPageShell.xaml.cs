using GlassSettings.Core.Layout;
using GlassSettings.Core.Snapshot;
using GlassSettings.Pages;
using GlassSettings.Services;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

[ContentProperty(Name = nameof(Body))]
public sealed partial class SettingsPageShell : UserControl
{
    private SettingsPageOperationGate? _operations;

    public static readonly DependencyProperty CommitScopeProperty =
        DependencyProperty.Register(nameof(CommitScope), typeof(SettingsCommitScope?), typeof(SettingsPageShell),
            new PropertyMetadata(null, (d, _) => ((SettingsPageShell)d).ApplyCommitScope()));

    public SettingsPageShell()
    {
        InitializeComponent();
        SettingsContentColumnLayout.AttachColumn(ContentColumn, ContentScroll);
        SettingsScrollEdgeFade.Attach(ContentScroll);
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        AddHandler(
            UIElement.PointerReleasedEvent,
            new PointerEventHandler(OnInteractionCompleted),
            handledEventsToo: true);
        AddHandler(
            UIElement.KeyUpEvent,
            new KeyEventHandler(OnInteractionCompletedKey),
            handledEventsToo: true);
    }

    public UIElement? Body
    {
        get => BodyHost.Content as UIElement;
        set => BodyHost.Content = value;
    }

    public SettingsCommitScope? CommitScope
    {
        get => (SettingsCommitScope?)GetValue(CommitScopeProperty);
        set => SetValue(CommitScopeProperty, value);
    }

    public ScrollViewer PageScrollViewer => ContentScroll;

    /// <summary>Serial-конвейер фаз страницы (Animating → RegistryMutation → Cosmetic → NoticeSync).</summary>
    public SettingsPageOperationGate Operations =>
        _operations ??= new SettingsPageOperationGate(DispatcherQueue);

    public static void NotifyPageNavigatedTo(Page page)
    {
        if (page is HomePage)
            return;

        if (page.Content is SettingsPageShell shell)
            shell.OnPageNavigatedTo();
    }

    public SettingsPageNoticeStack PageNoticeStack => NoticeStack;

    public void UpdateCommitScope(SettingsCommitScope scope)
    {
        CommitScope = scope;
        NoticeStack.Refresh();
    }

    public void RefreshSessionNotice() => NoticeStack.Refresh();

    public void RefreshSessionNoticeProgrammatic() => NoticeStack.RefreshProgrammatic();

    /// <summary>NoticeSync-фаза конвейера — один sync notice по финальному dirty.</summary>
    public void FlushSessionNotice() => NoticeStack.FlushPendingNoticeSync();

    public void NotifyInteractionCompleted() => NoticeStack.OnInteractionCompleted();

    private void OnPageNavigatedTo()
    {
        PageSessionNoticeState.ResetForNavigation();
        PageExternalSyncNoticeState.ClearPending();
        PageSessionNoticeState.SuppressAnimateForNavigation();
        EnsurePageScrollRegistration();
        ResolveCommitScopeFromPage();
        NoticeStack.RefreshForNavigation();
        SettingsUnsavedIndicatorRefresh.Refresh(this);

        // После NavFrame.Navigated и layout: у кэшированных hub/detail Loaded может не сработать.
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, ScheduleNoticeMirrorRefreshAfterNavigation);
    }

    private void ScheduleNoticeMirrorRefreshAfterNavigation()
    {
        EnsurePageScrollRegistration();
        if (ContentScroll.ViewportHeight <= 0)
            _ = RefreshNoticeAfterLayoutAsync();
        else
            _ = RefreshNoticeAndMirrorAfterNavigationAsync();
    }

    private void EnsurePageScrollRegistration()
    {
        SettingsContentColumnLayout.SetPageContentScroll(ContentScroll);
        NoticeStack.BindScrollViewer(ContentScroll);
        NoticeViewportMirrorService.RegisterNoticeStack(NoticeStack);
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
        SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
    }

    private async Task RefreshNoticeAndMirrorAfterNavigationAsync() =>
        await FinalizeNoticesAfterNavigationAsync().ConfigureAwait(true);

    private async Task RefreshNoticeAfterLayoutAsync()
    {
        for (var i = 0; i < 8; i++)
        {
            await Task.Delay(16).ConfigureAwait(true);
            ContentScroll.UpdateLayout();
            if (ContentScroll.ViewportHeight > 0)
                break;
        }

        NoticeStack.Refresh();
        await FinalizeNoticesAfterNavigationAsync().ConfigureAwait(true);
    }

    private async Task FinalizeNoticesAfterNavigationAsync()
    {
        // ExternalSync green не restore — только в момент sync-commit (DirtyChanged).
        await SettingsNoticeCoordinator.RestoreShellAfterNavigationAsync(NoticeStack, ContentScroll)
            .ConfigureAwait(true);
        await NoticeViewportMirrorService.ReattachPageAsync(NoticeStack, ContentScroll).ConfigureAwait(true);
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        EnsurePageScrollRegistration();
        ResolveCommitScopeFromPage();
        SettingsUnsavedIndicatorRefresh.Refresh(this);
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        SettingsContentColumnLayout.SetPageContentScroll(null);
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e)
    {
        if (SettingsSession.Current.IsShuttingDown)
            return;

        SettingsUnsavedIndicatorRefresh.Refresh(this);
        NoticeStack.OnDirtyChanged();

        if (SettingsSession.Current.LastDirtyChangedSource != SettingsSession.DirtyChangedSource.RegistryWatchSynced)
            return;

        if (SettingsPageOperationGate.Current is { ShouldDeferRegistryWatch: true })
            return;

        if (SettingsSession.Current.HasUserOriginDwmDirty())
        {
            RegistryWatchTrace.Write("Shell RegistryWatchSynced: skip — user-origin DWM dirty");
            SettingsSession.Current.ResetDirtyChangedSource();
            return;
        }

        if (FindParentPage() is IExternalRegistrySyncReloadable reloadable)
            reloadable.ReloadFromExternalRegistrySync();

        var showNotice = CommitScope is { } scope && PageExternalSyncNoticeState.ShouldShow(scope);
        RegistryWatchTrace.Write(
            $"Shell RegistryWatchSynced: showNotice={showNotice}, commitScope={CommitScope?.Kind}");

        _ = NoticeStack.TryShowExternalSyncNoticeAsync(
            animate: PageExternalSyncNoticeState.ConsumeAnimateNextOpen());

        SettingsSession.Current.ResetDirtyChangedSource();
    }

    private void OnInteractionCompleted(object sender, PointerRoutedEventArgs e) =>
        NoticeStack.OnInteractionCompleted();

    private void OnInteractionCompletedKey(object sender, KeyRoutedEventArgs e) =>
        NoticeStack.OnInteractionCompleted();

    private void ResolveCommitScopeFromPage()
    {
        var page = FindParentPage();
        if (page is ISettingsCommitScopeProvider provider)
            CommitScope = provider.GetCommitScope();
        else if (page is not null)
            CommitScope = CommitScopeRegistry.TryGetForPageType(page.GetType());

        ApplyCommitScope();
    }

    private void ApplyCommitScope()
    {
        NoticeStack.CommitScope = CommitScope;
        NoticeStack.Visibility = Visibility.Visible;
    }

    private Page? FindParentPage()
    {
        DependencyObject? current = this;
        while (current is not null)
        {
            if (current is Page page)
                return page;
            current = VisualTreeHelper.GetParent(current);
        }

        return null;
    }
}

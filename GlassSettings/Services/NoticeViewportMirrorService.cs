using GlassSettings.Controls;
using GlassSettings.Core.Layout;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Foundation;

namespace GlassSettings.Services;

/// <summary>
/// Зеркало notice у верхней кромки контента: дублирует открытые полосы страницы,
/// когда якорь под ними вне viewport ScrollViewer. См. GlassSettings/docs/GlassSettings-notices.md.
/// </summary>
internal static class NoticeViewportMirrorService
{
    private sealed class MirrorBinding
    {
        public required SettingsUnsavedNoticeStrip Source { get; init; }

        public EventHandler? DismissHandler { get; set; }
    }

    private static readonly List<MirrorBinding> _stackBindings = new();
    private static readonly List<MirrorBinding> _legacyBindings = new();

    private static SettingsPageNoticeStack? _activeStack;
    private static FrameworkElement? _legacyViewportAnchor;
    private static ScrollViewer? _subscribedScroll;
    private static bool _mirrorActive;
    private static bool _syncInProgress;
    private static bool _resyncRequested;
    private static bool _settleScheduled;
    private static SettingsUnsavedNoticeStrip? _informationalSource;
    private static bool _informationalMirrorActive;
    /// <summary>Hide+deposit informational в процессе — не уходить в pinned activity-dismiss.</summary>
    private static bool _informationalDismissInProgress;
    private static double _informationalScrollBaseline;
    private static long _informationalActivityAllowedAfterTick;

    internal const int InformationalActivityGraceMs = 600;
    internal const int InformationalLayoutSettleMs = 400;

    private const double ScrollTopStructuralEpsilon = 8;
    private const double ScrollOffsetDismissEpsilon = 4;

    /// <summary>
    /// Lifecycle ID pinned-зеркала: activity/X гасит только текущий ID;
    /// новое notice (мутация) → <see cref="NotifyNewPinnedNoticeEpoch"/> → новый ID → зеркало снова можно показать.
    /// </summary>
    private static long _pinnedMirrorEpochId;
    private static long _dismissedPinnedMirrorEpochId;

    /// <summary>Идёт SlideUpOut pinned-зеркала по activity — Sync не должен звать HideImmediate.</summary>
    private static bool _pinnedMirrorHideInProgress;

    /// <summary>
    /// Synthetic ChangeView после open/close штатной полосы off-screen — не считать user-scroll
    /// для activity-dismiss зеркала.
    /// </summary>
    private static int _scrollCompensationDepth;

    /// <summary>
    /// Идёт ClosePinned* / compensate — <see cref="SettingsUnsavedNoticeStrip.CloseImmediate"/>
    /// не должен рекурсивно звать ClosePinned снова.
    /// </summary>
    private static int _scrollPreservingCloseDepth;

    internal static bool IsInScrollPreservingClose => _scrollPreservingCloseDepth > 0;

    /// <summary>
    /// Подавление зеркала для конкретного pinned-error notice.
    /// Ключи не сбрасываются на навигации: suppress должен переживать возврат на страницу.
    /// </summary>
    private static readonly HashSet<string> _suppressedErrorMirrorKeys = new(StringComparer.Ordinal);

    private readonly struct ScrollLayoutCapture
    {
        public bool Enabled { get; init; }

        public ScrollViewer? Scroll { get; init; }

        public double ExtentBefore { get; init; }

        public double OffsetBefore { get; init; }
    }

    private static FrameworkElement? _windowActivityRoot;
    private static PointerEventHandler? _windowPointerPressedHandler;
    private static PointerEventHandler? _scrollWheelHandler;
    private static PointerEventHandler? _windowWheelHandler;

    /// <summary>Подпись показанного зеркала (включая SaveEnabled); пустая — скрыто.</summary>
    private static string _shownSignature = string.Empty;

    /// <summary>Подпись содержимого без SaveEnabled — diff только кнопки «Сохранить».</summary>
    private static string _shownContentSignature = string.Empty;

    public static bool IsMirrorActive => _mirrorActive;

    public static bool IsInformationalMirrorActive => _informationalMirrorActive;

    public static void Initialize() { }

    public static void RegisterNoticeStack(SettingsPageNoticeStack stack) => _activeStack = stack;

    /// <summary>
    /// После <see cref="OnPageNavigated"/> повторно привязать scroll и открытые полосы shell-страницы.
    /// Только quiet sync: при входе страница сверху — зеркало не активировать (см. scrollTopNoMirror).
    /// </summary>
    public static async Task ReattachPageAsync(SettingsPageNoticeStack stack, ScrollViewer? scrollViewport)
    {
        _activeStack = stack;
        var viewport = scrollViewport ?? SettingsContentColumnLayout.GetPageContentScroll();
        AttachScrollHandler(viewport);
        foreach (var strip in stack.GetOpenStrips())
            BindStackItem(strip);
        // animateShow всегда false — restore on nav не «поднимает» зеркало.
        await SyncMirrorToViewportAsync(viewport, animateShow: false, animateHide: false).ConfigureAwait(true);
        ScheduleSettleSync();
    }

    /// <summary>После навигации / внешней policy-mutation — legacy-полосы «Обзора» и «Процессы».</summary>
    public static async Task ReattachLegacyAsync(ScrollViewer? scrollViewport, FrameworkElement? viewportAnchor)
    {
        NoticeMirrorTrace.Write(
            $"ReattachLegacyAsync: anchor={(viewportAnchor is not null ? viewportAnchor.GetType().Name : "null")}, " +
            $"legacyBindings={_legacyBindings.Count}, subscribedScroll={(ReferenceEquals(_subscribedScroll, scrollViewport) ? "same" : "rebind")}");

        PruneStaleLegacyBindings();

        if (viewportAnchor is not null)
            _legacyViewportAnchor = viewportAnchor;

        var viewport = scrollViewport ?? SettingsContentColumnLayout.GetPageContentScroll();
        await EnsureScrollViewportReadyAsync(viewport).ConfigureAwait(true);
        AttachScrollHandler(viewport);

        foreach (var binding in _legacyBindings)
        {
            if (binding.Source.IsVisuallyOpen && _legacyViewportAnchor is not null)
                BindLegacySource(binding.Source, _legacyViewportAnchor);
        }

        await SyncMirrorToViewportAsync(viewport, animateShow: false, animateHide: false).ConfigureAwait(true);
        ScheduleSettleSync();
    }

    public static async Task SyncLegacyMirrorQuietAsync(ScrollViewer? scrollViewport)
    {
        await SyncMirrorToViewportAsync(scrollViewport, animateShow: false, animateHide: false).ConfigureAwait(true);
        ScheduleSettleSync();
    }

    public static void UnregisterNoticeStack(SettingsPageNoticeStack stack)
    {
        if (!ReferenceEquals(_activeStack, stack))
            return;

        ClearStackBindings();
        _activeStack = null;
        HideMirrorImmediate();
        DetachScrollHandler();
    }

    /// <summary>
    /// Смена страницы: немедленно снять зеркало и informational epoch без анимаций.
    /// Deposit Home footer — только через HomePage.OnNavigatedFrom / ConsumeGreenForNavigation.
    /// </summary>
    public static void OnPageNavigated()
    {
        NoticeMirrorTrace.Write(
            $"OnPageNavigated: stackBindings={_stackBindings.Count}, legacyBindings={_legacyBindings.Count}, " +
            $"mirrorActive={_mirrorActive}, mirrorOpen={App.ShellWindow?.IsTopNoticeMirrorOpen == true}");
        // HideMirrorImmediate сбрасывает informational flags + host (без SlideUpOut).
        HideMirrorImmediate(manualDismissForPage: false);
        ClearStackBindings();
        ClearLegacyBindings();
        _activeStack = null;
        ClearShownSignatures();
        _pinnedMirrorEpochId = 0;
        _dismissedPinnedMirrorEpochId = 0;
        _pinnedMirrorHideInProgress = false;
        _informationalScrollBaseline = 0;
        _informationalActivityAllowedAfterTick = 0;
        _legacyViewportAnchor = null;
        DetachScrollHandler();
    }

    /// <summary>
    /// Новое pinned-notice / новая мутация: новый lifecycle ID — activity-dismiss предыдущего зеркала не блокирует.
    /// </summary>
    public static void NotifyNewPinnedNoticeEpoch()
    {
        _pinnedMirrorEpochId++;
        NoticeMirrorTrace.Write(
            $"NotifyNewPinnedNoticeEpoch: epoch={_pinnedMirrorEpochId}, dismissed={_dismissedPinnedMirrorEpochId}");
    }

    private static bool IsPinnedMirrorDismissedForCurrentEpoch() =>
        _pinnedMirrorEpochId > 0 && _dismissedPinnedMirrorEpochId == _pinnedMirrorEpochId;

    private static void DismissPinnedMirrorForCurrentEpoch()
    {
        if (_pinnedMirrorEpochId <= 0)
            _pinnedMirrorEpochId = 1;
        _dismissedPinnedMirrorEpochId = _pinnedMirrorEpochId;
    }

    private static void EnsurePinnedMirrorEpochStarted()
    {
        if (_pinnedMirrorEpochId <= 0)
            _pinnedMirrorEpochId = 1;
    }

    public static bool ShouldUseMirror(FrameworkElement anchor, ScrollViewer? scrollViewport) =>
        GetMirrorViewportState(Array.Empty<SettingsUnsavedNoticeStrip>(), null, anchor, scrollViewport)
            == NoticeMirrorViewportState.NotVisible;

    private enum NoticeMirrorViewportState
    {
        Visible,
        NotVisible,
        LayoutNotReady,
        ElementNotReady
    }

    private readonly struct PinnedMirrorPolicy
    {
        public bool MustHide { get; init; }

        public bool MustShow { get; init; }

        public string Reason { get; init; }
    }

    public static async Task PresentAsync(
        SettingsUnsavedNoticeStrip source,
        ScrollViewer? scrollViewport,
        SettingsNoticeSeverity severity,
        string message,
        bool showSave,
        bool animate,
        FrameworkElement? viewportAnchor = null)
    {
        var anchor = viewportAnchor ?? source;
        var viewport = scrollViewport ?? SettingsContentColumnLayout.GetPageContentScroll();
        NoticeMirrorTrace.Write(
            $"PresentAsync: source={DescribeStrip(source)}, severity={severity}, showSave={showSave}, animate={animate}, " +
            $"anchor={anchor.GetType().Name}, viewportH={viewport?.ViewportHeight ?? 0}");

        await EnsureScrollViewportReadyAsync(viewport).ConfigureAwait(true);

        if (TryFindNoticeStack(source) is { } stack)
        {
            NoticeMirrorTrace.Write("PresentAsync: path=stack");
            await PresentStackItemAsync(stack, source, viewport, severity, message, showSave, animate)
                .ConfigureAwait(true);
            return;
        }

        NoticeMirrorTrace.Write("PresentAsync: path=legacy");
        PruneStaleLegacyBindings();
        BindLegacySource(source, anchor);
        await OpenSourceStripAsync(source, anchor, viewport, severity, message, showSave, animate)
            .ConfigureAwait(true);
        AttachScrollHandler(viewport);
    }

    public static async Task PresentStackItemAsync(
        SettingsPageNoticeStack stack,
        SettingsUnsavedNoticeStrip strip,
        ScrollViewer? scrollViewport,
        SettingsNoticeSeverity severity,
        string message,
        bool showSave,
        bool animate)
    {
        _activeStack = stack;
        var viewport = scrollViewport ?? SettingsContentColumnLayout.GetPageContentScroll();
        await EnsureScrollViewportReadyAsync(viewport).ConfigureAwait(true);

        BindStackItem(strip);
        var anchor = stack.ViewportAnchorElement;
        await OpenSourceStripAsync(strip, anchor, viewport, severity, message, showSave, animate, stack)
            .ConfigureAwait(true);
        AttachScrollHandler(viewport);
    }

    public static Task SyncMirrorForStackAsync(SettingsPageNoticeStack stack, ScrollViewer? scrollViewport)
    {
        _activeStack = stack;
        return SyncMirrorToViewportAsync(scrollViewport, animateShow: true, animateHide: true);
    }

    /// <summary>
    /// Тихий sync при повторных мутациях настроек при уже открытом notice — без slide-in/out.
    /// Прокрутка использует обычный <see cref="SyncMirrorForStackAsync"/>.
    /// </summary>
    public static async Task SyncMirrorForStackQuietAsync(SettingsPageNoticeStack stack, ScrollViewer? scrollViewport)
    {
        _activeStack = stack;
        await SyncMirrorToViewportAsync(scrollViewport, animateShow: false, animateHide: false).ConfigureAwait(true);
        ScheduleSettleSync();
    }

    public static void ReleaseStackItem(SettingsUnsavedNoticeStrip source)
    {
        NoticeMirrorTrace.Write($"ReleaseStackItem: {DescribeStrip(source)}");
        RemoveStackBinding(source);
        _ = SyncMirrorToViewportAsync(_subscribedScroll, animateShow: false, animateHide: false);
    }

    /// <summary>Удалить binding для source без sync зеркала (для ручного dismiss зеркала).</summary>
    public static void ReleaseStackItemWithoutSync(SettingsUnsavedNoticeStrip source)
    {
        NoticeMirrorTrace.Write($"ReleaseStackItemWithoutSync: {DescribeStrip(source)}");
        RemoveStackBinding(source);
    }

    public static void ReleaseSource(SettingsUnsavedNoticeStrip source)
    {
        NoticeMirrorTrace.Write(
            $"ReleaseSource: {DescribeStrip(source)}, IsStack={IsStackSource(source)}, IsLegacy={IsLegacySource(source)}");
        ClearMirrorSuppressForSource(source);
        if (IsStackSource(source))
        {
            ReleaseStackItem(source);
            return;
        }

        if (!IsLegacySource(source))
            return;

        RemoveLegacyBinding(source);
        _ = SyncMirrorToViewportAsync(_subscribedScroll, animateShow: false, animateHide: false);
    }

    /// <summary>Release legacy binding + тихий sync без HideMirrorImmediate (hide одной полосы).</summary>
    public static void ReleaseLegacySourceAndSyncQuiet(SettingsUnsavedNoticeStrip source) =>
        ReleaseSource(source);

    /// <summary>Удалить binding для source без sync зеркала (для ручного dismiss зеркала).</summary>
    public static void ReleaseSourceWithoutSync(SettingsUnsavedNoticeStrip source)
    {
        NoticeMirrorTrace.Write(
            $"ReleaseSourceWithoutSync: {DescribeStrip(source)}, IsStack={IsStackSource(source)}, IsLegacy={IsLegacySource(source)}");
        if (IsStackSource(source))
        {
            RemoveStackBinding(source);
            return;
        }

        if (!IsLegacySource(source))
            return;

        RemoveLegacyBinding(source);
    }

    /// <summary>
    /// Закрыт крестик зеркала pinned-error notice: не показывать это зеркало повторно,
    /// пока не сменится content/key или пока source не будет штатно закрыт.
    /// </summary>
    public static void SuppressMirrorForSource(SettingsUnsavedNoticeStrip source)
    {
        if (source.Severity != SettingsNoticeSeverity.Error)
            return;

        var key = ResolveMirrorSuppressKey(source);
        _suppressedErrorMirrorKeys.Add(key);
        NoticeMirrorTrace.Write($"SuppressMirrorForSource: key={key}");
    }

    /// <summary>Снять suppress для source при штатном закрытии notice.</summary>
    public static void ClearMirrorSuppressForSource(SettingsUnsavedNoticeStrip source)
    {
        if (source.Severity != SettingsNoticeSeverity.Error)
            return;

        var key = ResolveMirrorSuppressKey(source);
        if (_suppressedErrorMirrorKeys.Remove(key))
            NoticeMirrorTrace.Write($"ClearMirrorSuppressForSource: key={key}");
    }

    public static void HideMirror() => _ = HideMirrorAnimatedAsync();

    public static void HideMirrorImmediate(bool manualDismissForPage = false)
    {
        NoticeMirrorTrace.Write(
            $"HideMirrorImmediate: manualDismiss={manualDismissForPage}, mirrorActive={_mirrorActive}, " +
            $"mirrorOpen={App.ShellWindow?.IsTopNoticeMirrorOpen == true}");
        if (manualDismissForPage)
            DismissPinnedMirrorForCurrentEpoch();

        DetachMirrorActivityHandlers();
        _informationalMirrorActive = false;
        _informationalDismissInProgress = false;
        _informationalSource = null;
        _informationalActivityAllowedAfterTick = 0;

        if (!_mirrorActive && App.ShellWindow?.IsTopNoticeMirrorOpen != true)
            return;

        _mirrorActive = false;
        ClearShownSignatures();
        App.ShellWindow?.HideTopNoticeMirrorImmediate();
    }

    /// <summary>
    /// Startup green → footer на Обзоре: если зона карточки/футера вне viewport и страница
    /// прокручена — мгновенный reveal + compensate VerticalOffset на Δ высоты (без анимации MaxHeight).
    /// Возвращает true, если off-screen путь выполнен.
    /// </summary>
    public static bool TryRevealHomeFooterOffScreenPreservingScroll(
        ScrollViewer? scroll,
        FrameworkElement visibilityProbe,
        Action revealImmediate)
    {
        if (scroll is null || !scroll.IsLoaded)
            return false;

        if (scroll.VerticalOffset <= ScrollTopStructuralEpsilon)
            return false;

        var state = GetElementViewportState(visibilityProbe, scroll);
        // В кадре — caller анимирует высоту; вне кадра / не готов — compensate.
        if (state == NoticeMirrorViewportState.Visible)
            return false;

        var capture = BeginScrollLayoutCapture(scroll, enabled: true);
        RunWithAncestorChildrenTransitionsSuppressed(visibilityProbe, () =>
        {
            revealImmediate();
            CommitScrollLayoutCompensation(capture);
        });
        return true;
    }

    /// <summary>
    /// Закрыть штатную pinned-полосу; если зона notice вне viewport — компенсировать VerticalOffset
    /// на Δ ExtentHeight (контент в кадре не прыгает).
    /// </summary>
    public static void ClosePinnedSourcePreservingScroll(SettingsUnsavedNoticeStrip source)
    {
        if (!source.IsVisuallyOpen)
            return;

        var scroll = _subscribedScroll ?? SettingsContentColumnLayout.GetPageContentScroll();
        var should = ShouldCompensateScrollForPinnedStrip(source, scroll);
        var capture = BeginScrollLayoutCapture(scroll, should);
        _scrollPreservingCloseDepth++;
        try
        {
            // RepositionThemeTransition на shell/Home иначе анимирует BodyHost вверх — визуальный «прыжок»
            // даже когда VerticalOffset уже скорректирован платформой.
            RunWithAncestorChildrenTransitionsSuppressed(source, () =>
            {
                source.CloseImmediateCore();
                CommitScrollLayoutCompensation(capture);
            });
        }
        finally
        {
            _scrollPreservingCloseDepth--;
        }
    }

    /// <summary>Асинхронный вариант <see cref="ClosePinnedSourcePreservingScroll"/> (fade или immediate).</summary>
    public static async Task ClosePinnedSourcePreservingScrollAsync(
        SettingsUnsavedNoticeStrip source,
        bool animate)
    {
        if (!source.IsVisuallyOpen)
            return;

        var scroll = _subscribedScroll ?? SettingsContentColumnLayout.GetPageContentScroll();
        var should = ShouldCompensateScrollForPinnedStrip(source, scroll);
        var capture = BeginScrollLayoutCapture(scroll, should);
        _scrollPreservingCloseDepth++;
        try
        {
            if (animate)
            {
                // Fade при полной высоте; collapse + compensate — без RepositionThemeTransition.
                await source.FadeOutForPreservingCloseAsync().ConfigureAwait(true);
                RunWithAncestorChildrenTransitionsSuppressed(source, () =>
                {
                    source.CloseImmediateCore();
                    CommitScrollLayoutCompensation(capture);
                });
            }
            else
            {
                RunWithAncestorChildrenTransitionsSuppressed(source, () =>
                {
                    source.CloseImmediateCore();
                    CommitScrollLayoutCompensation(capture);
                });
            }
        }
        finally
        {
            _scrollPreservingCloseDepth--;
        }
    }

    /// <summary>
    /// Крестик на informational-зеркале: slide-out host (1000 ms), затем NotifyDismiss/footer.
    /// Не вызывать HideMirrorImmediate — иначе политика анимации ломается.
    /// </summary>
    public static Task CompleteInformationalDismissFromMirrorAsync(SettingsUnsavedNoticeStrip source)
    {
        if (!_informationalMirrorActive)
        {
            // Epoch уже снят — всё равно осадить footer, если pending.
            HomeSessionNoticeState.DepositStartupGreenToFooterIfPending();
            return Task.CompletedTask;
        }

        // source уже в _informationalSource; параметр — для идемпотентности API крестика.
        _ = source;
        return CompleteInformationalDismissAsync(animateMirror: true);
    }

    public static Task HideMirrorAnimatedAsync()
    {
        if (!_mirrorActive && App.ShellWindow?.IsTopNoticeMirrorOpen != true)
            return Task.CompletedTask;

        return DeactivateMirrorAsync(animate: true);
    }

    public static void HideMirrorIfSource(SettingsUnsavedNoticeStrip source)
    {
        // Пока идёт animated informational dismiss — не стартовать второй hide/sync (CancelActive → 0 ms).
        if (_informationalDismissInProgress)
            return;

        if (IsStackSource(source))
        {
            if (_activeStack is not null)
                _ = SyncMirrorForStackAsync(_activeStack, _subscribedScroll);
            return;
        }

        if (!IsLegacySource(source))
            return;

        // На «Обзоре» две legacy-полосы (зелёная+жёлтая) могут зеркалиться одновременно. Закрытие одной
        // не должно убирать зеркало другой — если остались открытые полосы, пересобираем зеркало по ним.
        if (GetOpenLegacyStrips().Count > 0)
            _ = SyncMirrorToViewportAsync(_subscribedScroll, animateShow: false, animateHide: true);
        else
            _ = HideMirrorAnimatedAsync();
    }

    /// <summary>
    /// Синхронизировать состояние кнопки «Сохранить» на зеркале и подпись показанного зеркала,
    /// чтобы дедуп <see cref="SyncMirrorToViewportAsync"/> не уходил в рассинхрон.
    /// </summary>
    public static void SyncSaveEnabled(bool enabled)
    {
        App.ShellWindow?.SyncTopNoticeMirrorSaveEnabled(enabled);

        if (_shownSignature.Length == 0)
            return;

        var openStrips = GetCurrentOpenStrips();
        if (openStrips.Count == 0)
            return;

        var specs = BuildSpecs(openStrips);
        if (specs.Count == 0)
            return;

        SetShownSignatures(BuildSignature(specs), specs);
    }

    private static async Task OpenSourceStripAsync(
        SettingsUnsavedNoticeStrip source,
        FrameworkElement anchor,
        ScrollViewer? viewport,
        SettingsNoticeSeverity severity,
        string message,
        bool showSave,
        bool animate,
        SettingsPageNoticeStack? stack = null)
    {
        var showAction = source.ShowActionButton;
        var actionText = source.ActionButtonText;

        if (!source.DisableInformationalMirror
            && NoticePresentationPolicy.IsInformationalMirrorOnly(severity, showSave, showAction))
        {
            if (stack is not null)
                _activeStack = stack;
            await PresentInformationalMirrorAsync(source, viewport, severity, message, animate, stack)
                .ConfigureAwait(true);
            return;
        }

        await CancelInformationalForPinnedTransitionAsync(source).ConfigureAwait(true);

        var useMirror = ShouldActivateMirrorForOpen(stack, anchor, viewport, source);
        NoticeMirrorTrace.Write(
            $"OpenSourceStripAsync: {DescribeStrip(source)}, alreadyOpen={source.IsVisuallyOpen}, useMirror={useMirror}, " +
            $"disableInfoMirror={source.DisableInformationalMirror}");

        if (source.IsVisuallyOpen)
        {
            // Полоса уже открыта (в т.ч. restore после nav): контент на месте, зеркало — только quiet sync.
            // Не animateShow: иначе при возврате на страницу сверху возможен ложный slide-in.
            source.OpenImmediate(severity, message, showSave, showAction, actionText);
            await SyncMirrorToViewportAsync(
                viewport,
                animateShow: false,
                animateHide: false).ConfigureAwait(true);
            return;
        }

        // Штатная полоса всегда в layout; slide-in только когда зона notice в кадре.
        // useMirror часто false до первого measure полосы — ориентируемся на VerticalOffset:
        // при scroll вниз OpenAnimatedAsync всё равно занимает высоту с 1-го кадра → прыжок без compensate.
        var scroll = viewport ?? _subscribedScroll ?? SettingsContentColumnLayout.GetPageContentScroll();
        var scrolledAway = scroll is not null
            && scroll.IsLoaded
            && scroll.VerticalOffset > ScrollTopStructuralEpsilon;
        if (useMirror || !animate || scrolledAway)
        {
            var capture = BeginScrollLayoutCapture(scroll, enabled: scrolledAway);
            RunWithAncestorChildrenTransitionsSuppressed(source, () =>
            {
                source.OpenImmediate(severity, message, showSave, showAction, actionText);
                CommitScrollLayoutCompensation(capture);
            });
        }
        else
        {
            await source.OpenAnimatedAsync(severity, message, showSave, showAction, actionText).ConfigureAwait(true);
        }

        await SyncMirrorToViewportAsync(
            viewport,
            animateShow: useMirror || scrolledAway,
            animateHide: true).ConfigureAwait(true);

        // Полоса может быть ещё не измерена в момент первого sync (ActualHeight==0) → зеркало «не
        // появляется». Доигрываем на устоявшемся layout.
        ScheduleSettleSync();
    }

    private static async Task SyncMirrorToViewportAsync(
        ScrollViewer? scrollViewport,
        bool animateShow,
        bool animateHide)
    {
        NoticeMirrorTrace.Write(
            $"SyncMirrorToViewportAsync: начало animateShow={animateShow}, animateHide={animateHide}, " +
            $"informational={_informationalMirrorActive}");
        await EnsurePinnedMirrorScrollUnblockedAsync().ConfigureAwait(true);
        if (_informationalMirrorActive || _informationalDismissInProgress || _pinnedMirrorHideInProgress)
        {
            NoticeMirrorTrace.Write(
                $"SyncMirrorToViewportAsync: skip — informational/pinned hide in progress " +
                $"(info={_informationalMirrorActive}, infoDismiss={_informationalDismissInProgress}, pinnedHide={_pinnedMirrorHideInProgress})");
            return;
        }

        if (_syncInProgress)
        {
            _resyncRequested = true;
            NoticeMirrorTrace.Write("SyncMirrorToViewportAsync: sync уже в процессе, запрошен resync");
            return;
        }

        _syncInProgress = true;
        try
        {
            do
            {
                _resyncRequested = false;

                var viewport = scrollViewport ?? _subscribedScroll ?? SettingsContentColumnLayout.GetPageContentScroll();
                if (viewport is null)
                    return;
                AttachScrollHandler(viewport);

                var stackOpen = _activeStack?.GetOpenStrips() ?? Array.Empty<SettingsUnsavedNoticeStrip>();
                var legacyOpen = GetOpenLegacyStrips();
                var openStrips = stackOpen.Count > 0 ? stackOpen : legacyOpen;

                NoticeMirrorTrace.Write(
                    $"SyncMirrorToViewportAsync: stackOpen={stackOpen.Count}, legacyOpen={legacyOpen.Count}, " +
                    $"openStrips={openStrips.Count}, viewportH={viewport.ViewportHeight}, scrollOffset={viewport.VerticalOffset:F0}");

                var anchor = ResolveViewportAnchor(stackOpen.Count > 0);
                var mirrorOpen = App.ShellWindow?.IsTopNoticeMirrorOpen == true;
                var policy = EvaluatePinnedMirrorPolicy(openStrips, _activeStack, anchor, viewport);

                NoticeMirrorTrace.Write(
                    $"SyncMirrorToViewportAsync: policy MustHide={policy.MustHide}, MustShow={policy.MustShow}, " +
                    $"reason={policy.Reason}, mirrorOpen={mirrorOpen}, anchor={DescribeElement(anchor)}");

                if (policy.MustHide && mirrorOpen)
                {
                    // dismissedEpoch / activity: всегда slide-out; quiet settle не должен Immediate.
                    var hideAnimated = animateHide || policy.Reason is "dismissedEpoch";
                    NoticeMirrorTrace.Write(
                        $"SyncMirrorToViewportAsync: деактивация зеркала (policy MustHide reason={policy.Reason}, animate={hideAnimated})");
                    await DeactivateMirrorAsync(hideAnimated).ConfigureAwait(true);
                    continue;
                }

                if (mirrorOpen && !policy.MustHide && !policy.MustShow)
                {
                    if (openStrips.Count == 0
                        || IsPinnedStripStructurallyOnScreen(openStrips, viewport))
                    {
                        NoticeMirrorTrace.Write(
                            $"SyncMirrorToViewportAsync: indeterminate → structural hide reason={policy.Reason}");
                        await DeactivateMirrorAsync(animateHide).ConfigureAwait(true);
                        continue;
                    }

                    NoticeMirrorTrace.Write(
                        $"SyncMirrorToViewportAsync: indeterminate defer reason={policy.Reason}");
                    continue;
                }

                if (!policy.MustShow || IsPinnedMirrorDismissedForCurrentEpoch())
                    continue;

                var specs = BuildSpecs(openStrips);
                if (specs.Count == 0)
                    continue;

                var signature = BuildSignature(specs);
                var contentSignature = BuildContentSignature(specs);

                NoticeMirrorTrace.Write($"SyncMirrorToViewportAsync: MustShow, specs={specs.Count}, sigLen={signature.Length}");

                if (mirrorOpen && string.Equals(_shownContentSignature, contentSignature, StringComparison.Ordinal))
                {
                    if (!string.Equals(_shownSignature, signature, StringComparison.Ordinal))
                    {
                        var saveEnabled = specs.Count > 0 && specs[0].SaveEnabled;
                        App.ShellWindow?.SyncTopNoticeMirrorSaveEnabled(saveEnabled);
                        SetShownSignatures(signature, specs);
                    }
                }
                else
                {
                    // Первый показ host — всегда slide-in (quiet settle/nav не должен глотать анимацию).
                    // Уже открытое зеркало — Update без переанимации.
                    var effectiveAnimateShow = !mirrorOpen;

                    if (!mirrorOpen)
                    {
                        NoticeMirrorTrace.Write(
                            $"SyncMirrorToViewportAsync: активация зеркала, specs={specs.Count}, animate={effectiveAnimateShow} (callerAnimateShow={animateShow})");
                        await ActivateStackMirrorAsync(specs, effectiveAnimateShow).ConfigureAwait(true);
                        SetShownSignatures(signature, specs);
                    }
                    else if (!string.Equals(_shownSignature, signature, StringComparison.Ordinal))
                    {
                        NoticeMirrorTrace.Write($"SyncMirrorToViewportAsync: UpdateTopNoticeMirrorStack, specs={specs.Count}");
                        App.ShellWindow?.UpdateTopNoticeMirrorStack(specs);
                        SetShownSignatures(signature, specs);
                    }
                }
            }
            while (_resyncRequested);
        }
        finally
        {
            _syncInProgress = false;
            if (_resyncRequested)
            {
                NoticeMirrorTrace.Write("SyncMirrorToViewportAsync: finally — отложенный resync");
                _resyncRequested = false;
                await SyncMirrorToViewportAsync(scrollViewport, animateShow: false, animateHide: false).ConfigureAwait(true);
            }
        }

        NoticeMirrorTrace.Write("SyncMirrorToViewportAsync: конец");
    }

    /// <summary>
    /// Отложенные пересчёты после остановки прокрутки / открытия полосы, когда layout ещё не устоялся.
    /// </summary>
    private static void ScheduleSettleSync()
    {
        if (_informationalMirrorActive || _settleScheduled)
            return;

        NoticeMirrorTrace.Write("ScheduleSettleSync: запланирован settle (4×70ms)");
        _settleScheduled = true;
        _ = RunSettleSyncAsync();
    }

    private static async Task RunSettleSyncAsync()
    {
        try
        {
            for (var i = 0; i < 4; i++)
            {
                await Task.Delay(70).ConfigureAwait(true);
                if (_informationalMirrorActive)
                    continue;

                NoticeMirrorTrace.Write($"RunSettleSyncAsync: проход {i + 1}/4");
                await SyncMirrorToViewportAsync(_subscribedScroll, animateShow: false, animateHide: false).ConfigureAwait(true);
            }
        }
        finally
        {
            _settleScheduled = false;
            NoticeMirrorTrace.Write("RunSettleSyncAsync: завершён");
        }
    }

    private static FrameworkElement? ResolveViewportAnchor(bool useStack)
    {
        if (useStack && _activeStack is not null)
            return _activeStack.ViewportAnchorElement;

        return _legacyViewportAnchor;
    }

    private static IReadOnlyList<SettingsUnsavedNoticeStrip> GetCurrentOpenStrips()
    {
        var stackOpen = _activeStack?.GetOpenStrips() ?? Array.Empty<SettingsUnsavedNoticeStrip>();
        return stackOpen.Count > 0 ? stackOpen : GetOpenLegacyStrips();
    }

    private static List<SettingsUnsavedNoticeStrip> GetOpenLegacyStrips()
    {
        var list = new List<SettingsUnsavedNoticeStrip>();
        foreach (var binding in _legacyBindings)
        {
            if (binding.Source.IsLoaded && binding.Source.IsVisuallyOpen)
                list.Add(binding.Source);
        }

        if (_legacyViewportAnchor is Panel panel && list.Count > 1)
        {
            list.Sort((a, b) =>
                GetStripOrderInAnchor(panel, a).CompareTo(GetStripOrderInAnchor(panel, b)));
        }

        return list;
    }

    private static int GetStripOrderInAnchor(Panel anchor, SettingsUnsavedNoticeStrip strip)
    {
        for (var i = 0; i < anchor.Children.Count; i++)
        {
            if (ReferenceEquals(anchor.Children[i], strip))
                return i;

            if (anchor.Children[i] is FrameworkElement child && ContainsDescendant(child, strip))
                return i;
        }

        return int.MaxValue;
    }

    private static bool ContainsDescendant(DependencyObject root, DependencyObject target)
    {
        if (ReferenceEquals(root, target))
            return true;

        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            if (ContainsDescendant(VisualTreeHelper.GetChild(root, i), target))
                return true;
        }

        return false;
    }

    private static async Task ActivateStackMirrorAsync(
        IReadOnlyList<TopNoticeMirrorStripSpec> specs,
        bool animate)
    {
        if (App.ShellWindow is null || specs.Count == 0)
            return;

        EnsurePinnedMirrorEpochStarted();
        _mirrorActive = true;
        BeginMirrorActivityGrace();
        _informationalScrollBaseline = _subscribedScroll?.VerticalOffset ?? 0;
        await App.ShellWindow.ShowTopNoticeMirrorStackAsync(specs, animate).ConfigureAwait(true);
        AttachMirrorActivityHandlers();
    }

    private static async Task DeactivateMirrorAsync(bool animate)
    {
        DetachMirrorActivityHandlers();
        _mirrorActive = false;
        ClearShownSignatures();

        if (App.ShellWindow is { } shell)
            await shell.HideTopNoticeMirrorAsync(animate).ConfigureAwait(true);
    }

    private static bool IsMirrorSuppressed(SettingsUnsavedNoticeStrip strip) =>
        strip.Severity == SettingsNoticeSeverity.Error
        && _suppressedErrorMirrorKeys.Contains(ResolveMirrorSuppressKey(strip));

    private static string ResolveMirrorSuppressKey(SettingsUnsavedNoticeStrip strip)
    {
        if (!string.IsNullOrWhiteSpace(strip.MirrorSuppressKey))
            return strip.MirrorSuppressKey!;

        var actionText = strip.ShowActionButton
            ? strip.ActionButtonText ?? string.Empty
            : string.Empty;
        return $"{(int)strip.Severity}|{strip.ShowSaveButton}|{strip.ShowActionButton}|{actionText}|{strip.Message}";
    }

    private static List<TopNoticeMirrorStripSpec> BuildSpecs(IReadOnlyList<SettingsUnsavedNoticeStrip> sources)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var specs = new List<TopNoticeMirrorStripSpec>();
        foreach (var strip in sources)
        {
            if (IsMirrorSuppressed(strip))
            {
                NoticeMirrorTrace.Write($"BuildSpecs: skip suppressed {DescribeStrip(strip)}");
                continue;
            }

            var actionText = strip.ShowActionButton
                ? strip.ActionButtonText ?? string.Empty
                : string.Empty;
            var key = $"{(int)strip.Severity}|{strip.ShowSaveButton}|{strip.ShowActionButton}|{actionText}|{strip.Message}";
            if (!seen.Add(key))
                continue;

            specs.Add(new TopNoticeMirrorStripSpec(
                strip.Severity,
                strip.Message,
                strip.ShowSaveButton,
                strip.IsSaveEnabled,
                strip,
                strip.ShowActionButton,
                strip.ActionButtonText));
        }

        return specs;
    }

    private static string BuildSignature(IReadOnlyList<TopNoticeMirrorStripSpec> specs) =>
        string.Join("\u2063", specs.Select(s => $"{(int)s.Severity}|{s.ShowSave}|{s.ShowAction}|{s.SaveEnabled}|{s.ActionText}|{s.Message}"));

    private static string BuildContentSignature(IReadOnlyList<TopNoticeMirrorStripSpec> specs) =>
        string.Join("\u2063", specs.Select(s => $"{(int)s.Severity}|{s.ShowSave}|{s.ShowAction}|{s.ActionText}|{s.Message}"));

    private static void SetShownSignatures(string signature, IReadOnlyList<TopNoticeMirrorStripSpec> specs)
    {
        _shownSignature = signature;
        _shownContentSignature = BuildContentSignature(specs);
    }

    private static void ClearShownSignatures()
    {
        _shownSignature = string.Empty;
        _shownContentSignature = string.Empty;
    }

    private static void BindStackItem(SettingsUnsavedNoticeStrip source)
    {
        if (_stackBindings.Any(b => ReferenceEquals(b.Source, source)))
            return;

        var binding = new MirrorBinding { Source = source };
        binding.DismissHandler = (_, _) => HideMirrorIfSource(source);
        source.DismissRequested += binding.DismissHandler;
        _stackBindings.Add(binding);
    }

    private static void RemoveStackBinding(SettingsUnsavedNoticeStrip source)
    {
        for (var i = _stackBindings.Count - 1; i >= 0; i--)
        {
            if (!ReferenceEquals(_stackBindings[i].Source, source))
                continue;

            if (_stackBindings[i].DismissHandler is not null)
                source.DismissRequested -= _stackBindings[i].DismissHandler;

            _stackBindings.RemoveAt(i);
        }
    }

    private static void ClearStackBindings()
    {
        foreach (var binding in _stackBindings)
        {
            if (binding.DismissHandler is not null)
                binding.Source.DismissRequested -= binding.DismissHandler;
        }

        _stackBindings.Clear();
    }

    private static bool IsStackSource(SettingsUnsavedNoticeStrip source) =>
        _stackBindings.Any(b => ReferenceEquals(b.Source, source));

    private static bool IsLegacySource(SettingsUnsavedNoticeStrip source) =>
        _legacyBindings.Any(b => ReferenceEquals(b.Source, source));

    private static SettingsPageNoticeStack? TryFindNoticeStack(SettingsUnsavedNoticeStrip source)
    {
        DependencyObject? current = source;
        while (current is not null)
        {
            if (current is SettingsPageNoticeStack stack)
                return stack;
            current = VisualTreeHelper.GetParent(current);
        }

        return null;
    }

    private static void BindLegacySource(SettingsUnsavedNoticeStrip source, FrameworkElement anchor)
    {
        if (_legacyBindings.Any(b => ReferenceEquals(b.Source, source)))
        {
            _legacyViewportAnchor = anchor;
            NoticeMirrorTrace.Write($"BindLegacySource: уже привязан {DescribeStrip(source)}, anchor={DescribeElement(anchor)}");
            return;
        }

        NoticeMirrorTrace.Write($"BindLegacySource: новый {DescribeStrip(source)}, anchor={DescribeElement(anchor)}");

        var binding = new MirrorBinding { Source = source };
        binding.DismissHandler = (_, _) => HideMirrorIfSource(source);
        source.DismissRequested += binding.DismissHandler;
        _legacyBindings.Add(binding);
        _legacyViewportAnchor = anchor;
    }

    private static void RemoveLegacyBinding(SettingsUnsavedNoticeStrip source)
    {
        for (var i = _legacyBindings.Count - 1; i >= 0; i--)
        {
            if (!ReferenceEquals(_legacyBindings[i].Source, source))
                continue;

            if (_legacyBindings[i].DismissHandler is not null)
                source.DismissRequested -= _legacyBindings[i].DismissHandler;

            _legacyBindings.RemoveAt(i);
        }

        if (_legacyBindings.Count == 0)
            _legacyViewportAnchor = null;
    }

    private static void ClearLegacyBindings()
    {
        foreach (var binding in _legacyBindings)
        {
            if (binding.DismissHandler is not null)
                binding.Source.DismissRequested -= binding.DismissHandler;
        }

        _legacyBindings.Clear();
        _legacyViewportAnchor = null;
    }

    private static void AttachScrollHandler(ScrollViewer? scroll)
    {
        if (ReferenceEquals(_subscribedScroll, scroll))
            return;

        NoticeMirrorTrace.Write(
            $"AttachScrollHandler: rebind scroll (was={_subscribedScroll is not null}, new={scroll is not null})");
        DetachScrollHandler();
        _subscribedScroll = scroll;
        if (scroll is not null)
        {
            scroll.ViewChanged += OnScrollViewChanged;

            if (IsAnyMirrorOpenForActivity())
            {
                _scrollWheelHandler = OnMirrorScrollWheelChanged;
                scroll.AddHandler(UIElement.PointerWheelChangedEvent, _scrollWheelHandler, handledEventsToo: true);
            }
        }
    }

    private static void DetachScrollHandler()
    {
        if (_subscribedScroll is null)
            return;

        NoticeMirrorTrace.Write("DetachScrollHandler");
        _subscribedScroll.ViewChanged -= OnScrollViewChanged;
        if (_scrollWheelHandler is not null)
            _subscribedScroll.RemoveHandler(UIElement.PointerWheelChangedEvent, _scrollWheelHandler);

        _subscribedScroll = null;
        _scrollWheelHandler = null;
    }

    private static void OnScrollViewChanged(object? sender, ScrollViewerViewChangedEventArgs e)
    {
        NoticeMirrorTrace.Write(
            $"OnScrollViewChanged: intermediate={e.IsIntermediate}, offset={_subscribedScroll?.VerticalOffset:F0}, " +
            $"informational={_informationalMirrorActive}, mirrorActive={_mirrorActive}");
        _ = OnScrollViewChangedAsync(e);
    }

    private static async Task OnScrollViewChangedAsync(ScrollViewerViewChangedEventArgs e)
    {
        // Пока идёт informational SlideUpOut — не уходить в pinned CancelActive (иначе await hide зависает).
        if (_informationalDismissInProgress)
            return;

        await EnsurePinnedMirrorScrollUnblockedAsync().ConfigureAwait(true);
        if (_informationalMirrorActive)
        {
            var shouldDismiss = ShouldDismissMirrorOnScrollEvent(_subscribedScroll, e);
            if (shouldDismiss)
                await DismissInformationalOnActivityAsync(resyncViewport: true).ConfigureAwait(true);
            return;
        }

        if (IsPinnedMirrorOpenForActivity() && ShouldDismissMirrorOnScrollEvent(_subscribedScroll, e))
        {
            await DismissPinnedMirrorOnActivityAsync().ConfigureAwait(true);
            return;
        }

        await SyncMirrorToViewportAsync(_subscribedScroll, animateShow: true, animateHide: true).ConfigureAwait(true);

        if (!e.IsIntermediate)
            ScheduleSettleSync();
    }

    private static async Task EnsureScrollViewportReadyAsync(ScrollViewer? viewport)
    {
        if (viewport is null)
            return;

        for (var i = 0; i < 8; i++)
        {
            if (viewport.IsLoaded && viewport.ViewportHeight > 0)
                return;

            await Task.Delay(16).ConfigureAwait(true);
            viewport.UpdateLayout();
        }
    }

    /// <summary>
    /// Состояние viewport-якоря (под стеком notice на shell или legacy host).
    /// </summary>
    private static NoticeMirrorViewportState GetMirrorViewportState(
        IReadOnlyList<SettingsUnsavedNoticeStrip> openStrips,
        SettingsPageNoticeStack? stack,
        FrameworkElement? legacyAnchor,
        ScrollViewer? scrollViewport)
    {
        var viewport = scrollViewport ?? _subscribedScroll ?? SettingsContentColumnLayout.GetPageContentScroll();
        if (viewport is null || !viewport.IsLoaded || viewport.ViewportHeight <= 0)
            return NoticeMirrorViewportState.LayoutNotReady;

        if (openStrips.Count > 0)
        {
            if (stack is not null)
                return GetElementViewportState(stack.ViewportAnchorElement, viewport);

            if (legacyAnchor is not null)
                return GetElementViewportState(legacyAnchor, viewport);

            return NoticeMirrorViewportState.ElementNotReady;
        }

        if (stack is not null)
            return GetElementViewportState(stack.ViewportAnchorElement, viewport);

        if (legacyAnchor is not null)
            return GetElementViewportState(legacyAnchor, viewport);

        return NoticeMirrorViewportState.Visible;
    }

    /// <summary>
    /// Единая политика pinned viewport mirror: show/hide без sticky-состояния и suppress.
    /// </summary>
    private static PinnedMirrorPolicy EvaluatePinnedMirrorPolicy(
        IReadOnlyList<SettingsUnsavedNoticeStrip> openStrips,
        SettingsPageNoticeStack? stack,
        FrameworkElement? legacyAnchor,
        ScrollViewer? scrollViewport)
    {
        if (openStrips.Count == 0)
            return new PinnedMirrorPolicy { MustHide = true, Reason = "noOpenStrips" };

        if (IsPinnedMirrorDismissedForCurrentEpoch())
            return new PinnedMirrorPolicy { MustHide = true, Reason = "dismissedEpoch" };

        var viewport = scrollViewport ?? _subscribedScroll ?? SettingsContentColumnLayout.GetPageContentScroll();

        // Вход/возврат на страницу — почти всегда scroll≈0: якорь ниже полос, штатное notice видно.
        // Зеркало не активировать «на возврате»; MustShow только после прокрутки (якорь NotVisible).
        if (viewport is not null
            && viewport.IsLoaded
            && viewport.VerticalOffset <= ScrollTopStructuralEpsilon)
        {
            return new PinnedMirrorPolicy { MustHide = true, Reason = "scrollTopNoMirror" };
        }

        if (IsPinnedStripStructurallyOnScreen(openStrips, viewport))
            return new PinnedMirrorPolicy { MustHide = true, Reason = "scrollTopGuard" };

        var anchorState = GetMirrorViewportState(openStrips, stack, legacyAnchor, viewport);
        if (anchorState == NoticeMirrorViewportState.Visible)
            return new PinnedMirrorPolicy { MustHide = true, Reason = "anchorVisible" };

        if (GetAnyOpenStripVisibleInViewport(openStrips, viewport))
            return new PinnedMirrorPolicy { MustHide = true, Reason = "stripVisibleInViewport" };

        if (anchorState == NoticeMirrorViewportState.NotVisible && AreAllOpenStripsMeasurable(openStrips))
            return new PinnedMirrorPolicy { MustShow = true, Reason = "anchorNotVisible" };

        var mirrorOpen = App.ShellWindow?.IsTopNoticeMirrorOpen == true;
        if (mirrorOpen
            && viewport is not null
            && viewport.IsLoaded
            && viewport.VerticalOffset <= ScrollTopStructuralEpsilon)
        {
            NoticeMirrorTrace.Write(
                $"EvaluatePinnedMirrorPolicy: STICKY indeterminate→MustHide scrollTop={viewport.VerticalOffset:F1}, anchor={anchorState}");
            return new PinnedMirrorPolicy { MustHide = true, Reason = "indeterminateScrollTop" };
        }

        if (mirrorOpen
            && anchorState is NoticeMirrorViewportState.LayoutNotReady or NoticeMirrorViewportState.ElementNotReady
            && viewport is not null
            && viewport.IsLoaded
            && viewport.VerticalOffset <= ScrollTopStructuralEpsilon)
        {
            NoticeMirrorTrace.Write(
                $"EvaluatePinnedMirrorPolicy: STICKY layoutNotReady→MustHide scrollTop={viewport.VerticalOffset:F1}");
            return new PinnedMirrorPolicy { MustHide = true, Reason = "indeterminateLayoutTop" };
        }

        NoticeMirrorTrace.Write(
            $"EvaluatePinnedMirrorPolicy: STICKY indeterminate anchor={anchorState}, scrollTop={viewport?.VerticalOffset:F1}, mirrorOpen={mirrorOpen}");
        return new PinnedMirrorPolicy { Reason = "indeterminate" };
    }

    private static bool GetAnyOpenStripVisibleInViewport(
        IReadOnlyList<SettingsUnsavedNoticeStrip> openStrips,
        ScrollViewer? viewport)
    {
        if (viewport is null || !viewport.IsLoaded)
            return false;

        foreach (var strip in openStrips)
        {
            if (!strip.IsVisuallyOpen)
                continue;

            if (GetElementViewportState(strip, viewport) == NoticeMirrorViewportState.Visible)
            {
                NoticeMirrorTrace.Write(
                    $"EvaluatePinnedMirrorPolicy: stripVisibleInViewport {DescribeStrip(strip)}");
                return true;
            }
        }

        return false;
    }

    /// <summary>
    /// Notice stack — первый ребёнок scroll-content; при scroll≈0 открытая полоса structurally on-screen.
    /// </summary>
    private static bool IsPinnedStripStructurallyOnScreen(
        IReadOnlyList<SettingsUnsavedNoticeStrip> openStrips,
        ScrollViewer? scroll)
    {
        if (scroll is null || !scroll.IsLoaded)
            return false;

        if (scroll.VerticalOffset > ScrollTopStructuralEpsilon)
            return false;

        foreach (var strip in openStrips)
        {
            if (strip.IsVisuallyOpen)
                return true;
        }

        return false;
    }

    /// <summary>
    /// Dismiss зеркала по ViewChanged — grace + реальное изменение VerticalOffset (не layout-reposition).
    /// </summary>
    private static bool ShouldDismissMirrorOnScrollEvent(ScrollViewer? scroll, ScrollViewerViewChangedEventArgs e)
    {
        if (scroll is null || e.IsIntermediate)
            return false;

        if (_scrollCompensationDepth > 0)
        {
            NoticeMirrorTrace.Write("MirrorActivityDismiss: skip synthetic scroll compensation");
            return false;
        }

        if (!CanDismissMirrorForActivity())
            return false;

        var offset = scroll.VerticalOffset;
        var delta = Math.Abs(offset - _informationalScrollBaseline);
        if (delta <= ScrollOffsetDismissEpsilon)
        {
            NoticeMirrorTrace.Write(
                $"MirrorActivityDismiss: skip layout ViewChanged (offset={offset:F1}, baseline={_informationalScrollBaseline:F1}, delta={delta:F1})");
            return false;
        }

        _informationalScrollBaseline = offset;
        return true;
    }

    private static void BeginMirrorActivityGrace()
    {
        _informationalActivityAllowedAfterTick = Environment.TickCount64 + InformationalActivityGraceMs;
        NoticeMirrorTrace.Write($"MirrorActivityGrace: {InformationalActivityGraceMs}ms from present");
    }

    private static bool CanDismissMirrorForActivity()
    {
        if (Environment.TickCount64 >= _informationalActivityAllowedAfterTick)
            return true;

        NoticeMirrorTrace.Write(
            $"MirrorActivityDismiss: blocked grace (remaining={_informationalActivityAllowedAfterTick - Environment.TickCount64}ms)");
        return false;
    }

    private static bool IsAnyMirrorOpenForActivity() =>
        _informationalMirrorActive || IsPinnedMirrorOpenForActivity();

    private static bool IsPinnedMirrorOpenForActivity() =>
        !_informationalMirrorActive
        && !_informationalDismissInProgress
        && !_pinnedMirrorHideInProgress
        && (_mirrorActive || App.ShellWindow?.IsTopNoticeMirrorOpen == true);

    private static bool ShouldActivateMirrorForOpen(
        SettingsPageNoticeStack? stack,
        FrameworkElement anchor,
        ScrollViewer? viewport,
        SettingsUnsavedNoticeStrip incomingStrip)
    {
        var openStrips = stack?.GetOpenStrips() ?? Array.Empty<SettingsUnsavedNoticeStrip>();
        if (openStrips.Count > 0 && AreAllOpenStripsMeasurable(openStrips))
        {
            var state = GetMirrorViewportState(openStrips, stack, anchor, viewport);
            return state == NoticeMirrorViewportState.NotVisible;
        }

        if (incomingStrip.IsLoaded && incomingStrip.ActualHeight > 0)
        {
            var stripViewport = viewport ?? _subscribedScroll ?? SettingsContentColumnLayout.GetPageContentScroll();
            if (stripViewport is not null)
            {
                return GetElementViewportState(incomingStrip, stripViewport)
                    == NoticeMirrorViewportState.NotVisible;
            }
        }

        // До первого layout полосы — OpenAnimatedAsync / settle-sync, без ложного mirror-path.
        return false;
    }

    private static bool AreAllOpenStripsMeasurable(IReadOnlyList<SettingsUnsavedNoticeStrip> openStrips)
    {
        if (openStrips.Count == 0)
            return false;

        foreach (var strip in openStrips)
        {
            if (!strip.IsLoaded || strip.ActualHeight <= 0)
                return false;
        }

        return true;
    }

    private static async Task PresentInformationalMirrorAsync(
        SettingsUnsavedNoticeStrip source,
        ScrollViewer? viewport,
        SettingsNoticeSeverity severity,
        string message,
        bool animate,
        SettingsPageNoticeStack? stack)
    {
        // Page-dismiss pinned-зеркала не блокирует новый green MirrorOnly.
        DetachMirrorActivityHandlers();

        if (source.IsVisuallyOpen)
        {
            // Yellow→green off-screen: убрать layout-высоту без прыжка контента.
            await ClosePinnedSourcePreservingScrollAsync(source, animate: false).ConfigureAwait(true);
        }

        source.Visibility = Visibility.Collapsed;

        if (stack is not null)
            BindStackItem(source);
        else
            BindLegacySource(source, source);

        _informationalSource = source;
        _informationalMirrorActive = true;
        _mirrorActive = true;

        // До slide-in/grace: activity-dismiss (600 ms) иначе обгоняет Mark после анимации (1000 ms)
        // и стартовое green не осаждается в footer.
        HomeSessionNoticeState.MarkStartupGreenPresentedIfArmed();

        BeginMirrorActivityGrace();

        var spec = new TopNoticeMirrorStripSpec(
            severity,
            message,
            showSave: false,
            saveEnabled: true,
            source);

        SetShownSignatures(BuildSignature([spec]), [spec]);

        if (App.ShellWindow is { } shell)
            await shell.ShowTopNoticeMirrorStackAsync([spec], animate).ConfigureAwait(true);

        _informationalScrollBaseline = viewport?.VerticalOffset ?? 0;
        AttachScrollHandler(viewport);

        AttachMirrorActivityHandlers();
    }

    private static void AttachMirrorActivityHandlers()
    {
        DetachMirrorActivityHandlers();

        if (App.ShellWindow?.Content is not FrameworkElement root)
            return;

        _windowActivityRoot = root;
        _windowPointerPressedHandler = OnMirrorWindowPointerPressed;
        root.AddHandler(UIElement.PointerPressedEvent, _windowPointerPressedHandler, handledEventsToo: true);

        _windowWheelHandler = OnMirrorScrollWheelChanged;
        root.AddHandler(UIElement.PointerWheelChangedEvent, _windowWheelHandler, handledEventsToo: true);

        if (_subscribedScroll is not null)
        {
            _scrollWheelHandler = OnMirrorScrollWheelChanged;
            _subscribedScroll.AddHandler(UIElement.PointerWheelChangedEvent, _scrollWheelHandler, handledEventsToo: true);
        }
    }

    private static void OnMirrorScrollWheelChanged(object sender, PointerRoutedEventArgs e)
    {
        if (!CanDismissMirrorForActivity() || _informationalDismissInProgress)
            return;

        if (_informationalMirrorActive)
        {
            _ = DismissInformationalOnActivityAsync(resyncViewport: true);
            return;
        }

        if (IsPinnedMirrorOpenForActivity())
            _ = DismissPinnedMirrorOnActivityAsync();
    }

    private static void OnMirrorWindowPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (!CanDismissMirrorForActivity() || _informationalDismissInProgress)
            return;

        if (_informationalMirrorActive)
        {
            _ = DismissInformationalOnActivityAsync(resyncViewport: true);
            return;
        }

        if (IsPinnedMirrorOpenForActivity())
            _ = DismissPinnedMirrorOnActivityAsync();
    }

    private static async Task DismissInformationalOnActivityAsync(bool resyncViewport)
    {
        if (!_informationalMirrorActive)
            return;

        await CompleteInformationalDismissAsync(animateMirror: true).ConfigureAwait(true);

        if (resyncViewport)
            await SyncMirrorToViewportAsync(_subscribedScroll, animateShow: true, animateHide: true).ConfigureAwait(true);
    }

    /// <summary>Pinned зеркало: SlideUpOut по activity; штатные полосы не закрывать. Крестик — HideMirrorImmediate.</summary>
    private static async Task DismissPinnedMirrorOnActivityAsync()
    {
        if (!IsPinnedMirrorOpenForActivity() || _pinnedMirrorHideInProgress)
            return;

        NoticeMirrorTrace.Write("DismissPinnedMirrorOnActivity: dismiss current mirror epoch, strips keep");
        _pinnedMirrorHideInProgress = true;
        DismissPinnedMirrorForCurrentEpoch();
        DetachMirrorActivityHandlers();
        try
        {
            await DeactivateMirrorAsync(animate: true).ConfigureAwait(true);
        }
        finally
        {
            _pinnedMirrorHideInProgress = false;
        }
    }

    private static async Task CompleteInformationalDismissAsync(bool animateMirror)
    {
        if (!_informationalMirrorActive || _informationalDismissInProgress)
            return;

        var source = _informationalSource;

        // Блокируем Sync/pinned activity на время SlideUpOut (иначе CancelActive рвёт await).
        // Deposit/строка футера — только после hide (политика UX).
        _informationalDismissInProgress = true;
        DetachMirrorActivityHandlers();
        _informationalMirrorActive = false;
        _informationalSource = null;
        _informationalActivityAllowedAfterTick = 0;
        _mirrorActive = false;
        ClearShownSignatures();

        try
        {
            if (App.ShellWindow is { } shell)
            {
                if (animateMirror)
                    await shell.HideTopNoticeMirrorAsync(true).ConfigureAwait(true);
                else
                    shell.HideTopNoticeMirrorImmediate();
            }
        }
        finally
        {
            // После скрытия зелёного: retract + осаждение строки в footer (анимация высоты — в HomePage).
            source?.NotifyDismissFromEphemeral();
            HomeSessionNoticeState.DepositStartupGreenToFooterIfPending();
            _informationalDismissInProgress = false;
        }
    }

    private static void DetachMirrorActivityHandlers()
    {
        if (_windowActivityRoot is not null)
        {
            if (_windowPointerPressedHandler is not null)
                _windowActivityRoot.RemoveHandler(UIElement.PointerPressedEvent, _windowPointerPressedHandler);

            if (_windowWheelHandler is not null)
                _windowActivityRoot.RemoveHandler(UIElement.PointerWheelChangedEvent, _windowWheelHandler);
        }

        if (_subscribedScroll is not null && _scrollWheelHandler is not null)
            _subscribedScroll.RemoveHandler(UIElement.PointerWheelChangedEvent, _scrollWheelHandler);

        _windowActivityRoot = null;
        _windowPointerPressedHandler = null;
        _windowWheelHandler = null;
        _scrollWheelHandler = null;
    }

    private static bool HasOpenPinnedStrips()
    {
        if (_activeStack?.GetOpenStrips() is { Count: > 0 } stackOpen)
        {
            foreach (var strip in stackOpen)
            {
                if (strip.IsVisuallyOpen)
                    return true;
            }
        }

        foreach (var binding in _legacyBindings)
        {
            if (binding.Source.IsVisuallyOpen)
                return true;
        }

        return false;
    }

    /// <summary>
    /// Снять informational epoch, если открыта pinned session Warning — scroll sync не должен блокироваться.
    /// Стартовое green на «Обзоре» не снимаем: оно живёт до activity/крестика → footer.
    /// </summary>
    internal static async Task EnsurePinnedMirrorScrollUnblockedAsync()
    {
        if (!_informationalMirrorActive)
            return;

        if (HomeSessionNoticeState.ShouldPreserveStartupGreenInformational)
        {
            NoticeMirrorTrace.Write("EnsurePinnedMirrorScrollUnblocked: skip — Home startup green preserved");
            return;
        }

        var stackOpen = _activeStack?.GetOpenStrips() ?? Array.Empty<SettingsUnsavedNoticeStrip>();
        var legacyOpen = GetOpenLegacyStrips();
        var openStrips = stackOpen.Count > 0 ? stackOpen : legacyOpen;

        SettingsUnsavedNoticeStrip? pinnedWarning = null;
        foreach (var strip in openStrips)
        {
            if (IsPinnedSessionWarningStrip(strip))
            {
                pinnedWarning = strip;
                break;
            }
        }

        if (pinnedWarning is null)
            return;

        // Tip/ExternalSync green на другой полосе стека сосуществует с Session yellow.
        // Cancel только same-strip (иначе FinalizeMirror→Reattach убивает tip).
        if (_informationalSource is not null
            && !ReferenceEquals(_informationalSource, pinnedWarning))
        {
            NoticeMirrorTrace.Write(
                "EnsurePinnedMirrorScrollUnblocked: skip — informational on other strip");
            return;
        }

        NoticeMirrorTrace.Write("EnsurePinnedMirrorScrollUnblocked: yellow open — cancel informational epoch");
        await CancelInformationalForPinnedTransitionAsync(pinnedWarning).ConfigureAwait(true);
    }

    /// <summary>Снять informational epoch при retract Green (FinalizeMirror иначе skip sync).</summary>
    internal static Task EndInformationalEpochIfActiveAsync(SettingsUnsavedNoticeStrip contextStrip) =>
        CancelInformationalForPinnedTransitionAsync(contextStrip);

    private static bool IsPinnedSessionWarningStrip(SettingsUnsavedNoticeStrip strip) =>
        strip.IsVisuallyOpen
        && strip.Severity == SettingsNoticeSeverity.Warning
        && strip.ShowSaveButton;

    /// <summary>
    /// Снять informational-зеркало перед открытием закреплённой полосы; без ephemeral-dismiss на том же strip.
    /// Стартовое Home green не снимаем при open yellow/update.
    /// </summary>
    private static Task CancelInformationalForPinnedTransitionAsync(SettingsUnsavedNoticeStrip incomingSource)
    {
        if (!_informationalMirrorActive)
            return Task.CompletedTask;

        var informationalSource = _informationalSource;
        var sameStrip = ReferenceEquals(informationalSource, incomingSource);

        // Pinned yellow/update на Обзоре не должен «съедать» стартовое green до activity/крестика.
        if (!sameStrip && HomeSessionNoticeState.ShouldPreserveStartupGreenInformational)
        {
            NoticeMirrorTrace.Write(
                $"CancelInformationalForPinnedTransition: preserve Home startup green (incoming={DescribeStrip(incomingSource)})");
            return Task.CompletedTask;
        }

        DetachMirrorActivityHandlers();
        _informationalMirrorActive = false;
        _informationalSource = null;
        _informationalActivityAllowedAfterTick = 0;
        _mirrorActive = false;
        ClearShownSignatures();
        App.ShellWindow?.HideTopNoticeMirrorImmediate();

        if (!sameStrip)
            informationalSource?.NotifyDismissFromEphemeral();
        // sameStrip (Retract): footer только через NotifyDismiss/CompleteDismissGreen — не осаждать здесь.

        return Task.CompletedTask;
    }

    /// <summary>
    /// Компенсировать скролл, только если страница прокручена и полоса (ещё) вне кадра.
    /// </summary>
    private static bool ShouldCompensateScrollForPinnedStrip(
        SettingsUnsavedNoticeStrip source,
        ScrollViewer? scroll)
    {
        if (source.FloatingOverlayChrome)
            return false;

        if (scroll is null || !scroll.IsLoaded)
            return false;

        if (scroll.VerticalOffset <= ScrollTopStructuralEpsilon)
            return false;

        var state = GetElementViewportState(source, scroll);
        return state is NoticeMirrorViewportState.NotVisible or NoticeMirrorViewportState.ElementNotReady;
    }

    private static ScrollLayoutCapture BeginScrollLayoutCapture(ScrollViewer? scroll, bool enabled)
    {
        if (!enabled || scroll is null || !scroll.IsLoaded)
            return default;

        if (scroll.VerticalOffset <= ScrollTopStructuralEpsilon)
            return default;

        return new ScrollLayoutCapture
        {
            Enabled = true,
            Scroll = scroll,
            ExtentBefore = scroll.ExtentHeight,
            OffsetBefore = scroll.VerticalOffset
        };
    }

    private static void CommitScrollLayoutCompensation(in ScrollLayoutCapture capture)
    {
        if (!capture.Enabled || capture.Scroll is null)
            return;

        var scroll = capture.Scroll;
        scroll.UpdateLayout();

        var delta = scroll.ExtentHeight - capture.ExtentBefore;
        if (Math.Abs(delta) < 0.5)
            return;

        var maxOffset = Math.Max(0, scroll.ExtentHeight - scroll.ViewportHeight);
        var newOffset = Math.Clamp(capture.OffsetBefore + delta, 0, maxOffset);

        // Платформа часто уже сдвинула offset на −Δ; ChangeView идемпотентен.
        // Визуальный прыжок гасит suppress RepositionThemeTransition.
        NoticeMirrorTrace.Write(
            $"ScrollCompensate: delta={delta:F1}, offset {capture.OffsetBefore:F1}→{newOffset:F1}, " +
            $"extent {capture.ExtentBefore:F1}→{scroll.ExtentHeight:F1}");

        _scrollCompensationDepth++;
        try
        {
            _informationalScrollBaseline = newOffset;
            scroll.ChangeView(null, newOffset, null, disableAnimation: true);
        }
        finally
        {
            _scrollCompensationDepth--;
        }
    }

    /// <summary>
    /// Снять ChildrenTransitions у предков до ScrollViewer на время layout-мутации notice
    /// (иначе RepositionThemeTransition двигает BodyHost в кадре).
    /// </summary>
    private static void RunWithAncestorChildrenTransitionsSuppressed(FrameworkElement strip, Action action)
    {
        var saved = new List<(Panel Panel, TransitionCollection? Transitions)>(4);
        for (DependencyObject? cur = strip; cur is not null; cur = VisualTreeHelper.GetParent(cur))
        {
            if (cur is ScrollViewer)
                break;

            if (cur is Panel panel && panel.ChildrenTransitions is not null)
            {
                saved.Add((panel, panel.ChildrenTransitions));
                panel.ChildrenTransitions = null;
            }
        }

        try
        {
            action();
        }
        finally
        {
            for (var i = 0; i < saved.Count; i++)
                saved[i].Panel.ChildrenTransitions = saved[i].Transitions;
        }
    }

    private static NoticeMirrorViewportState GetElementViewportState(FrameworkElement element, ScrollViewer? scroll)
    {
        if (scroll is null || !scroll.IsLoaded || scroll.ViewportHeight <= 0)
            return NoticeMirrorViewportState.LayoutNotReady;

        if (!element.IsLoaded || element.ActualHeight <= 0)
            return NoticeMirrorViewportState.ElementNotReady;

        try
        {
            var topLeft = element.TransformToVisual(scroll).TransformPoint(new Point(0, 0));
            var height = Math.Max(element.ActualHeight, 1);
            var top = topLeft.Y;
            var bottom = top + height;
            const double edgeSlack = 2;
            var fullyVisible = top >= -edgeSlack && bottom <= scroll.ViewportHeight + edgeSlack;
            return fullyVisible ? NoticeMirrorViewportState.Visible : NoticeMirrorViewportState.NotVisible;
        }
        catch
        {
            return NoticeMirrorViewportState.ElementNotReady;
        }
    }

    private static void PruneStaleLegacyBindings()
    {
        for (var i = _legacyBindings.Count - 1; i >= 0; i--)
        {
            var source = _legacyBindings[i].Source;
            if (source.IsVisuallyOpen || source.IsLoaded)
                continue;

            NoticeMirrorTrace.Write($"PruneStaleLegacyBindings: {DescribeStrip(source)}");
            RemoveLegacyBinding(source);
        }
    }

    private static string DescribeStrip(SettingsUnsavedNoticeStrip strip) =>
        $"{strip.Severity}|open={strip.IsVisuallyOpen}|vis={strip.Visibility}|h={strip.ActualHeight:F0}|msg={Truncate(strip.Message, 40)}";

    private static string DescribeElement(FrameworkElement? element)
    {
        if (element is null)
            return "null";

        return $"{element.GetType().Name}(loaded={element.IsLoaded}, h={element.ActualHeight:F0})";
    }

    private static string Truncate(string? text, int max)
    {
        if (string.IsNullOrEmpty(text))
            return "";

        return text.Length <= max ? text : text[..max] + "…";
    }
}

using GlassSettings.Core.Customization;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Flyout;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Core.Text;

namespace GlassSettings.Services;

/// <summary>План notify после отката снимка при закрытии окна (снимается до Revert на UI-потоке).</summary>
public sealed class RevertOnExitPlan
{
    public bool NeedsThemeNotify { get; init; }
    public bool NeedsColorizationNotify { get; init; }
    public bool NeedsGlassPlatformBroadcast { get; init; }
    public bool NeedsRootReload { get; init; }
    /// <summary>InkCapsule / GlobalTextGlow отличались от baseline — reload + invalidate Edit после Revert.</summary>
    public bool NeedsTextRenderReload { get; init; }
    /// <summary>Tray / Tray\Xaml dirty — нужен TaskbarCompat reload сразу после Revert (не ждать reopen GS).</summary>
    public bool NeedsTaskbarReload { get; init; }
}

public sealed class SettingsSession
{
    public enum DirtyChangedSource
    {
        Default,
        RegistryWatchSynced
    }

    public static SettingsSession Current { get; } = new();

    private readonly SettingsSnapshot _snapshot = new();
    private readonly HashSet<SettingsCommitScope> _scopeDirtyFlags =
        new(SettingsCommitScopeComparer.Instance);
    private bool _baselineCaptured;

    public event EventHandler? DirtyChanged;

    /// <summary>Последний источник DirtyChanged — для programmatic/external-sync path notice.</summary>
    public DirtyChangedSource LastDirtyChangedSource { get; private set; }

    /// <summary>true после начала закрытия окна — UI notice и DirtyChanged не обновляются.</summary>
    public bool IsShuttingDown { get; private set; }

    public void BeginShutdown()
    {
        IsShuttingDown = true;
        LiveReloadScheduler.CancelPending();
    }

    /// <summary>Сброс после отмены закрытия окна — DirtyChanged и notice снова обновляются.</summary>
    public void CancelShutdown()
    {
        IsShuttingDown = false;
    }

    public bool IsBaselineCaptured => _baselineCaptured;

    internal SettingsSnapshot RegistrySnapshot => _snapshot;

    public void Capture()
    {
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(800));
        _snapshot.Capture();
        _scopeDirtyFlags.Clear();
        _baselineCaptured = true;
        RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
    }

    // Как в temp/GlassSettings: semantic dirty по снимку/флагам. Ложный cold-start yellow
    // закрывается defer до Capture + MarkReady (не гейтом HasUnsavedChanges).
    public bool HasUnsavedChanges
    {
        get
        {
            if (!_baselineCaptured)
                return false;
            if (_scopeDirtyFlags.Count > 0)
                return true;

            return _snapshot.HasUnsavedChanges();
        }
    }

    public bool HasUnsavedInScope(SettingsCommitScope scope)
    {
        if (!_baselineCaptured)
            return false;

        if (scope.Kind == SettingsCommitScopeKind.All)
            return HasUnsavedChanges;

        if (scope.Kind is SettingsCommitScopeKind.System or SettingsCommitScopeKind.Processes)
            return _scopeDirtyFlags.Contains(scope);

        if (_scopeDirtyFlags.Contains(scope))
        {
            if (_snapshot.HasUnsavedInScope(scope))
                return true;

            _scopeDirtyFlags.Remove(scope);
        }

        if (_snapshot.HasUnsavedInScope(scope))
            return true;

        // Area на hub: dirty часто в FlyoutParameterGroup-флагах; страница смотрит Area.
        // Если конкретный flag под этой Area ещё dirty — notice должен открыться.
        if (scope.Kind == SettingsCommitScopeKind.Area && scope.Area is { } area)
        {
            foreach (var flag in _scopeDirtyFlags)
            {
                if (!IsFlagUnderArea(flag, area))
                    continue;
                if (_snapshot.HasUnsavedInScope(flag))
                    return true;
            }
        }

        return false;
    }

    private static bool IsFlagUnderArea(SettingsCommitScope flag, CustomizationAreaId area)
    {
        if (flag.Kind == SettingsCommitScopeKind.Area)
            return flag.Area == area;

        // GlassPlatform — scope page-notice на hub, не гранулярных Area-точек.
        // Иначе любой dirty в Glass подсвечивает все строки Root/Popup/Taskbar/Text.

        if (flag.Kind is SettingsCommitScopeKind.FlyoutParameterGroup or SettingsCommitScopeKind.GlassSubKey)
        {
            var areaPartitions = CommitScopeCatalog.GetGlassPartitions(SettingsCommitScope.ForArea(area));
            var flagPartitions = CommitScopeCatalog.GetGlassPartitions(flag);
            foreach (var p in flagPartitions)
            {
                if (areaPartitions.Contains(p))
                    return true;
            }

            // Flyout group: partition via subkey map
            if (flag.GlassSubKey is { } sub
                && CommitScopeCatalog.GetGlassPartitions(SettingsCommitScope.ForGlassSubKey(sub)) is { Count: > 0 } mapped)
            {
                foreach (var p in mapped)
                {
                    if (areaPartitions.Contains(p))
                        return true;
                }
            }
        }

        return false;
    }

    public void MarkScopeDirty(SettingsCommitScope scope)
    {
        if (scope.Kind == SettingsCommitScopeKind.All)
        {
            MarkDirty();
            return;
        }

        _scopeDirtyFlags.Add(scope);
        RaiseDirtyChanged();
    }

    public void Revert()
    {
        ResetDirtyChangedSource();
        var needsTextRenderReload = TextScopeCatalog.HasTextRenderRegistryDirty(_snapshot);
        _snapshot.Revert();
        _scopeDirtyFlags.Clear();
        ReloadRenderService.NotifyDwmColorizationChanged();
        ReloadRenderService.SignalReloadRender(ReloadRenderFlags.GlassSettingsCommit);
        if (needsTextRenderReload)
            InkCapsuleLiveReload.ReloadEngineAndControls();
        RaiseDirtyChanged();
    }

    /// <summary>Снять dirty-флаги до Revert (на UI-потоке).</summary>
    public RevertOnExitPlan CaptureRevertOnExitPlan()
    {
        var dwmDirtyKeys = _snapshot.GetDirtyDwmValueNames();
        var hadAnyDwmDirty = _snapshot.HasAnyDwmDirty();
        var needsThemeNotify = dwmDirtyKeys.Any(DwmLiveReloadKeys.IsThemeImage);
        var needsColorizationNotify = dwmDirtyKeys.Any(static k => !DwmLiveReloadKeys.IsThemeImage(k));
        if (hadAnyDwmDirty && !needsThemeNotify && !needsColorizationNotify)
            needsColorizationNotify = true;

        var glassDirty = _snapshot.HasAnyGlassDirty();
        return new RevertOnExitPlan
        {
            NeedsThemeNotify = needsThemeNotify,
            NeedsColorizationNotify = needsColorizationNotify,
            NeedsGlassPlatformBroadcast = glassDirty,
            NeedsRootReload = _snapshot.HasRootDirty(),
            NeedsTextRenderReload = TextScopeCatalog.HasTextRenderRegistryDirty(_snapshot),
            // Иначе live-preview Tray\Xaml откатывается в реестре, а панель визуально — только после reopen GS.
            NeedsTaskbarReload = glassDirty,
        };
    }

    /// <summary>Только восстановление реестра (можно с фонового потока).</summary>
    public void RevertSnapshotOnly()
    {
        ShutdownPhaseTrace.Mark("RevertSnapshotBegin");
        _snapshot.Revert();
        _scopeDirtyFlags.Clear();
        ShutdownPhaseTrace.Mark("RevertSnapshotDone");
    }

    /// <summary>Notify OpenGlass / платформа после Revert (на UI-потоке).</summary>
    public void ApplyRevertOnExitNotifications(RevertOnExitPlan plan)
    {
        if (plan.NeedsThemeNotify)
        {
            ReloadRenderService.NotifyThemeChangedWithFallback();
            ShutdownPhaseTrace.Mark("NotifyDwmThemeExit");
        }

        if (plan.NeedsColorizationNotify)
        {
            ReloadRenderService.NotifyDwmColorizationChangedWithFallback();
            ShutdownPhaseTrace.Mark("NotifyDwmColorizationExit");
        }

        if (plan.NeedsGlassPlatformBroadcast)
        {
            ReloadRenderService.NotifyDwmColorizationChangedBroadcast();
            ShutdownPhaseTrace.Mark("NotifyGlassPlatformBroadcast");
        }

        if (plan.NeedsRootReload)
        {
            ReloadRenderService.SignalReloadRender(
                ReloadRenderFlags.RootBackdropReapply | ReloadRenderFlags.GlassCore);
            ShutdownPhaseTrace.Mark("SignalReloadRoot");
            NativeHostInterop.ReapplyOpenRootBackdropsIfEngineLoaded();
            ShutdownPhaseTrace.Mark("ReapplyRoot");
        }

        if (plan.NeedsTextRenderReload)
        {
            InkCapsuleLiveReload.ReloadEngineAndControls();
            ShutdownPhaseTrace.Mark("ReloadTextRenderExit");
        }

        if (plan.NeedsTaskbarReload)
        {
            ReloadRenderService.SignalReloadRender(ReloadRenderFlags.TaskbarBoth);
            ShutdownPhaseTrace.Mark("SignalReloadTaskbarExit");
        }
    }

    /// <summary>
    /// Откат при закрытии окна: DWM → notify OpenGlass (окно Dwm); BWM → HWND_BROADCAST;
    /// Root — SignalReloadRender + reapply backdrop; Text — RenderText после Revert.
    /// </summary>
    public void RevertOnExit()
    {
        var plan = CaptureRevertOnExitPlan();
        RevertSnapshotOnly();
        ApplyRevertOnExitNotifications(plan);
    }

    public void ApplyCommit()
    {
        ResetDirtyChangedSource();
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(800));
        _snapshot.Capture();
        _scopeDirtyFlags.Clear();
        RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
        ReloadRenderService.NotifyDwmColorizationChanged();
        ReloadRenderService.SignalReloadRender(ReloadRenderFlags.GlassSettingsCommit);
        RaiseDirtyChanged();
    }

    /// <summary>
    /// Обновить baseline DWM после запуска платформы (ColorizationColor → COLOR_WINDOW), без reload render.
    /// </summary>
    public void ReloadDwmSnapshotBaseline()
    {
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromSeconds(2));
        ApplyDwmSnapshotBaselineFromRegistry();
        RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
        RaiseDirtyChanged();
    }

    /// <summary>
    /// Ждёт стабилизации HKCU/HKLM\DWM (политика хоста, debounce темы) и переснимает baseline.
    /// </summary>
    public async Task ReloadDwmSnapshotBaselineAsync()
    {
        string? previous = null;
        for (var attempt = 0; attempt < 24; attempt++)
        {
            var fingerprint = _snapshot.ComputeDwmRegistryFingerprint();
            if (previous == fingerprint)
            {
                RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromSeconds(2));
                ApplyDwmSnapshotBaselineFromRegistry();
                RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
                RaiseDirtyChanged();
                return;
            }

            previous = fingerprint;
            await Task.Delay(250).ConfigureAwait(true);
        }

        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromSeconds(2));
        ApplyDwmSnapshotBaselineFromRegistry();
        RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
        RaiseDirtyChanged();
    }

    private void ApplyDwmSnapshotBaselineFromRegistry()
    {
        _snapshot.CaptureScope(SettingsCommitScope.DwmPlatform);
        ClearDwmScopeDirtyFlags();
        ReconcileScopeDirtyFlags();
    }

    /// <summary>Переснимок baseline Root после автополитики OpenGlass (без reload).</summary>
    public void SyncRootBackdropBaseline()
    {
        SyncScopeBaseline(SettingsCommitScope.ForArea(CustomizationAreaId.Root));
        _scopeDirtyFlags.Remove(SettingsCommitScope.GlassPlatform);
        ReconcileScopeDirtyFlags();
        RaiseDirtyChanged();
    }

    /// <summary>Переснимок baseline области без пометки dirty (внешняя политика / после старта подсистемы).</summary>
    public void SyncScopeBaseline(SettingsCommitScope scope)
    {
        if (scope.Kind == SettingsCommitScopeKind.All)
        {
            Capture();
            _scopeDirtyFlags.Clear();
        }
        else
        {
            RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromSeconds(2));
            _snapshot.CaptureScope(scope);
            _scopeDirtyFlags.Remove(scope);
            if (IsDwmRelatedScope(scope))
                RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
        }

        ReconcileScopeDirtyFlags();
        RaiseDirtyChanged();
    }

    /// <summary>Ключ был в baseline (для delete-on-inherit на Adjacent surfaces).</summary>
    public bool HasGlassSnapshotValue(string relativeSubKey, string valueName) =>
        _baselineCaptured && _snapshot.HasGlassSnapshotValue(relativeSubKey, valueName);

    /// <summary>Подтянуть один Glass-ключ в baseline после тихой миграции реестра.</summary>
    public void AbsorbGlassBaseline(string relativeSubKey, string valueName)
    {
        if (!_baselineCaptured)
            return;

        _snapshot.AbsorbGlassParameter(relativeSubKey, valueName);
        ReconcileScopeDirtyFlags();
        RaiseDirtyChanged();
    }

    /// <summary>DWM + Taskbar baseline после старта Glass Platform (без ложных dirty).</summary>
    public async Task ReloadBaselinesAfterPlatformSubsystemStartAsync()
    {
        var (fingerprint, dwmValues) = await WaitForStableDwmBaselineAsync().ConfigureAwait(false);
        var tray = SettingsSnapshot.ReadGlassPartition(GlassRegistryPartition.Tray);
        await RunOnUiAsync(() =>
        {
            var dirtyBefore = _scopeDirtyFlags.Count;
            RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromSeconds(2));
            _snapshot.ReplaceDwmBaseline(dwmValues);
            _snapshot.ReplaceGlassPartition(GlassRegistryPartition.Tray, tray);
            ClearDwmScopeDirtyFlags();
            _scopeDirtyFlags.Remove(SettingsCommitScope.ForArea(CustomizationAreaId.Taskbar));
            ReconcileScopeDirtyFlags();
            RegistrySnapshotWatchService.Current?.AdoptDwmFingerprint(_snapshot, fingerprint);
            var dirtyAfter = _scopeDirtyFlags.Count;
            // Подписчики DirtyChanged обходят дерево и реестр (~300 мс на UI).
            // Если флаги не менялись, индикаторы уже верные — событие не нужно.
            if (dirtyBefore != dirtyAfter)
            {
                App.ShellWindow?.DispatcherQueue.TryEnqueue(
                    Microsoft.UI.Dispatching.DispatcherQueuePriority.Low,
                    RaiseDirtyChanged);
            }
        }).ConfigureAwait(false);
    }

    /// <summary>Отпечаток и значения DWM вне диспетчера: два одинаковых чтения подряд или 24 попытки.</summary>
    private async Task<(string Fingerprint, List<RegEntry> Values)> WaitForStableDwmBaselineAsync()
    {
        string? previous = null;
        List<RegEntry>? previousValues = null;
        for (var attempt = 0; attempt < 24; attempt++)
        {
            var values = _snapshot.ReadDwmValues();
            var fingerprint = SettingsSnapshot.FormatDwmFingerprint(values);
            if (previous == fingerprint && previousValues is not null)
                return (fingerprint, previousValues);

            previous = fingerprint;
            previousValues = values;
            await Task.Delay(250).ConfigureAwait(false);
        }

        return (previous ?? string.Empty, previousValues ?? _snapshot.ReadDwmValues());
    }

    /// <summary>Запись снимка и DirtyChanged — на UI. Без окна применение пропускается.</summary>
    private static Task RunOnUiAsync(Action action)
    {
        var queue = App.ShellWindow?.DispatcherQueue;
        if (queue is null)
            return Task.CompletedTask;

        if (queue.HasThreadAccess)
        {
            action();
            return Task.CompletedTask;
        }

        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!queue.TryEnqueue(() =>
            {
                try
                {
                    action();
                }
                finally
                {
                    done.TrySetResult();
                }
            }))
        {
            done.TrySetResult();
        }

        return done.Task;
    }

    private void ClearDwmScopeDirtyFlags()
    {
        foreach (var scope in _scopeDirtyFlags.Where(IsDwmRelatedScope).ToList())
            _scopeDirtyFlags.Remove(scope);
    }

    private static bool IsDwmRelatedScope(SettingsCommitScope scope) => scope.Kind switch
    {
        SettingsCommitScopeKind.DwmPlatform => true,
        SettingsCommitScopeKind.Area when scope.Area is CustomizationAreaId.DwmColorization
            or CustomizationAreaId.DwmGlass
            or CustomizationAreaId.DwmTheme => true,
        SettingsCommitScopeKind.DwmKeySubset => true,
        _ => false
    };

    public async Task ApplyCommitAsync()
    {
        ResetDirtyChangedSource();
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(800));
        WindowMetricsRegistryWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(800));

        if (TextScopeCatalog.HasSystemFontsRegistryDirty(_snapshot))
            await ApplySystemFontsIfNeededAsync().ConfigureAwait(true);

        _snapshot.Capture();
        await Task.Run(() =>
        {
            ReloadRenderService.NotifyDwmColorizationChanged();
            ReloadRenderService.SignalReloadRender(ReloadRenderFlags.GlassSettingsCommit);
        }).ConfigureAwait(true);
        _scopeDirtyFlags.Clear();
        RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
        WindowMetricsRegistryWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
        RaiseDirtyChanged();
    }

    public async Task ApplyCommitScopeAsync(SettingsCommitScope scope)
    {
        if (scope.Kind == SettingsCommitScopeKind.All)
        {
            await ApplyCommitAsync().ConfigureAwait(true);
            return;
        }

        if (scope.Kind == SettingsCommitScopeKind.Processes)
        {
            Core.Platform.AttachListLiveReload.NotifyAttachListChanged();
            _scopeDirtyFlags.Remove(scope);
            RaiseDirtyChanged();
            return;
        }

        ResetDirtyChangedSource();
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(800));
        WindowMetricsRegistryWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(800));

        if (ScopeTouchesSystemFonts(scope) && TextScopeCatalog.HasSystemFontsRegistryDirty(_snapshot))
            await ApplySystemFontsIfNeededAsync().ConfigureAwait(true);

        // Baseline снимка — синхронно с текущим реестром до live-reload.
        _snapshot.CaptureScope(scope);

        await Task.Run(() =>
        {
            ReloadRenderService.NotifyDwmColorizationChanged();
            ReloadRenderService.SignalReloadRender(ReloadRenderFlags.GlassSettingsCommit);
        }).ConfigureAwait(true);

        _scopeDirtyFlags.Remove(scope);
        ReconcileScopeDirtyFlags();
        if (IsDwmRelatedScope(scope))
            RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
        if (ScopeTouchesSystemFonts(scope))
            WindowMetricsRegistryWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
        RaiseDirtyChanged();
    }

    /// <summary>SPI-применение системных шрифтов под шторой с progress (длительная операция).</summary>
    private async Task ApplySystemFontsIfNeededAsync()
    {
        await WindowBusyOverlayService.RunAsync(async () =>
        {
            await Task.Run(() =>
            {
                var fontsModel = new SystemFontsModel();
                fontsModel.LoadFromRegistryAndTheme();
                SystemFontsApplyService.Apply(fontsModel);
            }).ConfigureAwait(false);
        }, showProgress: true).ConfigureAwait(true);
    }

    private static bool ScopeTouchesSystemFonts(SettingsCommitScope scope) => scope.Kind switch
    {
        SettingsCommitScopeKind.All => true,
        SettingsCommitScopeKind.Area when scope.Area == CustomizationAreaId.Text => true,
        SettingsCommitScopeKind.WindowMetricsKeySubset => true,
        _ => false
    };

    /// <summary>Проба semantic dirty для registry watch — без RaiseDirtyChanged.</summary>
    internal void ApplyRegistryWatchProbe()
    {
        StampDirtyScopesFromSnapshot();
        ReconcileScopeDirtyFlags();
    }

    /// <summary>Read-only: snapshot↔реестр без мутации _scopeDirtyFlags (registry watch settle).</summary>
    internal bool HasRegistryWatchSemanticDirty() =>
        _baselineCaptured && _snapshot.HasUnsavedChanges();

    /// <summary>Детализация semantic-dirty для [RegWatch] (hive, value, snap→reg).</summary>
    internal IReadOnlyList<string> DescribeRegistryWatchSemanticDirty() =>
        _baselineCaptured ? _snapshot.DescribeSemanticDirtyDwmEntries() : Array.Empty<string>();

    /// <summary>Несохранённые DWM-изменения из UI (MarkDirty / MarkScopeDirty), не regedit.</summary>
    internal bool HasUserOriginDwmDirty() =>
        _scopeDirtyFlags.Any(IsDwmRelatedScope);

    /// <summary>External-sync commit блокируется при user-origin DWM dirty.</summary>
    internal bool ShouldBlockExternalRegistrySync() =>
        HasUserOriginDwmDirty();

    internal List<SettingsCommitScope> CollectAffectedDwmScopesForExternalSyncReadOnly()
    {
        var result = new HashSet<SettingsCommitScope>(SettingsCommitScopeComparer.Instance);

        foreach (var area in new[]
                 {
                     CustomizationAreaId.DwmColorization,
                     CustomizationAreaId.DwmGlass,
                     CustomizationAreaId.DwmTheme
                 })
        {
            foreach (var hive in new[] { DwmRegistryHive.Hkcu, DwmRegistryHive.Hklm })
            {
                var scope = SettingsCommitScope.ForDwmHiveArea(area, hive);
                if (_snapshot.HasUnsavedInScope(scope))
                    result.Add(scope);
            }
        }

        var systemScope = DwmSystemScopeCatalog.AllScope(DwmRegistryHive.Hklm);
        if (_snapshot.HasUnsavedInScope(systemScope))
            result.Add(systemScope);

        if (_snapshot.HasUnsavedInScope(SettingsCommitScope.DwmPlatform))
            result.Add(SettingsCommitScope.DwmPlatform);

        return result.ToList();
    }

    internal void ApplyExternalRegistrySyncFromWatch()
    {
        var affectedScopes = CollectAffectedDwmScopesForExternalSyncReadOnly();
        if (affectedScopes.Count == 0)
        {
            RegistryWatchTrace.Write("ApplyExternalRegistrySyncFromWatch: skip — no affected scopes");
            return;
        }

        if (ShouldBlockExternalRegistrySync())
        {
            RegistryWatchTrace.Write("ApplyExternalRegistrySyncFromWatch: skip — user-origin DWM dirty");
            return;
        }

        RegistryWatchTrace.Write(
            $"ApplyExternalRegistrySyncFromWatch: scopes={affectedScopes.Count} [{string.Join(", ", affectedScopes.Select(FormatCommitScopeForWatch))}]");
        RegistryWatchTrace.WriteSemanticDirtyDetails(this, "ApplyExternalRegistrySyncFromWatch");
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromSeconds(2));
        ApplyDwmSnapshotBaselineFromRegistry();
        RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(_snapshot);
        CompositionWallpaperLayersCache.Invalidate();
        PageExternalSyncNoticeState.MarkScopesSynced(affectedScopes);
        LastDirtyChangedSource = DirtyChangedSource.RegistryWatchSynced;
        RaiseDirtyChanged();
    }

    internal void ResetDirtyChangedSource() => LastDirtyChangedSource = DirtyChangedSource.Default;

    public void MarkDirty()
    {
        StampDirtyScopesFromSnapshot();
        ReconcileScopeDirtyFlags();
        RaiseDirtyChanged();
    }

    /// <summary>Помечает области с расхождением snapshot↔реестр — единый паттерн для точек на «Обзор» и nav.</summary>
    private void StampDirtyScopesFromSnapshot()
    {
        if (!_baselineCaptured)
            return;

        foreach (var area in CustomizationCatalog.SessionDirtyAreas)
        {
            if (area is CustomizationAreaId.DwmColorization
                or CustomizationAreaId.DwmGlass
                or CustomizationAreaId.DwmTheme)
            {
                foreach (var hive in new[] { DwmRegistryHive.Hkcu, DwmRegistryHive.Hklm })
                {
                    var scope = SettingsCommitScope.ForDwmHiveArea(area, hive);
                    if (_snapshot.HasUnsavedInScope(scope))
                        _scopeDirtyFlags.Add(scope);
                }
            }
            else
            {
                var scope = SettingsCommitScope.ForArea(area);
                if (_snapshot.HasUnsavedInScope(scope))
                    _scopeDirtyFlags.Add(scope);
            }
        }

        if (_snapshot.HasUnsavedInScope(SettingsCommitScope.GlassPlatform))
            _scopeDirtyFlags.Add(SettingsCommitScope.GlassPlatform);
        if (_snapshot.HasUnsavedInScope(SettingsCommitScope.DwmPlatform))
            _scopeDirtyFlags.Add(SettingsCommitScope.DwmPlatform);
    }

    private void ReconcileScopeDirtyFlags()
    {
        if (_scopeDirtyFlags.Count == 0)
            return;

        foreach (var scope in _scopeDirtyFlags
                     .Where(s => s.Kind == SettingsCommitScopeKind.Area
                                 && s.DwmHive is null
                                 && s.Area is CustomizationAreaId.DwmColorization
                                     or CustomizationAreaId.DwmGlass
                                     or CustomizationAreaId.DwmTheme)
                     .ToList())
            _scopeDirtyFlags.Remove(scope);

        var clean = _scopeDirtyFlags
            .Where(scope => scope.Kind is not (SettingsCommitScopeKind.System or SettingsCommitScopeKind.Processes))
            .Where(scope => !_snapshot.HasUnsavedInScope(scope))
            .ToList();
        foreach (var scope in clean)
            _scopeDirtyFlags.Remove(scope);
    }

    public void PrimeExcludedDwmKeyForRevert(string valueName) =>
        PrimeExcludedDwmKeyForRevert(DwmRegistryHive.Hkcu, valueName);

    public void PrimeExcludedDwmKeyForRevert(DwmRegistryHive hive, string valueName)
    {
        if (!_baselineCaptured || !DwmSnapshotExclusions.IsExcludedDwmValue(valueName))
            return;

        if (_snapshot.ContainsDwmKey(valueName, hive))
            return;

        _snapshot.PrimeDwmKeysFromRegistry(hive, [valueName]);
        MarkScopeDirty(SettingsCommitScope.ForArea(CustomizationAreaId.DwmColorization, hive));
    }

    public void WriteGlassDword(string? subKey, string value, uint data, bool liveFlyout = false)
    {
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(600));
        GlassRegistry.WriteDword(subKey, value, data);
        if (!IsNeverSnapshotSubKey(subKey))
            MarkDirty();
        if (liveFlyout)
            LiveReloadScheduler.ScheduleFlyoutReload();
    }

    public void WriteDwmDword(string valueName, uint data, bool liveDwm = false, bool dwmNotifyImmediate = false) =>
        WriteDwmDword(DwmRegistryHive.Hkcu, valueName, data, liveDwm, dwmNotifyImmediate);

    public void WriteDwmDword(DwmRegistryHive hive, string valueName, uint data, bool liveDwm = false, bool dwmNotifyImmediate = false)
    {
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(600));
        if (DwmSnapshotExclusions.IsExcludedDwmValue(valueName) && !_snapshot.ContainsDwmKey(valueName, hive))
            PrimeExcludedDwmKeyForRevert(hive, valueName);

        Core.Registry.DwmRegistry.WriteDword(hive, valueName, data);
        MarkDirty();
        if (liveDwm)
            NotifyDwmLiveReload(valueName, immediate: dwmNotifyImmediate);
    }

    /// <summary>
    /// Live-preview слайдера: реестр + немедленный DWM notify без MarkDirty (dirty — в конце жеста).
    /// </summary>
    public void WriteDwmDwordLivePreview(string valueName, uint data) =>
        WriteDwmDwordLivePreview(DwmRegistryHive.Hkcu, valueName, data);

    public void WriteDwmDwordLivePreview(DwmRegistryHive hive, string valueName, uint data)
    {
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(600));
        Core.Registry.DwmRegistry.WriteDword(hive, valueName, data);
        NotifyDwmLiveReload(valueName, immediate: true);
    }

    /// <summary>
    /// OpenGlass-семантика: при deleteWhen(value) ключ удаляется, иначе DWORD.
    /// </summary>
    public void WriteDwmDwordOrDelete(
        string valueName,
        uint data,
        Func<uint, bool> deleteWhen,
        bool livePreview = false,
        bool liveDwm = true) =>
        WriteDwmDwordOrDelete(DwmRegistryHive.Hkcu, valueName, data, deleteWhen, livePreview, liveDwm);

    public void WriteDwmDwordOrDelete(
        DwmRegistryHive hive,
        string valueName,
        uint data,
        Func<uint, bool> deleteWhen,
        bool livePreview = false,
        bool liveDwm = true)
    {
        if (deleteWhen(data))
        {
            if (livePreview)
                DeleteDwmValueLivePreview(hive, valueName);
            else
                DeleteDwmValue(hive, valueName, liveDwm);
            return;
        }

        if (livePreview)
            WriteDwmDwordLivePreview(hive, valueName, data);
        else
            WriteDwmDword(hive, valueName, data, liveDwm, dwmNotifyImmediate: true);
    }

    public void DeleteDwmValueLivePreview(string valueName) =>
        DeleteDwmValueLivePreview(DwmRegistryHive.Hkcu, valueName);

    public void DeleteDwmValueLivePreview(DwmRegistryHive hive, string valueName)
    {
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(600));
        Core.Registry.DwmRegistry.DeleteValue(hive, valueName);
        NotifyDwmLiveReload(valueName, immediate: true);
    }

    public void WriteDwmString(string valueName, string data, bool liveDwm = false) =>
        WriteDwmString(DwmRegistryHive.Hkcu, valueName, data, liveDwm);

    public void WriteDwmString(DwmRegistryHive hive, string valueName, string data, bool liveDwm = false)
    {
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(600));
        Core.Registry.DwmRegistry.WriteString(hive, valueName, data);
        MarkDirty();
        if (liveDwm)
            NotifyDwmLiveReload(valueName, immediate: true);
    }

    public void DeleteDwmValue(string valueName, bool liveDwm = false) =>
        DeleteDwmValue(DwmRegistryHive.Hkcu, valueName, liveDwm);

    public void DeleteDwmValue(DwmRegistryHive hive, string valueName, bool liveDwm = false)
    {
        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(600));
        Core.Registry.DwmRegistry.DeleteValue(hive, valueName);
        MarkDirty();
        if (liveDwm)
            NotifyDwmLiveReload(valueName, immediate: true);
    }

    private static void NotifyDwmLiveReload(string valueName, bool immediate)
    {
        if (Core.Platform.DwmLiveReloadKeys.IsThemeImage(valueName))
        {
            Core.Platform.LiveReloadScheduler.NotifyThemeChangedNow();
            return;
        }

        if (immediate)
            Core.Platform.LiveReloadScheduler.NotifyDwmColorizationNow();
        else
            Core.Platform.LiveReloadScheduler.ScheduleDwmColorizationReload();
    }

    public void DeleteGlassValue(string? subKey, string value, bool liveFlyout = false)
    {
        GlassRegistry.DeleteValue(subKey, value);
        if (!IsNeverSnapshotSubKey(subKey))
            MarkDirty();
        if (liveFlyout)
            LiveReloadScheduler.ScheduleFlyoutReload();
    }

    private static bool IsNeverSnapshotSubKey(string? subKey) =>
        RegistryPaths.IsNeverSnapshotRelativeSubKey(subKey);

    private void RaiseDirtyChanged()
    {
        if (IsShuttingDown)
            return;

        DirtyChanged?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>Внешнее изменение DWM — устаревший entry point; используйте ApplyExternalRegistrySyncFromWatch.</summary>
    internal void NotifyDirtyChangedFromRegistryWatch() =>
        ApplyExternalRegistrySyncFromWatch();

    private static string FormatCommitScopeForWatch(SettingsCommitScope scope) =>
        scope.DwmHive is { } hive
            ? $"{scope.Kind}/{scope.Area}+{hive}"
            : $"{scope.Kind}/{scope.Area}";
}

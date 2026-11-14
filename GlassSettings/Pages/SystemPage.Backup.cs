using GlassSettings.Controls;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;

namespace GlassSettings.Pages;

public sealed partial class SystemPage
{
    private const double BackupActionHysteresis = 48;
    private const double BackupCheckGap = 16;

    private enum BackupBodyMode
    {
        TwoColumn,
        OneColumn,
        Stacked
    }

    private BackupBodyMode _createMode = BackupBodyMode.TwoColumn;
    private BackupBodyMode _restoreMode = BackupBodyMode.TwoColumn;
    private bool _createModeReady;
    private bool _restoreModeReady;
    private bool _backupBusy;
    private string? _restorePath;
    private int _backupScrollRetry;
    private string? _pendingBackupScroll;

    public void ApplyBackupStrings()
    {
        BackupExpander.Description = GlassSettingsResources.Get("System_BackupDescription");
        BackupCreateTab.Header = GlassSettingsResources.Get("System_BackupCreateTab");
        BackupRestoreTab.Header = GlassSettingsResources.Get("System_BackupRestoreTab");
        BackupCreatePanel.Description = GlassSettingsResources.Get("System_BackupCreateCall");
        BackupRestorePanel.Description = GlassSettingsResources.Get("System_BackupRestoreCall");
        BackupCreatePanel.SetDefaultActionState(showDefaultAction: false, disableDefaultButton: true);
        BackupRestorePanel.SetDefaultActionState(showDefaultAction: false, disableDefaultButton: true);
        BackupCreateHeading.Text = GlassSettingsResources.Get("System_BackupCreateHeading");
        BackupRestoreHeading.Text = GlassSettingsResources.Get("System_BackupRestoreHeading");
        CreateAppearanceCheck.Content = GlassSettingsResources.Get("System_BackupPartAppearance");
        CreateProcessesCheck.Content = GlassSettingsResources.Get("System_BackupPartProcesses");
        CreateOpenGlassCheck.Content = GlassSettingsResources.Get("System_BackupPartOpenGlass");
        CreateAppCheck.Content = GlassSettingsResources.Get("System_BackupPartApp");
        RestoreAppearanceCheck.Content = GlassSettingsResources.Get("System_BackupPartAppearance");
        RestoreProcessesCheck.Content = GlassSettingsResources.Get("System_BackupPartProcesses");
        RestoreOpenGlassCheck.Content = GlassSettingsResources.Get("System_BackupPartOpenGlass");
        RestoreAppCheck.Content = GlassSettingsResources.Get("System_BackupPartApp");
        CreateBackupButton.Content = GlassSettingsResources.Get("System_BackupSave");
        RestoreSelectButton.Content = GlassSettingsResources.Get("System_BackupSelect");
        RestoreApplyButton.Content = GlassSettingsResources.Get("System_BackupApply");
        UpdateCreateButton();
        UpdateRestoreButton();
    }

    public void WireBackupLayout() =>
        BackupExpander.WireExpandedLayoutRefresh(RefreshBackupLayout);

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        if (SettingsBackupOpenRequest.TryConsume(out var path))
            OpenRestore(path);
    }

    /// <summary>Команда открытия .glassbackup: разворот, вкладка «Восстановить», прокрутка и разбор файла.</summary>
    internal void OpenRestore(string path)
    {
        BackupExpander.IsExpanded = true;
        BackupPivot.SelectedIndex = 1;
        _pendingBackupScroll = "backup";
        _backupScrollRetry = 0;
        ScheduleBackupScroll();
        _ = LoadRestoreFileAsync(path);
    }

    private void ScheduleBackupScroll()
    {
        if (_pendingBackupScroll is null)
            return;
        DispatcherQueue.TryEnqueue(
            Microsoft.UI.Dispatching.DispatcherQueuePriority.Low,
            TryBackupScroll);
    }

    private void TryBackupScroll()
    {
        if (!IsLoaded || _pendingBackupScroll is null)
            return;
        if (!TryScrollToBackup())
        {
            if (_backupScrollRetry++ < 8)
            {
                ScheduleBackupScroll();
                return;
            }

            _pendingBackupScroll = null;
            return;
        }

        _pendingBackupScroll = null;
        _backupScrollRetry = 0;
    }

    private bool TryScrollToBackup()
    {
        if (BackupExpander.ActualHeight <= 0)
            return false;
        var scroll = PageShell.PageScrollViewer;
        if (scroll.Content is not UIElement contentRoot)
            return false;
        double y;
        try
        {
            y = BackupExpander.TransformToVisual(contentRoot).TransformPoint(new Windows.Foundation.Point(0, 0)).Y;
        }
        catch
        {
            return false;
        }

        if (y >= double.MaxValue / 2)
            return false;
        scroll.ChangeView(null, Math.Max(0, y - 12), null, disableAnimation: false);
        return true;
    }

    private void BackupPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (BackupCreatePanel is null || BackupRestorePanel is null)
            return;
        var restore = BackupPivot.SelectedIndex == 1;
        BackupCreatePanel.Visibility = restore ? Visibility.Collapsed : Visibility.Visible;
        BackupRestorePanel.Visibility = restore ? Visibility.Visible : Visibility.Collapsed;
        RefreshBackupLayout();
    }

    private void BackupCreateBody_OnSizeChanged(object sender, SizeChangedEventArgs e) =>
        UpdateBackupBody(
            BackupCreateBody,
            BackupCreateChecks,
            BackupCreateDivider,
            CreateBackupButton,
            BackupCreateHeading,
            CreateAppearanceCheck,
            CreateProcessesCheck,
            CreateOpenGlassCheck,
            CreateAppCheck,
            ref _createMode,
            ref _createModeReady);

    private void BackupRestoreBody_OnSizeChanged(object sender, SizeChangedEventArgs e) =>
        UpdateBackupBody(
            BackupRestoreBody,
            BackupRestoreChecks,
            BackupRestoreDivider,
            BackupRestoreActions,
            BackupRestoreHeading,
            RestoreAppearanceCheck,
            RestoreProcessesCheck,
            RestoreOpenGlassCheck,
            RestoreAppCheck,
            ref _restoreMode,
            ref _restoreModeReady);

    private void RefreshBackupLayout()
    {
        if (BackupCreatePanel is null || BackupRestorePanel is null)
            return;
        BackupCreatePanel.RefreshLayout();
        BackupRestorePanel.RefreshLayout();
        _createModeReady = false;
        _restoreModeReady = false;
        UpdateBackupBody(
            BackupCreateBody, BackupCreateChecks, BackupCreateDivider, CreateBackupButton, BackupCreateHeading,
            CreateAppearanceCheck, CreateProcessesCheck, CreateOpenGlassCheck, CreateAppCheck,
            ref _createMode, ref _createModeReady);
        UpdateBackupBody(
            BackupRestoreBody, BackupRestoreChecks, BackupRestoreDivider, BackupRestoreActions, BackupRestoreHeading,
            RestoreAppearanceCheck, RestoreProcessesCheck, RestoreOpenGlassCheck, RestoreAppCheck,
            ref _restoreMode, ref _restoreModeReady);
    }

    /// <summary>
    /// Широкая ширина — чекбоксы в две колонки и кнопки справа за вертикальным разделителем.
    /// Узкая — одна колонка, тело спойлера выше. Совсем узкая — кнопки под чекбоксами.
    /// </summary>
    private static void UpdateBackupBody(
        Grid host,
        Grid checks,
        Border divider,
        FrameworkElement action,
        TextBlock heading,
        CheckBox first,
        CheckBox second,
        CheckBox third,
        CheckBox fourth,
        ref BackupBodyMode mode,
        ref bool ready)
    {
        var width = host.ActualWidth;
        if (width <= 0 || first is null || action is null)
            return;

        var infinite = new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity);
        heading.Measure(infinite);
        first.Measure(infinite);
        second.Measure(infinite);
        third.Measure(infinite);
        fourth.Measure(infinite);
        action.Measure(infinite);

        var left = Math.Max(first.DesiredSize.Width, third.DesiredSize.Width);
        var right = Math.Max(second.DesiredSize.Width, fourth.DesiredSize.Width);
        var twoWidth = Math.Max(heading.DesiredSize.Width, left + BackupCheckGap + right);
        var oneWidth = Math.Max(
            heading.DesiredSize.Width,
            Math.Max(Math.Max(first.DesiredSize.Width, second.DesiredSize.Width), Math.Max(third.DesiredSize.Width, fourth.DesiredSize.Width)));
        var sideExtras = 1 + 32;
        var twoSide = twoWidth + sideExtras + action.DesiredSize.Width;
        var oneSide = oneWidth + sideExtras + action.DesiredSize.Width;

        var next = ResolveBackupMode(width, twoSide, oneSide, mode, ready);
        if (ready && next == mode)
            return;

        mode = next;
        ready = true;
        var two = next == BackupBodyMode.TwoColumn;
        var stacked = next == BackupBodyMode.Stacked;
        PlaceBackupChecks(heading, first, second, third, fourth, two);
        PlaceBackupChrome(checks, divider, action, stacked);
    }

    private static BackupBodyMode ResolveBackupMode(double width, double twoSide, double oneSide, BackupBodyMode mode, bool ready)
    {
        if (!ready)
        {
            if (width >= twoSide)
                return BackupBodyMode.TwoColumn;
            if (width >= oneSide)
                return BackupBodyMode.OneColumn;
            return BackupBodyMode.Stacked;
        }

        return mode switch
        {
            BackupBodyMode.TwoColumn when width < twoSide =>
                width >= oneSide ? BackupBodyMode.OneColumn : BackupBodyMode.Stacked,
            BackupBodyMode.OneColumn when width >= twoSide + BackupActionHysteresis => BackupBodyMode.TwoColumn,
            BackupBodyMode.OneColumn when width < oneSide => BackupBodyMode.Stacked,
            BackupBodyMode.Stacked when width >= twoSide + BackupActionHysteresis => BackupBodyMode.TwoColumn,
            BackupBodyMode.Stacked when width >= oneSide + BackupActionHysteresis => BackupBodyMode.OneColumn,
            _ => mode
        };
    }

    private static void PlaceBackupChecks(
        TextBlock heading,
        CheckBox first,
        CheckBox second,
        CheckBox third,
        CheckBox fourth,
        bool twoColumns)
    {
        Grid.SetColumnSpan(heading, twoColumns ? 2 : 1);
        if (twoColumns)
        {
            PlaceCheck(first, 0, 1, new Thickness(0));
            PlaceCheck(second, 1, 1, new Thickness(BackupCheckGap, 0, 0, 0));
            PlaceCheck(third, 0, 2, new Thickness(0, 8, 0, 0));
            PlaceCheck(fourth, 1, 2, new Thickness(BackupCheckGap, 8, 0, 0));
            return;
        }

        PlaceCheck(first, 0, 1, new Thickness(0));
        PlaceCheck(second, 0, 2, new Thickness(0, 8, 0, 0));
        PlaceCheck(third, 0, 3, new Thickness(0, 8, 0, 0));
        PlaceCheck(fourth, 0, 4, new Thickness(0, 8, 0, 0));
    }

    private static void PlaceCheck(CheckBox box, int column, int row, Thickness margin)
    {
        Grid.SetColumn(box, column);
        Grid.SetRow(box, row);
        box.Margin = margin;
    }

    private static void PlaceBackupChrome(Grid checks, Border divider, FrameworkElement action, bool stacked)
    {
        if (stacked)
        {
            Grid.SetColumn(checks, 0);
            Grid.SetColumnSpan(checks, 3);
            Grid.SetRow(checks, 0);
            Grid.SetColumn(divider, 0);
            Grid.SetColumnSpan(divider, 3);
            Grid.SetRow(divider, 1);
            divider.Width = double.NaN;
            divider.Height = 1;
            divider.Margin = new Thickness(0, 12, 0, 12);
            divider.HorizontalAlignment = HorizontalAlignment.Stretch;
            divider.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(action, 0);
            Grid.SetColumnSpan(action, 3);
            Grid.SetRow(action, 2);
            action.Margin = new Thickness(0);
            action.HorizontalAlignment = HorizontalAlignment.Right;
            action.VerticalAlignment = VerticalAlignment.Top;
            return;
        }

        Grid.SetColumn(checks, 0);
        Grid.SetColumnSpan(checks, 1);
        Grid.SetRow(checks, 0);
        Grid.SetColumn(divider, 1);
        Grid.SetColumnSpan(divider, 1);
        Grid.SetRow(divider, 0);
        divider.Width = 1;
        divider.Height = double.NaN;
        divider.Margin = new Thickness(16, 0, 16, 0);
        divider.HorizontalAlignment = HorizontalAlignment.Center;
        divider.VerticalAlignment = VerticalAlignment.Stretch;
        Grid.SetColumn(action, 2);
        Grid.SetColumnSpan(action, 1);
        Grid.SetRow(action, 0);
        action.Margin = new Thickness(0);
        action.HorizontalAlignment = HorizontalAlignment.Left;
        action.VerticalAlignment = VerticalAlignment.Center;
    }

    private void CreatePart_OnChanged(object sender, RoutedEventArgs e) => UpdateCreateButton();

    private void RestorePart_OnChanged(object sender, RoutedEventArgs e) => UpdateRestoreButton();

    private void UpdateCreateButton()
    {
        if (CreateBackupButton is null)
            return;
        CreateBackupButton.IsEnabled = !_backupBusy && AnyChecked(
            CreateAppearanceCheck, CreateProcessesCheck, CreateOpenGlassCheck, CreateAppCheck);
    }

    private void UpdateRestoreButton()
    {
        if (RestoreApplyButton is null)
            return;
        RestoreApplyButton.IsEnabled = !_backupBusy
            && _restorePath is not null
            && AnyChecked(RestoreAppearanceCheck, RestoreProcessesCheck, RestoreOpenGlassCheck, RestoreAppCheck);
    }

    private static bool AnyChecked(params CheckBox[] boxes) =>
        boxes.All(static box => box is not null) && boxes.Any(box => box.IsEnabled && box.IsChecked == true);

    private async void CreateBackup_OnClick(object sender, RoutedEventArgs e)
    {
        var sections = SelectedSections(
            CreateAppearanceCheck, CreateProcessesCheck, CreateOpenGlassCheck, CreateAppCheck);
        if (sections.Count == 0 || _backupBusy)
            return;

        _backupBusy = true;
        UpdateCreateButton();
        try
        {
            var path = await SettingsRegistryBackupService.SaveAsync(sections);
            await ShowBackupNoticeAsync(
                SettingsNoticeSeverity.Success,
                GlassSettingsResources.Get("System_BackupSaved") + "\n" + path);
        }
        catch (Exception error)
        {
            await ShowBackupNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Get("System_BackupFailed") + "\n" + error.Message);
        }
        finally
        {
            _backupBusy = false;
            UpdateCreateButton();
        }
    }

    private async void RestoreSelect_OnClick(object sender, RoutedEventArgs e)
    {
        var path = await ExecutableFilePickerDialogService.PickBackupAsync(DispatcherQueue);
        if (!string.IsNullOrWhiteSpace(path))
            await LoadRestoreFileAsync(path);
    }

    private async Task LoadRestoreFileAsync(string path)
    {
        var inspection = await SettingsRegistryBackupService.InspectAsync(path);
        if (!inspection.IsValid)
        {
            _restorePath = null;
            ResetRestoreChecks();
            await ShowBackupNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Get("System_BackupInvalidFile") + "\n" + inspection.Error);
            return;
        }

        _restorePath = path;
        ApplyRestoreAvailability(inspection.Present);
    }

    private void ResetRestoreChecks()
    {
        foreach (var box in RestoreBoxes())
        {
            box.IsChecked = false;
            box.IsEnabled = false;
        }

        UpdateRestoreButton();
    }

    private void ApplyRestoreAvailability(IReadOnlySet<SettingsBackupSection> present)
    {
        SetRestoreBox(RestoreAppearanceCheck, present.Contains(SettingsBackupSection.Appearance));
        SetRestoreBox(RestoreProcessesCheck, present.Contains(SettingsBackupSection.Processes));
        SetRestoreBox(RestoreOpenGlassCheck, present.Contains(SettingsBackupSection.OpenGlass));
        SetRestoreBox(RestoreAppCheck, present.Contains(SettingsBackupSection.App));
        UpdateRestoreButton();
    }

    private static void SetRestoreBox(CheckBox box, bool available)
    {
        box.IsEnabled = available;
        if (!available)
            box.IsChecked = false;
    }

    private async void RestoreApply_OnClick(object sender, RoutedEventArgs e)
    {
        if (_backupBusy || _restorePath is null)
            return;
        var sections = SelectedSections(
            RestoreAppearanceCheck, RestoreProcessesCheck, RestoreOpenGlassCheck, RestoreAppCheck);
        if (sections.Count == 0)
            return;

        _backupBusy = true;
        UpdateRestoreButton();
        try
        {
            var result = await SettingsRegistryBackupService.ApplyAsync(_restorePath, sections);
            ApplyLiveReload(result);
            await ShowBackupNoticeAsync(
                SettingsNoticeSeverity.Success,
                GlassSettingsResources.Get("System_BackupApplied"));
        }
        catch (Exception error)
        {
            await ShowBackupNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Get("System_BackupApplyFailed") + "\n" + error.Message);
        }
        finally
        {
            _backupBusy = false;
            UpdateRestoreButton();
        }
    }

    private static void ApplyLiveReload(SettingsBackupApplyResult result)
    {
        if (result.Applied.Contains(SettingsBackupSection.Appearance))
        {
            _ = ReloadRenderService.SignalReloadRender(
                ReloadRenderFlags.All
                | ReloadRenderFlags.RenderText
                | ReloadRenderFlags.RootLiveBackdropAll
                | ReloadRenderFlags.RootLiveThemePaint
                | ReloadRenderFlags.RootLiveColorizationApplied);
            SettingsSession.Current.SyncScopeBaseline(SettingsCommitScope.GlassPlatform);
        }

        if (result.Applied.Contains(SettingsBackupSection.Processes))
        {
            _ = ReloadRenderService.SignalReloadRender(ReloadRenderFlags.AttachList);
            SettingsSession.Current.SyncScopeBaseline(SettingsCommitScope.Processes);
        }

        if (result.Applied.Contains(SettingsBackupSection.OpenGlass))
        {
            ReloadRenderService.NotifyDwmColorizationChanged();
            if (result.ThemeNotify)
                ReloadRenderService.NotifyThemeChanged();
            SettingsSession.Current.ReloadDwmSnapshotBaseline();
        }

        if (result.LanguageChanged)
        {
            UiLanguageService.ApplyFromRegistry();
            UiLanguageRefreshService.RefreshAll();
        }
    }

    private static List<SettingsBackupSection> SelectedSections(
        CheckBox appearance,
        CheckBox processes,
        CheckBox openGlass,
        CheckBox app)
    {
        var sections = new List<SettingsBackupSection>();
        if (appearance.IsEnabled && appearance.IsChecked == true)
            sections.Add(SettingsBackupSection.Appearance);
        if (processes.IsEnabled && processes.IsChecked == true)
            sections.Add(SettingsBackupSection.Processes);
        if (openGlass.IsEnabled && openGlass.IsChecked == true)
            sections.Add(SettingsBackupSection.OpenGlass);
        if (app.IsEnabled && app.IsChecked == true)
            sections.Add(SettingsBackupSection.App);
        return sections;
    }

    private CheckBox[] RestoreBoxes() =>
    [
        RestoreAppearanceCheck, RestoreProcessesCheck, RestoreOpenGlassCheck, RestoreAppCheck
    ];
}

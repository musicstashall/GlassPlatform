namespace GlassSettings.Services;

using GlassSettings.Strings;
using GlassSettings.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

public static class ReflectionSettingsDialog
{
    // Navigation state belongs to this process, not the generated PNG recipe.
    private static int _selectedGroupIndex;

    public static async Task<ReflectionRenderSettings?> ShowAsync(string source)
    {
        var initial = ReflectionRenderState.Settings;
        var scale = Number("Reflection_SettingScale", 25, 200, initial.ScalePercent);
        var picker = new GlassAlphaColorPicker { IsAlphaVisible = true };
        picker.SetColor(((uint)Math.Round(initial.OpacityPercent * 255.0 / 100) << 24) | initial.ColorRgb);
        var presets = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var darkPreset = new Button { Content = GlassSettingsResources.Get("Reflection_PresetDark") };
        var lightPreset = new Button { Content = GlassSettingsResources.Get("Reflection_PresetLight") };
        presets.Children.Add(darkPreset); presets.Children.Add(lightPreset);
        var softness = Number("Reflection_SettingSoftness", 0, 20, initial.Softness);
        var noise = Number("Reflection_SettingNoise", 0, 100, initial.NoisePercent);
        var noiseDensity = Number("Reflection_SettingNoiseDensity", 0, 100, initial.NoiseDensity);
        var noiseType = new ComboBox { Width = 160, SelectedIndex = -1 };
        foreach (var key in new[] { "None", "Soft", "Film", "Dots", "Blocks", "Sand" })
            noiseType.Items.Add(GlassSettingsResources.Get("Reflection_Noise" + key));
        // UI inserts None first; persisted IDs 0..4 retain their original meaning.
        noiseType.SelectedIndex = initial.NoiseType + 1;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(noiseType,
            GlassSettingsResources.Get("Reflection_SettingNoiseType"));
        var enabled = new ToggleSwitch
        {
            Header = GlassSettingsResources.Get("Reflection_EnableProcessing"), IsOn = initial.Enabled
        };
        using var preview = new ReflectionDesktopPreview();
        var renderGate = new SemaphoreSlim(1, 1);
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };
        var reset = new Button { Content = GlassSettingsResources.Get("Reflection_Reset") };
        var body = new StackPanel { Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        body.Children.Add(new TextBlock
        {
            Text = Path.GetFileNameWithoutExtension(source),
            TextTrimming = TextTrimming.CharacterEllipsis,
            Opacity = 0.7
        });
        body.Children.Add(preview);
        var processingRow = new Grid { ColumnSpacing = 16 };
        processingRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        processingRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        enabled.VerticalAlignment = VerticalAlignment.Center;
        reset.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(reset, 1);
        processingRow.Children.Add(enabled);
        processingRow.Children.Add(reset);
        body.Children.Add(processingRow);
        var imageGroup = new StackPanel { Spacing = 12 };
        imageGroup.Children.Add(Row("Reflection_SettingScale", scale));
        imageGroup.Children.Add(Row("Reflection_SettingSoftness", softness));
        var noiseGroup = new StackPanel { Spacing = 12 };
        noiseGroup.Children.Add(Row("Reflection_SettingNoiseType", noiseType));
        noiseGroup.Children.Add(Row("Reflection_SettingNoise", noise));
        noiseGroup.Children.Add(Row("Reflection_SettingNoiseDensity", noiseDensity));
        var groups = new Pivot { Height = 176 };
        groups.Resources.Add(typeof(PivotHeaderItem), new Style(typeof(PivotHeaderItem))
        {
            BasedOn = (Style)Application.Current.Resources["GlassSettingsPivotHeaderItemStyle"]
        });
        groups.Items.Add(new PivotItem { Header = GlassSettingsResources.Get("Reflection_GroupImage"), Content = imageGroup });
        groups.Items.Add(new PivotItem { Header = GlassSettingsResources.Get("Reflection_GroupNoise"), Content = noiseGroup });
        int restoredGroupIndex = Math.Clamp(_selectedGroupIndex, 0, groups.Items.Count - 1);
        groups.SelectedIndex = restoredGroupIndex;
        bool trackingGroup = false;
        // Pivot may choose its default item when its template is first loaded.
        // Restore after that, and do not filter its routed event by OriginalSource.
        groups.Loaded += (_, _) =>
        {
            groups.SelectedIndex = restoredGroupIndex;
            trackingGroup = true;
        };
        groups.Unloaded += (_, _) => trackingGroup = false;
        groups.SelectionChanged += (_, _) =>
        {
            if (trackingGroup && groups.IsLoaded && groups.SelectedIndex >= 0)
                _selectedGroupIndex = restoredGroupIndex = groups.SelectedIndex;
        };
        groups.IsEnabled = enabled.IsOn;
        body.Children.Add(groups);
        body.Children.Add(error);
        var pickerRail = new StackPanel { Spacing = 12, MaxWidth = 330 };
        pickerRail.Children.Add(new TextBlock
        {
            Text = GlassSettingsResources.Get("Reflection_ColorTitle"),
            Style = (Style)Application.Current.Resources["SubtitleTextBlockStyle"]
        });
        pickerRail.Children.Add(picker); pickerRail.Children.Add(presets);
        var layout = new Grid { ColumnSpacing = 28, RowSpacing = 20 };
        layout.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        layout.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        layout.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        layout.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetColumn(pickerRail, 1);
        layout.Children.Add(body); layout.Children.Add(pickerRail);
        layout.SizeChanged += (_, args) =>
        {
            bool narrow = args.NewSize.Width < 740;
            Grid.SetColumn(pickerRail, narrow ? 0 : 1);
            Grid.SetRow(pickerRail, narrow ? 1 : 0);
            Grid.SetColumnSpan(body, narrow ? 2 : 1);
            Grid.SetColumnSpan(pickerRail, narrow ? 2 : 1);
        };
        int revision = 0;
        bool closed = false;
        ReflectionRenderSettings Values() => new(
            double.IsFinite(scale.Value) ? (int)scale.Value : 100,
            (int)Math.Round((picker.GetColorArgb() >> 24) * 100.0 / 255),
            double.IsFinite(softness.Value) ? (int)softness.Value : 0,
            double.IsFinite(noise.Value) ? (int)noise.Value : 0,
            1, Math.Clamp(noiseType.SelectedIndex - 1, -1, 4),
            double.IsFinite(noiseDensity.Value) ? (int)noiseDensity.Value : 50, enabled.IsOn, picker.GetColorArgb() & 0x00FFFFFFu);
        async Task RefreshAsync()
        {
            int current = ++revision;
            await Task.Delay(180);
            if (closed || current != revision) return;
            await renderGate.WaitAsync();
            try
            {
                if (closed || current != revision) return;
                var png = await ReflectionTextureService.RenderConfiguredAsync(
                    source, preview.DesktopWidth, preview.DesktopHeight, Values());
                if (closed || current != revision) return;
                using var stream = new MemoryStream(png);
                using var random = stream.AsRandomAccessStream();
                var bitmap = new BitmapImage();
                await bitmap.SetSourceAsync(random);
                if (closed || current != revision) return;
                preview.SetReflection(bitmap); error.Text = ""; error.Visibility = Visibility.Collapsed;
            }
            catch (Exception exception)
            {
                if (!closed && current == revision)
                {
                    error.Text = exception.Message;
                    error.Visibility = Visibility.Visible;
                }
            }
            finally { renderGate.Release(); }
        }
        scale.ValueChanged += (_, _) => _ = RefreshAsync();
        picker.ColorChanged += (_, _) => _ = RefreshAsync();
        void ApplyPreset(uint argb)
        {
            picker.SetColor(argb); _ = RefreshAsync();
        }
        darkPreset.Click += (_, _) => ApplyPreset(0xFF000000u);
        lightPreset.Click += (_, _) => ApplyPreset(0xFFA0A0A0u);
        softness.ValueChanged += (_, _) => _ = RefreshAsync();
        noise.ValueChanged += (_, _) => _ = RefreshAsync();
        noiseDensity.ValueChanged += (_, _) => _ = RefreshAsync();
        void SyncNoiseControls()
        {
            bool hasNoise = enabled.IsOn && noiseType.SelectedIndex > 0;
            noise.IsEnabled = hasNoise;
            noiseDensity.IsEnabled = hasNoise;
        }
        noiseType.SelectionChanged += (_, _) => { SyncNoiseControls(); _ = RefreshAsync(); };
        enabled.Toggled += (_, _) =>
        {
            groups.IsEnabled = pickerRail.IsEnabled = enabled.IsOn;
            SyncNoiseControls(); _ = RefreshAsync();
        };
        pickerRail.IsEnabled = enabled.IsOn;
        SyncNoiseControls();
        reset.Click += (_, _) => { enabled.IsOn = true; scale.Value = 100; picker.SetColor(0xFF000000u); softness.Value = 0; noise.Value = 0; noiseDensity.Value = 50; noiseType.SelectedIndex = 0; };
        _ = RefreshAsync();
        try
        {
            var result = await HostedDialogService.ShowAsync(new HostedDialogSpec
            {
                Title = GlassSettingsResources.Get("Reflection_Configure"), Body = HostedDialogLayout.WrapBodyWithContentPadding(layout),
                MinWidth = 780, MaxWidth = 1100, MaxHeight = 760,
                Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(GlassSettingsResources.Get("CmdApply"))
            });
            return result == HostedDialogResult.Primary ? Values().Normalize() : null;
        }
        finally { closed = true; revision++; }
    }

    private static NumberBox Number(string label, double minimum, double maximum, double value)
    {
        var number = new NumberBox
        {
            Style = (Style)Application.Current.Resources["GlassSettingsNumberBoxStyle"],
            Minimum = minimum, Maximum = maximum, Value = value,
            SmallChange = 1, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(number, GlassSettingsResources.Get(label));
        return number;
    }

    private static Grid Row(string label, FrameworkElement number)
    {
        var row = new Grid { ColumnSpacing = 16 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(new TextBlock
        {
            Text = GlassSettingsResources.Get(label), VerticalAlignment = VerticalAlignment.Center,
            TextWrapping = TextWrapping.Wrap
        });
        Grid.SetColumn(number, 1);
        row.Children.Add(number);
        return row;
    }
}

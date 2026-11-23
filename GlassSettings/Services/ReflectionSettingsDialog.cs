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
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: read settings");
        var initial = ReflectionRenderState.Settings;
        var scale = Number("Reflection_SettingScale", 25, 200, initial.ScalePercent);
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: create picker");
        var picker = new GlassAlphaColorPicker { IsAlphaVisible = true };
        picker.SetColor(((uint)Math.Round(initial.OpacityPercent * 255.0 / 100) << 24) | initial.ColorRgb);
        var presets = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var darkPreset = new AccentRecentColorSwatch { SwatchColor = Windows.UI.Color.FromArgb(255, 0, 0, 0) };
        var lightPreset = new AccentRecentColorSwatch { SwatchColor = Windows.UI.Color.FromArgb(255, 160, 160, 160) };
        GlassSettingsToolTipService.SetToolTip(darkPreset, GlassSettingsResources.Get("Reflection_PresetDark"));
        GlassSettingsToolTipService.SetToolTip(lightPreset, GlassSettingsResources.Get("Reflection_PresetLight"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(darkPreset, GlassSettingsResources.Get("Reflection_PresetDark"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(lightPreset, GlassSettingsResources.Get("Reflection_PresetLight"));
        presets.Children.Add(darkPreset); presets.Children.Add(lightPreset);
        var setColor = new Button { Content = GlassSettingsResources.Get("Reflection_SetColor") };
        var colorFlyout = new Flyout { Content = picker, Placement = FlyoutPlacementMode.Top };
        setColor.Flyout = colorFlyout;
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
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: create desktop preview");
        using var preview = new ReflectionDesktopPreview();
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: desktop preview ready");
        var renderGate = new SemaphoreSlim(1, 1);
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };
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
        processingRow.Children.Add(enabled);
        body.Children.Add(processingRow);
        var imageGroup = new StackPanel { Spacing = 12 };
        imageGroup.Children.Add(Row("Reflection_SettingScale", scale));
        imageGroup.Children.Add(Row("Reflection_SettingSoftness", softness));
        var noiseGroup = new StackPanel { Spacing = 12 };
        noiseGroup.Children.Add(Row("Reflection_SettingNoiseType", noiseType));
        noiseGroup.Children.Add(Row("Reflection_SettingNoise", noise));
        noiseGroup.Children.Add(Row("Reflection_SettingNoiseDensity", noiseDensity));
        var groups = new Pivot { Height = 176, VerticalAlignment = VerticalAlignment.Top };
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
        body.Children.Add(error);
        var pickerRail = new StackPanel
        {
            Spacing = 8, Width = 280, VerticalAlignment = VerticalAlignment.Top
        };
        pickerRail.Children.Add(new TextBlock
        {
            Text = GlassSettingsResources.Get("Reflection_ColorGroup"),
            Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"]
        });
        pickerRail.Children.Add(presets);
        pickerRail.Children.Add(setColor);
        // Preview spans the dialog; all editing controls live below it.
        var editors = new Grid { ColumnSpacing = 24 };
        editors.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        editors.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(pickerRail, 1);
        editors.Children.Add(groups); editors.Children.Add(pickerRail);
        var layout = new StackPanel { Spacing = 12 };
        layout.Children.Add(body); layout.Children.Add(editors);
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
                System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] reflection render: begin");
                double rasterScale = preview.XamlRoot?.RasterizationScale ?? 1;
                double factor = Math.Min(1, Math.Min(
                    Math.Max(1, preview.ActualWidth) * rasterScale / preview.DesktopWidth,
                    Math.Max(1, preview.ActualHeight) * rasterScale / preview.DesktopHeight));
                int width = Math.Max(1, (int)Math.Round(preview.DesktopWidth * factor));
                int height = Math.Max(1, (int)Math.Round(preview.DesktopHeight * factor));
                bool texture = ReflectionTextureService.IsTexture(source);
                // Texture cells keep the selected pixel scale in the thumbnail;
                // SVG retains its position/scale in the full virtual desktop.
                var png = await ReflectionTextureService.RenderConfiguredAsync(
                    source, texture ? width : preview.DesktopWidth,
                    texture ? height : preview.DesktopHeight, Values());
                if (closed || current != revision) return;
                System.Diagnostics.Debug.WriteLine($"[ReflectionConfigure] reflection render: complete, bytes={png.Length}");
                using var stream = new MemoryStream(png);
                using var random = stream.AsRandomAccessStream();
                // SVG is filtered down; texture is already rendered at display size.
                var decoder = await Windows.Graphics.Imaging.BitmapDecoder.CreateAsync(random);
                var pixels = await decoder.GetPixelDataAsync(
                    Windows.Graphics.Imaging.BitmapPixelFormat.Bgra8,
                    Windows.Graphics.Imaging.BitmapAlphaMode.Premultiplied,
                    new Windows.Graphics.Imaging.BitmapTransform
                    {
                        ScaledWidth = (uint)width, ScaledHeight = (uint)height,
                        InterpolationMode = Windows.Graphics.Imaging.BitmapInterpolationMode.Fant
                    }, Windows.Graphics.Imaging.ExifOrientationMode.IgnoreExifOrientation,
                    Windows.Graphics.Imaging.ColorManagementMode.DoNotColorManage);
                var bitmap = CompositionWallpaperPipelineBuilder.CreateBitmap(width, height, pixels.DetachPixelData());
                if (closed || current != revision) return;
                preview.SetReflection(bitmap); error.Text = ""; error.Visibility = Visibility.Collapsed;
            }
            catch (Exception exception)
            {
                System.Diagnostics.Debug.WriteLine($"[ReflectionConfigure] reflection render/decode failed: {exception}");
                if (!closed && current == revision)
                {
                    preview.ShowReflectionFailure();
                    error.Text = exception.Message;
                    error.Visibility = Visibility.Visible;
                }
            }
            finally { renderGate.Release(); }
        }
        scale.ValueChanged += (_, _) => _ = RefreshAsync();
        void SyncColorSelection()
        {
            uint argb = picker.GetColorArgb();
            darkPreset.IsSelected = argb == 0xFF000000u;
            lightPreset.IsSelected = argb == 0xFFA0A0A0u;
        }
        picker.ColorChanged += (_, _) => { SyncColorSelection(); _ = RefreshAsync(); };
        void ApplyPreset(uint argb)
        {
            enabled.IsOn = true;
            scale.Value = 100; softness.Value = 0;
            noise.Value = 0; noiseDensity.Value = 50; noiseType.SelectedIndex = 0;
            picker.SetColor(argb); SyncColorSelection(); _ = RefreshAsync();
        }
        darkPreset.Clicked += (_, _) => ApplyPreset(0xFF000000u);
        lightPreset.Clicked += (_, _) => ApplyPreset(0xFFA0A0A0u);
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
        void SyncProcessingControls()
        {
            groups.IsEnabled = enabled.IsOn;
            picker.IsEnabled = enabled.IsOn;
            setColor.IsEnabled = enabled.IsOn;
            SyncNoiseControls();
        }
        enabled.Toggled += (_, _) =>
        {
            SyncProcessingControls(); _ = RefreshAsync();
        };
        SyncProcessingControls();
        SyncColorSelection();
        preview.SizeChanged += (_, _) => _ = RefreshAsync();
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: start reflection render");
        _ = RefreshAsync();
        try
        {
            System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: show hosted dialog");
            var result = await HostedDialogService.ShowAsync(new HostedDialogSpec
            {
                Title = GlassSettingsResources.Get("Reflection_Configure"), Body = HostedDialogLayout.WrapBodyWithContentPadding(layout),
                MinWidth = 700, MaxWidth = 960, MaxHeight = 820,
                Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(GlassSettingsResources.Get("CmdApply"))
            });
            return result == HostedDialogResult.Primary ? Values().Normalize() : null;
        }
        finally { closed = true; revision++; colorFlyout.Hide(); }
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

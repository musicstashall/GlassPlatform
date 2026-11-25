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
        string initialProfile;
        HostedDialogHostContext? dialogContext = null;
        var initial = ReflectionRenderState.Settings;
        var scale = Number("Reflection_SettingScale", 25, 200, initial.ScalePercent);
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: create picker");
        var picker = new GlassAlphaColorPicker { IsAlphaVisible = true };
        picker.SetColor(((uint)Math.Round(initial.OpacityPercent * 255.0 / 100) << 24) | initial.ColorRgb);
        var presets = new Grid { ColumnSpacing = 8, RowSpacing = 8, HorizontalAlignment = HorizontalAlignment.Left };
        var darkPreset = new AccentRecentColorSwatch { SwatchColor = Windows.UI.Color.FromArgb(255, 0, 0, 0) };
        var lightPreset = new AccentRecentColorSwatch { SwatchColor = Windows.UI.Color.FromArgb(255, 160, 160, 160) };
        GlassSettingsToolTipService.SetToolTip(darkPreset, GlassSettingsResources.Get("Reflection_PresetDark"));
        GlassSettingsToolTipService.SetToolTip(lightPreset, GlassSettingsResources.Get("Reflection_PresetLight"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(darkPreset, GlassSettingsResources.Get("Reflection_PresetDark"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(lightPreset, GlassSettingsResources.Get("Reflection_PresetLight"));
        var accentPreset = new AccentRecentColorSwatch { SwatchColor = WindowsPersonalizationService.ReadUserAccentColor() };
        GlassSettingsToolTipService.SetToolTip(accentPreset, GlassSettingsResources.Get("Reflection_PresetAccent"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(accentPreset, GlassSettingsResources.Get("Reflection_PresetAccent"));
        var savedColors = ReflectionColorHistory.Read();
        uint? customColor = ReflectionColorHistory.ReadCustom()
            ?? (savedColors.Count > 0 ? savedColors[0] : picker.GetColorArgb());
        bool IsBuiltInColor(uint color) => color == 0xFF000000u || color == 0xFFA0A0A0u
            || color == ThemeAccentColorHelper.ToArgbDword(accentPreset.SwatchColor);
        if (!IsBuiltInColor(picker.GetColorArgb())) customColor = picker.GetColorArgb();
        var customPreset = new Button
        {
            Width = 48, Height = 48, Padding = new Thickness(0),
            Content = new FontIcon { Glyph = "\uE790", FontSize = 20 },
            HorizontalAlignment = HorizontalAlignment.Left
        };
        GlassSettingsToolTipService.SetToolTip(customPreset, GlassSettingsResources.Get("Reflection_PresetCustom"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(customPreset, GlassSettingsResources.Get("Reflection_PresetCustom"));
        presets.Children.Add(darkPreset); presets.Children.Add(lightPreset); presets.Children.Add(accentPreset);
        presets.Children.Add(customPreset);
        var colorFlyout = new Flyout { Content = picker, Placement = FlyoutPlacementMode.Top };
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
        var preview = new ReflectionDesktopPreview();
        initialProfile = preview.ProfileKey;
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: desktop preview ready");
        var renderGate = new SemaphoreSlim(1, 1);
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };
        var body = new StackPanel { Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        preview.SetSourceLabel(Path.GetFileNameWithoutExtension(source));
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
        var groups = new Pivot { MaxWidth = 420, Height = 176, VerticalAlignment = VerticalAlignment.Top, HorizontalAlignment = HorizontalAlignment.Stretch };
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
            Spacing = 8, Width = 216, VerticalAlignment = VerticalAlignment.Top
        };
        pickerRail.Children.Add(new TextBlock
        {
            Text = GlassSettingsResources.Get("Reflection_ColorGroup"),
            Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"]
        });
        pickerRail.Children.Add(presets);
        var recentSwatches = new List<AccentRecentColorSwatch>();
        var recentValues = ReflectionColorHistory.Read().ToList();
        if (customColor is { } initialCustom && !IsBuiltInColor(initialCustom))
        {
            recentValues.Remove(initialCustom); recentValues.Insert(0, initialCustom);
        }
        // Preview spans the dialog; all editing controls live below it.
        var editors = new Grid { ColumnSpacing = 16, RowSpacing = 16 };
        editors.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        editors.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        editors.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        editors.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(pickerRail, 1);
        editors.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var inlinePicker = new Grid { VerticalAlignment = VerticalAlignment.Top };
        Grid.SetColumn(inlinePicker, 2);
        editors.Children.Add(groups); editors.Children.Add(pickerRail); editors.Children.Add(inlinePicker);
        // Reparent one standard picker; never clone it or alter its template.
        void UpdateEditorLayout(double width)
        {
            picker.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
            double pickerWidth = Math.Max(300, picker.DesiredSize.Width);
            const double parameterBudget = 320, gap = 16, cell = 48, cellGap = 8;
            bool inline = width >= parameterBudget + cell + 2 * gap + pickerWidth;
            if (inline && inlinePicker.Children.Count == 0)
            {
                colorFlyout.Hide(); colorFlyout.Content = null;
                inlinePicker.Children.Add(picker);
            }
            else if (!inline && inlinePicker.Children.Count != 0)
            {
                inlinePicker.Children.Clear(); colorFlyout.Content = picker;
            }
            inlinePicker.Visibility = inline ? Visibility.Visible : Visibility.Collapsed;
            customPreset.Visibility = inline ? Visibility.Collapsed : Visibility.Visible;
            bool stacked = width < 380;
            double paletteBudget = stacked ? width
                : width - parameterBudget - gap - (inline ? pickerWidth + gap : 0);
            int columns = paletteBudget >= 4 * cell + 3 * cellGap ? 4
                : paletteBudget >= 2 * cell + cellGap ? 2 : 1;
            pickerRail.Width = columns * cell + (columns - 1) * cellGap;
            Grid.SetColumn(pickerRail, stacked ? 0 : 1);
            Grid.SetRow(pickerRail, stacked ? 1 : 0);
            groups.MaxWidth = stacked ? Math.Max(1, width) : 420;
            presets.ColumnDefinitions.Clear(); presets.RowDefinitions.Clear();
            for (int i = 0; i < columns; i++)
                presets.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var visiblePresets = presets.Children.OfType<FrameworkElement>()
                .Where(item => item.Visibility == Visibility.Visible).ToArray();
            for (int i = 0; i < (visiblePresets.Length + columns - 1) / columns; i++)
                presets.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            for (int i = 0; i < visiblePresets.Length; i++)
            {
                Grid.SetRow(visiblePresets[i], i / columns);
                Grid.SetColumn(visiblePresets[i], i % columns);
            }

        }
        editors.SizeChanged += (_, args) => UpdateEditorLayout(args.NewSize.Width);
        var layout = new StackPanel { Spacing = 12 };
        layout.Children.Add(body); layout.Children.Add(editors);
        double editorWidth = 0;
        editors.SizeChanged += (_, args) => editorWidth = args.NewSize.Width;
        int revision = 0;
        bool closed = false;
        // A live topology change must not publish a bitmap decoded for the old tree.
        var previewTree = new object();
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
                var treeAtStart = previewTree;
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
                if (closed || current != revision || !ReferenceEquals(treeAtStart, previewTree)) return;
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
                if (closed || current != revision || !ReferenceEquals(treeAtStart, previewTree)) return;
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
            accentPreset.IsSelected = argb == ThemeAccentColorHelper.ToArgbDword(accentPreset.SwatchColor);
            foreach (var swatch in recentSwatches)
                swatch.IsSelected = argb == ThemeAccentColorHelper.ToArgbDword(swatch.SwatchColor, swatch.SwatchColor.A);
        }
        void PopulateRecentColors()
        {
            recentValues.RemoveAll(IsBuiltInColor);
            if (recentValues.Count > 4) recentValues.RemoveRange(4, recentValues.Count - 4);
            // Keep the Flyout anchor parented while its color changes.
            foreach (var swatch in recentSwatches) presets.Children.Remove(swatch);
            recentSwatches.Clear();
            for (int i = 0; i < recentValues.Count; i++)
            {
                uint color = recentValues[i];
                var swatch = new AccentRecentColorSwatch
                {
                    IsEnabled = enabled.IsOn,
                    SwatchColor = Windows.UI.Color.FromArgb((byte)(color >> 24), (byte)(color >> 16), (byte)(color >> 8), (byte)color)
                };
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(swatch, $"#{color:X8}");
                swatch.Clicked += (_, _) =>
                {
                    picker.SetColor(color); UpdatePendingColor(); _ = RefreshAsync();
                };
                presets.Children.Insert(presets.Children.Count - 1, swatch);
                recentSwatches.Add(swatch);
            }
            if (editorWidth > 0) UpdateEditorLayout(editorWidth);
            SyncColorSelection();
        }
        void UpdatePendingColor()
        {
            uint color = picker.GetColorArgb();
            if (!IsBuiltInColor(color)) customColor = color;
            // Editing updates only the pending color and preview, never the history tiles.
            SyncColorSelection();
        }
        picker.ColorChanged += (_, _) => { UpdatePendingColor(); _ = RefreshAsync(); };
        void ApplyPreset(uint argb)
        {
            enabled.IsOn = true;
            scale.Value = 100; softness.Value = 0;
            noise.Value = 0; noiseDensity.Value = 50; noiseType.SelectedIndex = 0;
            picker.SetColor(argb); SyncColorSelection(); _ = RefreshAsync();
        }
        darkPreset.Clicked += (_, _) => ApplyPreset(0xFF000000u);
        lightPreset.Clicked += (_, _) => ApplyPreset(0xFFA0A0A0u);
        accentPreset.Clicked += (_, _) => ApplyPreset(ThemeAccentColorHelper.ToArgbDword(accentPreset.SwatchColor));
        customPreset.Click += (_, _) =>
        {
            enabled.IsOn = true;
            if (customColor is { } color) picker.SetColor(color);
            SyncColorSelection(); _ = RefreshAsync();
            if (inlinePicker.Children.Count != 0) picker.Focus(FocusState.Programmatic);
            else colorFlyout.ShowAt(customPreset);
        };
        var watcher = DesktopEnvironmentWatcher.Current;
        void OnDesktopChanged(object? sender, DesktopEnvironmentChangedEventArgs args)
        {
            if (closed) return;
            if (args.DisplayChanged)
            {
                ++revision; // Retire pending decodes before replacing the old visual tree.
                try
                {
                    var replacement = new ReflectionDesktopPreview();
                    replacement.SetSourceLabel(Path.GetFileNameWithoutExtension(source));
                    var old = preview;
                    if (!body.Children.Contains(old)) throw new InvalidOperationException("Desktop preview left the dialog.");
                    int index = body.Children.IndexOf(old);
                    // Placement must not depend on a ready bitmap: the old preview is
                    // already visible, so the swap happens inside the same layout pass.
                    body.Children.Add(replacement);
                    body.Children.Move((uint)body.Children.IndexOf(replacement), (uint)index);
                    // Move shifts the old preview down; it must leave the tree, or the
                    // dialog shows both the stale and the new monitor layout at once.
                    body.Children.Remove(old);
                    preview = replacement;
                    old.Dispose();
                    previewTree = new object(); // Invalidate decodes still in flight for the old tree.
                    initialProfile = replacement.ProfileKey;
                    dialogContext?.SetPrimaryEnabled(true);
                    error.Visibility = Visibility.Collapsed;
                    replacement.Loaded += (_, _) => _ = RefreshAsync();
                    // Keep the preview tied to the dialog's available width after re-layout.
                    replacement.SizeChanged += (_, _) =>
                    {
                        if (!closed && ReferenceEquals(preview, replacement)) _ = RefreshAsync();
                    };
                    // Preferred shell width follows the new topology; it is never a hard cap.
                    dialogContext?.UpdateMaxWidth(Math.Max(480,
                        GetDialogWidth(replacement.DesktopWidth, replacement.DesktopHeight, replacement.MonitorCount)));
                    _ = RefreshAsync(); // Preserve every pending editor value, including the color.
                }
                catch (Exception exception)
                {
                    dialogContext?.SetPrimaryEnabled(false);
                    error.Text = exception.Message;
                    error.Visibility = Visibility.Visible;
                }
                return;
            }
            if (!args.AccentChanged && !args.ThemeChanged) return;
            accentPreset.SwatchColor = WindowsPersonalizationService.ReadUserAccentColor();
            PopulateRecentColors();
        }
        if (watcher is not null) watcher.Changed += OnDesktopChanged;
        PopulateRecentColors();
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
            foreach (var swatch in recentSwatches) swatch.IsEnabled = enabled.IsOn;
            SyncNoiseControls();
        }
        enabled.Toggled += (_, _) =>
        {
            SyncProcessingControls(); _ = RefreshAsync();
        };
        SyncProcessingControls();
        SyncColorSelection();
        preview.SizeChanged += (_, _) =>
        {
            if (!closed) _ = RefreshAsync();
        };
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: start reflection render");
        _ = RefreshAsync();
        double dialogWidth = GetDialogWidth(preview.DesktopWidth, preview.DesktopHeight, preview.MonitorCount);
        try
        {
            System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] dialog: show hosted dialog");
            var result = await HostedDialogService.ShowAsync(new HostedDialogSpec
            {
                Title = GlassSettingsResources.Get("Reflection_Configure"), Body = HostedDialogLayout.WrapBodyWithContentPadding(layout),
                MinWidth = Math.Min(480, dialogWidth), MaxWidth = dialogWidth, MaxHeight = 820,
                Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(GlassSettingsResources.Get("CmdApply"))
            }, context =>
            {
                dialogContext = context;
                if (ReflectionDisplayProfile.Capture().Key != preview.ProfileKey)
                    OnDesktopChanged(null, new DesktopEnvironmentChangedEventArgs { DisplayChanged = true });
            });
            if (result == HostedDialogResult.Primary
                && ReflectionDisplayProfile.Capture().Key == initialProfile)
            {
                uint appliedColor = picker.GetColorArgb();
                if (!IsBuiltInColor(appliedColor))
                {
                    customColor = appliedColor;
                    recentValues.Remove(appliedColor);
                    recentValues.Insert(0, appliedColor);
                }
                recentValues.RemoveAll(IsBuiltInColor);
                if (recentValues.Count > 4) recentValues.RemoveRange(4, recentValues.Count - 4);
                ReflectionColorHistory.Save(recentValues, customColor);
                return Values().Normalize();
            }
            return null;
        }
        finally
        {
            closed = true; revision++; colorFlyout.Hide();
            if (watcher is not null) watcher.Changed -= OnDesktopChanged; // Never touch a disposed preview.
            preview.Dispose();
        }
    }

    private static double GetDialogWidth(int desktopWidth, int desktopHeight, int monitorCount)
    {
        // Multiple displays start at a smaller preferred scale. Do not cap
        // the Viewbox at that scale: extra dialog width can still grow it to 280 DIP.
        // The hosted shell clamps this preference to available space.
        double preferredHeight = monitorCount > 1 ? 210.0 : 280.0;
        double previewWidth = preferredHeight * desktopWidth / Math.Max(1, desktopHeight);
        return Math.Clamp(previewWidth + 64, 480, 1120);
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

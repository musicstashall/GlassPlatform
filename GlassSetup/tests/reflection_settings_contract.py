"""Portable UI/registry wiring checks; does not execute Windows UI, registry or rendering."""
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
page = (root / 'GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs').read_text()
service = (root / 'GlassSettings/Services/ReflectionTextureService.cs').read_text()
state = (root / 'GlassSettings/Services/ReflectionRenderSettings.cs').read_text()
dialog = (root / 'GlassSettings/Services/ReflectionSettingsDialog.cs').read_text()
assert 'Software\\StanislavKa\\GlassPlatform\\GlassSettings\\Textured' in state
assert 'RegistryHive.CurrentUser' in state and 'RegistryValueKind.String' in state
assert 'key.SetValue("State"' in state and 'SHA256.HashData(image)' in state
assert service.index('ReflectionRenderState.Invalidate(profile.Key);') < service.index('var output = Publish(target);')
assert service.index('var output = Publish(target);') < service.index('ReflectionRenderState.Save(')
assert 'var directory = Path.GetFullPath(PlatformPaths.ReflectionAppliedDirectory);' in service
assert 'if (!IsTexture(source) && !isSvg) return source;' in service
assert 'settings with { NoisePercent = 0, OpacityPercent = 100 }, wrap: true)' in service
assert 'ReflectionPixelEffects.ApplyAlpha(repeated, width, height,' in service
assert 'ReflectionPixelEffects.ApplyAlpha(pixels, width, height, settings, wrap: false)' in service
assert 'if (category != 1) return items;' in page
assert '.Where(item => ReflectionCategory(item.FilePath) == category)' in page
assert 'ReflectionSettingsDialog.ShowAsync(source)' in page
assert 'if (settings is not null) await ApplyReflectionSourceAsync(source, settings, profile)' in page
assert 'ReflectionRenderState.Save' not in dialog and 'WriteDwm' not in dialog
assert 'source, texture ? width : preview.DesktopWidth,' in dialog
assert 'texture ? height : preview.DesktopHeight, Values()' in dialog
xaml = ET.parse(root / 'GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml')
ns = {'x': 'http://schemas.microsoft.com/winfx/2006/xaml', 'ui': 'http://schemas.microsoft.com/winfx/2006/xaml/presentation'}
pivot = xaml.find('.//ui:Pivot[@x:Name="ReflectionCategoryPivot"]', ns)
assert pivot is not None and len(pivot.findall('ui:PivotItem', ns)) == 3
for language in ('ru-RU', 'en-US'):
    resources = ET.parse(root / f'GlassSettings/Strings/{language}/Resources.resw')
    for key in ('Reflection_TabTextures', 'Reflection_TabPattern', 'Reflection_TabOther',
                'Reflection_Configure', 'Reflection_SettingScale', 'Reflection_SettingOpacity',
                'Reflection_SettingSoftness', 'Reflection_Reset'):
        assert resources.find(f"data[@name='{key}']/value").text
print('PASS: reflection Pivot, default placement, passthrough, settings dialog, single output and HKCU publication order')

# Header actions and directly populated header-table cells must not be mixed:
# the expander's legacy bridge would discard the slider on Loaded.
assert 'ReflectionCollectionExpander.HeaderActions' not in page
assert 'HeaderControlsTable.SetCell(0, null, _reflectionConfigureButton)' in page
assert 'ReflectionCollectionExpander.HeaderControlsTable.SetCell(\n            1,' in page
assert 'ReflectionCollectionExpander.HeaderControlsTable.GetCell(1)!.Label' in page
header_style = pivot.find('ui:Pivot.Resources/ui:Style', ns)
assert header_style is not None
assert header_style.get('BasedOn') == '{StaticResource GlassSettingsPivotHeaderItemStyle}'
print('PASS: configure precedes intensity in one header table; Pivot reuses Adjacent Windows typography')
assert 'int NoisePercent = 0' in state and 'Math.Clamp(NoisePercent, 0, 100)' in state
assert 'noise.ValueChanged' in dialog and 'initial.NoisePercent' in dialog
assert 'MaxHeight = 820' in dialog and 'HostedDialogSpec' in dialog
assert 'CreateCancelConfirmFooter(GlassSettingsResources.Get("CmdApply"))' in dialog
for language in ('ru-RU', 'en-US'):
    assert 'Reflection_SettingNoise' in (root / f'GlassSettings/Strings/{language}/Resources.resw').read_text()
print('PASS: noise control persisted with settings; bounded scrollable dialog retains Apply footer')
assert 'int NoiseDensity = 50' in state and 'Math.Clamp(NoiseDensity, 0, 100)' in state
assert 'initial.NoiseDensity' in dialog and 'noiseDensity.ValueChanged' in dialog
assert 'noiseDensity.Value = 50' in dialog and 'noiseSize' not in dialog
for language in ('ru-RU', 'en-US'):
    assert 'Reflection_SettingNoiseDensity' in (root / f'GlassSettings/Strings/{language}/Resources.resw').read_text()
print('PASS: grain density control, defaults, bounds, preview and reset wiring')
assert 'int NoiseType = -1' in state and 'Math.Clamp(NoiseType, -1, 4)' in state
assert 'noiseType.SelectionChanged' in dialog and 'initial.NoiseType' in dialog
assert 'noiseType.SelectedIndex = 0' in dialog
for language in ('ru-RU', 'en-US'):
    strings = (root / f'GlassSettings/Strings/{language}/Resources.resw').read_text()
    for key in ('Reflection_SettingNoiseType', 'Reflection_NoiseSoft', 'Reflection_NoiseFilm',
                'Reflection_NoiseDots', 'Reflection_NoiseBlocks'):
        assert key in strings
print('PASS: four grain types, default smooth noise, persistence and preview wiring')
effects = (root / 'GlassSettings/Services/ReflectionPixelEffects.cs').read_text()
assert 'double luminance = 127.5 * (1 + grain)' in effects
assert 'Math.Abs(grain) * settings.NoisePercent' in effects
assert 'Math.Sqrt(-2 * Math.Log(u))' in effects
assert 'blue / (double)weight' not in effects and 'weight += a' not in effects
print('PASS: bipolar luminance grain independent of source RGB, Gaussian film samples')
assert 'bool Enabled = true' in state
assert 'if (!settings.Enabled) settings = new ReflectionRenderSettings();' in service
for control in ('groups', 'picker'):
    assert f'{control}.IsEnabled = enabled.IsOn;' in dialog
assert 'pickerRail.IsEnabled' not in dialog
assert 'void SyncProcessingControls()' in dialog
assert dialog.count('SyncProcessingControls();') == 2
assert 'new Pivot' in dialog and 'Reflection_GroupImage' in dialog and 'Reflection_GroupNoise' in dialog
assert 'GlassSettingsPivotHeaderItemStyle' in dialog
desktop_preview = (root / 'GlassSettings/Controls/ReflectionDesktopPreview.cs').read_text()
assert 'CornerRadius' in desktop_preview and 'ControlStrongStrokeColorDefaultBrush' in desktop_preview
assert 'DesktopWallpaperImageCache.GetDisplaySourceOrLoad' in desktop_preview
assert 'CalibrateNoise' in effects and 'target / value' in effects
print('PASS: calibrated intensity, persistent bypass, grouped Pivot and wallpaper desktop preview wiring')
assert 'private static int _selectedGroupIndex' in dialog
assert 'Math.Clamp(_selectedGroupIndex' in dialog and 'groups.Loaded +=' in dialog
assert '_selectedGroupIndex = restoredGroupIndex = groups.SelectedIndex' in dialog
assert 'trackingGroup && groups.IsLoaded' in dialog
assert 'Margin = new Thickness(0, 12, 0, 0)' in dialog
assert 'Checkerboard' not in desktop_preview and 'preview.Opacity' not in dialog
print('PASS: process-only Pivot selection, shared wallpaper without image dimming, top content inset')
assert 'groups.Loaded +=' in dialog and 'groups.Unloaded +=' in dialog
assert 'pickerRail.Children.Add(presets)' in dialog and 'var reset =' not in dialog
assert 'One full-desktop reflection bitmap' in desktop_preview
assert 'Shadow = new ThemeShadow()' in desktop_preview
assert 'Reflection_NoiseSand' in (root / 'GlassSettings/Strings/ru-RU/Resources.resw').read_text()
assert 'if (type == 4)' in effects and 'u < 1311' in effects and 'u < 64004' in effects
print('PASS: Pivot load restoration, top-right theme presets, preview shadow and independent-pixel sand grain')
assert '"None", "Soft", "Film", "Dots", "Blocks", "Sand"' in dialog
assert 'noiseType.SelectedIndex = initial.NoiseType + 1' in dialog
assert 'Math.Clamp(noiseType.SelectedIndex - 1, -1, 4)' in dialog
assert 'noise.IsEnabled = hasNoise' in dialog and 'noiseDensity.IsEnabled = hasNoise' in dialog
assert 'settings.NoiseType < 0 ||' in effects
print('PASS: None bypasses only noise, locks noise controls, preserves previous type IDs')
assert 'Math.Clamp(Softness, 0, 20)' in state
assert 'Number("Reflection_SettingSoftness", 0, 20, initial.Softness)' in dialog
print('PASS: reflection softness supports 0–20 px in both UI and persisted settings')
number_factory = dialog[dialog.index('private static NumberBox Number'):dialog.index('private static Grid Row')]
assert 'GlassSettingsNumberBoxStyle' in number_factory
assert 'NumberBoxSpinButtonPlacementMode.Inline' in number_factory
assert 'Width = 128' not in number_factory and 'NumberBoxSpinButtonPlacementMode.Compact' not in number_factory
print('PASS: reflection numeric inputs reuse popup Animation NumberBox style and inline steppers')

assert 'GlassAlphaColorPicker' in dialog and 'IsAlphaVisible = true' in dialog
assert 'ColorRgb' in state and 'Colorize(pixels, settings.ColorRgb)' in service
assert 'Reflection_PresetDark' in dialog and 'Reflection_PresetLight' in dialog
assert 'MaxWidth = dialogWidth' in dialog and 'Grid.SetColumn(pickerRail, 1)' in dialog
assert 'ReflectionDisplayProfile.Capture()' in desktop_preview
assert 'profile.Monitors.Select' in desktop_preview
assert 'WallpaperPreviewCrossfade' in desktop_preview
assert '_watcher.Changed += OnEnvironmentChanged' in desktop_preview
assert '_watcher.Changed -= OnEnvironmentChanged' in desktop_preview
assert 'new DesktopEnvironmentWatcher' not in desktop_preview
print('PASS: unified source tint/alpha picker, theme presets, proportional monitor layout, shared wallpaper lifecycle')
flat = (root / 'GlassSettings/Controls/FlatGlassPreviewLayer.cs').read_text()
shared_state = (root / 'GlassSettings/Services/CompositionPreviewRegistryStates.cs').read_text()
stylization = (root / 'GlassSettings/Controls/StylizationLayeredPreview.xaml.cs').read_text()
assert desktop_preview.index('layers.Children.Add(a)') < desktop_preview.index('layers.Children.Add(glass)')
assert 'layers.Children.Add(glass);' in desktop_preview
assert 'var reflection = new Image { Width = DesktopWidth, Height = DesktopHeight, Stretch = Stretch.Fill }' in desktop_preview
assert 'reflectionViewport.Children.Add(reflection);' in desktop_preview
assert 'layers.Children.Add(reflectionViewport);' in desktop_preview
assert 'desktop.Children.Add(layers);' in desktop_preview
assert 'CompositionWallpaperPipelineBuilder.Build(' in flat
assert 'CompositionPreviewRegistryStates.BuildRuntimeActive()' in flat and 'CompositionPreviewRegistryStates.BuildRuntimeActive()' in stylization
assert 'CompositionWallpaperPipelineBuilder.CreateBitmap(' in flat and 'CompositionWallpaperPipelineBuilder.CreateBitmap(' in stylization
assert 'BuildGaussian' not in flat and 'File.ReadAllBytes' not in flat
assert 'RuntimeReflectionOpacity' in desktop_preview and 'RuntimeReflectionOpacity' in stylization
assert 'Translation' not in flat and 'Margin' not in flat
cache = (root / 'GlassSettings/Services/DesktopWallpaperImageCache.cs').read_text()
assert 'GetMonitorDisplaySourcesAsync' in cache and 'GetMonitorDisplaySourcesAsync' in desktop_preview
assert 'MonitorSources.Clear()' in cache
print('PASS: flat wallpaper/glass/reflection order, shared Stylization glass policy/compositor and per-monitor wallpaper cache')

assert 'Directory.CreateTempSubdirectory("GlassPlatform-Reflection-")' in service
assert 'Directory.Delete(stagingDirectory, recursive: true)' in service
assert 'Path.Combine(PlatformPaths.ReflectionCacheDirectory, "Staging")' not in service
print('PASS: unique user TEMP staging directory cleaned in finally, no new LocalAppData reflection cache')

paths = (root / 'GlassSettings/Core/Platform/PlatformPaths.cs').read_text()
assert 'ReflectionCacheDirectory' not in paths + service
assert 'ReflectionPatternAppliedDirectory' not in paths + service
assert 'ReflectionSourceNaming' not in service
assert '_Lite' not in state and 'uint ColorRgb = 0' in state
assert 'state.Source' in service and 'state.Output' in service
print('PASS: only current publication resolves its explicit source; no legacy paths, filename aliases or color migration')

assert 'var displays = DisplayArea.FindAll();' not in desktop_preview
assert 'new Windows.Graphics.RectInt32(m.X, m.Y, m.Width, m.Height)' in desktop_preview
native_profile = (root / 'GlassSettings/Native/ReflectionProfilesNative.cpp').read_text()
assert 'EnumDisplayMonitors(nullptr, nullptr, Enumerate' in native_profile
assert 'rect->right - rect->left, rect->bottom - rect->top' in native_profile
print('PASS: monitor snapshot uses native full monitor rectangles, without WinRT DisplayArea calls')

assert 'layers.Clip = new RectangleGeometry' in desktop_preview
assert 'CreateGeometricClip(' not in desktop_preview
assert 'GetElementVisual(layers)' not in desktop_preview
assert 'layers.Loaded +=' not in desktop_preview
print('PASS: XAML owns monitor clipping; no geometric Composition clip on its visual')

assert 'layout.Children.Add(body); layout.Children.Add(editors);' in dialog
assert 'editors.Children.Add(groups); editors.Children.Add(pickerRail);' in dialog
assert 'pickerRail.Children.Add(picker)' not in dialog
assert 'BitmapInterpolationMode.Fant' in dialog
assert 'preview.DesktopWidth * factor' in dialog and 'RasterizationScale' in dialog
assert 'preview.SizeChanged +=' in dialog
assert 'scale.Value = 100; softness.Value = 0;' in dialog
assert 'darkPreset.IsEnabled = enabled.IsOn' not in dialog
print('PASS: compact preview-above-editors layout, full recipe presets and filtered desktop-scale thumbnail')

assert 'darkPreset.Clicked += (_, _) => ApplyPreset(0xFF000000u);' in dialog
assert 'lightPreset.Clicked += (_, _) => ApplyPreset(0xFFA0A0A0u);' in dialog
assert 'Height = 176, VerticalAlignment = VerticalAlignment.Top' in dialog
assert 'UseCompactLayout' not in dialog
picker_code = (root / 'GlassSettings/Controls/GlassAlphaColorPicker.xaml.cs').read_text()
assert 'ResizeSpectrum' not in picker_code
assert 'new AccentRecentColorSwatch' in dialog
assert 'darkPreset.IsSelected = argb == 0xFF000000u' in dialog
assert 'lightPreset.IsSelected = argb == 0xFFA0A0A0u' in dialog
assert 'new Flyout { Content = picker' in dialog and 'colorFlyout.ShowAt(customPreset)' in dialog
assert 'setColor' not in dialog
assert '_wallpaperReady || !_reflectionReady' in desktop_preview
assert '_desktopView.Opacity = 1' in desktop_preview and 'Opacity = 0' in desktop_preview
print('PASS: shared checked swatches, standard flyout picker, pixel-scale texture vs desktop-scale SVG, loading readiness gate')

assert 'GetDialogWidth(preview.DesktopWidth, preview.DesktopHeight, preview.MonitorCount)' in dialog
assert 'Math.Clamp(previewWidth + 64, 480, 1120)' in dialog
assert 'bool inline = width >= parameterBudget + cell + 2 * gap + pickerWidth' in dialog
assert 'colorFlyout.Content = null' in dialog and 'colorFlyout.Content = picker' in dialog
assert 'inlinePicker.Children.Add(picker)' in dialog and 'inlinePicker.Children.Clear()' in dialog
assert 'ReadUserAccentColor()' in dialog and 'accentPreset.Clicked +=' in dialog
assert 'watcher.Changed += OnDesktopChanged' in dialog and 'watcher.Changed -= OnDesktopChanged' in dialog
assert 'ReflectionColorHistory.Read()' in dialog and 'ReflectionColorHistory.Save' in dialog
assert 'preview.SetSourceLabel(' in dialog and 'body.Children.Add(new TextBlock' not in dialog
assert '_sourceBadge' in desktop_preview and 'CornerRadius = new CornerRadius(6)' in desktop_preview
history = (root / 'GlassSettings/Services/ReflectionColorHistory.cs').read_text()
assert '.Distinct().Take(4)' in history and 'RegistryValueKind.MultiString' in history
print('PASS: monitor-bound dialog width, bounded fields, adaptive inline/flyout picker, live accent, bounded recent colors and preview badge')

assert 'presets.Children.Add(customPreset)' in dialog
assert 'customPreset.Click +=' in dialog
assert 'recentValues.RemoveAll(IsBuiltInColor)' in dialog
assert 'ReadRecentAccentColors()' not in dialog
assert 'customColor = color;' in dialog and 'recentValues.Insert(0, appliedColor)' in dialog
assert 'ReflectionColorHistory.Save(recentValues, customColor)' in dialog
assert 'ReadCustom()' in history and '"CustomColor"' in history
print('PASS: fourth custom preset, previous custom enters history, all preset duplicates excluded, no imported Windows colors')

assert 'Themes_Colors_RecentColors' not in dialog
assert 'monitorCount > 1 ? 210.0 : 280.0' in dialog
assert 'preferredHeight * desktopWidth / Math.Max(1, desktopHeight)' in dialog
assert 'MonitorFromWindow' not in dialog and 'XamlRoot?.Size.Width' not in dialog
assert 'Grid.SetRow(pickerRail, stacked ? 1 : 0)' in dialog
print('PASS: preview-aspect dialog width, vertical narrow palettes/history, stacked fallback and no detached history heading')

assert 'customPreset.IsEnabled = customColor.HasValue' not in dialog
assert 'HorizontalAlignment = HorizontalAlignment.Left' in dialog
assert 'savedColors.Count > 0 ? savedColors[0] : picker.GetColorArgb()' in dialog
print('PASS: always-visible custom picker launcher, no redundant button, left-aligned compact swatch column')

assert 'var presets = new Grid' in dialog
assert 'paletteBudget >= 4 * cell + 3 * cellGap ? 4' in dialog
assert 'paletteBudget >= 2 * cell + cellGap ? 2 : 1' in dialog
assert 'Grid.SetColumn(visiblePresets[i], i % columns)' in dialog
assert 'double pickerWidth = Math.Max(300, picker.DesiredSize.Width)' in dialog
assert 'Width = new GridLength(1, GridUnitType.Star)' in dialog
assert 'MaxWidth = 420, Height = 176' in dialog
print('PASS: available-width 1/2/4 swatch grid, shared history columns, measured standard picker budget and flexible parameter column')

assert 'var customPreset = new Button' in dialog and 'new FontIcon' in dialog
assert 'presets.Children.Insert(presets.Children.Count - 1, swatch)' in dialog
assert 'presets.Children.Clear()' not in dialog
assert 'customPreset.SwatchColor' not in dialog
assert 'recentValues.Count > 4' in dialog and '.Take(8)' not in history
print('PASS: neutral icon picker launcher stays last in unified grid, max four custom history colors, stable flyout anchor')

pending_color = dialog[dialog.index('void UpdatePendingColor()'):dialog.index('void ApplyPreset')]
assert 'recentValues.' not in pending_color and 'PopulateRecentColors()' not in pending_color
assert 'UpdatePendingColor(); _ = RefreshAsync();' in pending_color
apply_block = dialog[dialog.index('if (result == HostedDialogResult.Primary'):dialog.index('return null;')]
assert 'recentValues.Insert(0, appliedColor)' in apply_block
assert 'ReflectionColorHistory.Save(recentValues, customColor)' in apply_block
assert dialog.count('recentValues.Insert(0, appliedColor)') == 1
assert 'RememberLocalColor' not in dialog
print('PASS: picker edits do not mutate/rebuild history; only explicit Apply records the final selected color')

assert 'SettingsSession.Current.DirtyChanged += OnSessionChanged' in desktop_preview
assert 'SettingsSession.Current.DirtyChanged -= OnSessionChanged' in desktop_preview
live_glass = desktop_preview[desktop_preview.index('private async void OnSessionChanged'):desktop_preview.index('private async Task RefreshWallpaperAsync')]
assert 'await Task.Delay(150)' in live_glass
assert '_lastGlassState == state' in live_glass and '_glassRefreshGate.WaitAsync()' in live_glass
assert '_glassLayers[i].RefreshAsync(_glassWallpaperPaths[i] ?? state.WallpaperPath, true)' in live_glass
assert 'SetReflection' not in live_glass and 'ReadMonitorWallpapers' not in live_glass
assert 'new RegistryValueChangeWatcher' not in desktop_preview
print('PASS: live glass reuses session registry notifications, debounces/deduplicates and preserves reflection/wallpaper sources')

assert 'customPreset.Visibility = inline ? Visibility.Collapsed : Visibility.Visible' in dialog
assert '.Where(item => item.Visibility == Visibility.Visible).ToArray()' in dialog
assert 'Grid.SetRow(frame, 1)' in desktop_preview
assert 'Grid.SetRow(_sourceBadge, 0)' in desktop_preview
assert 'Grid.SetRow(displayBadge, 2)' in desktop_preview
assert 'Grid.SetRow(_loading, 1)' in desktop_preview
assert '_sourceBadge.Opacity = 1' in desktop_preview
assert 'Visibility = Visibility.Visible;' not in desktop_preview
assert 'ReflectionDisplayProfile.GetDisplayNumber' in desktop_preview
assert 'SharesVerticalEdge' not in desktop_preview and 'sharedLeft' not in desktop_preview
assert 'BorderThickness = new Thickness(unit)' in desktop_preview
assert 'Width = Math.Max(0, monitor.Width - inset * 2)' in desktop_preview
assert 'double inset = Math.Max(6 * unit' in desktop_preview
assert 'Canvas.SetLeft(outline, monitor.X - left + inset)' in desktop_preview
assert 'CornerRadius = new CornerRadius(8 * unit)' in desktop_preview
assert 'desktop.Children.Add(outline)' in desktop_preview
assert 'public int MonitorCount => _bounds.Length' in desktop_preview
assert 'public string ProfileKey { get; }' in desktop_preview
assert 'MaxHeight = 280' in desktop_preview
print('PASS: labels in own rows, complete monitor frames with real gap and system numbers, one reflection canvas, inline picker hides its launcher')

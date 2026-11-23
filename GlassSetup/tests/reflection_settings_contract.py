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
assert service.index('ReflectionRenderState.Invalidate();') < service.index('var output = Publish(target);')
assert service.index('var output = Publish(target);') < service.index('ReflectionRenderState.Save(')
assert 'var directory = Path.GetFullPath(PlatformPaths.ReflectionAppliedDirectory);' in service
assert 'if (!IsTexture(source) && !isSvg) return source;' in service
assert 'settings with { NoisePercent = 0, OpacityPercent = 100 }, wrap: true)' in service
assert 'ReflectionPixelEffects.ApplyAlpha(repeated, width, height,' in service
assert 'ReflectionPixelEffects.ApplyAlpha(pixels, width, height, settings, wrap: false)' in service
assert 'if (category != 1) return items;' in page
assert '.Where(item => ReflectionCategory(item.FilePath) == category)' in page
assert 'ReflectionSettingsDialog.ShowAsync(source)' in page
assert 'if (settings is not null) await ApplyReflectionSourceAsync(source, settings)' in page
assert 'ReflectionRenderState.Save' not in dialog and 'WriteDwm' not in dialog
assert 'source, preview.DesktopWidth, preview.DesktopHeight, Values()' in dialog
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
assert 'MaxHeight = 760' in dialog and 'HostedDialogSpec' in dialog
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
for control in ('groups', 'picker', 'darkPreset', 'lightPreset'):
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
assert 'Grid.SetColumn(reset, 1)' in dialog and 'body.Children.Add(reset)' not in dialog
assert 'Shadow = new ThemeShadow()' in desktop_preview
assert 'Reflection_NoiseSand' in (root / 'GlassSettings/Strings/ru-RU/Resources.resw').read_text()
assert 'if (type == 4)' in effects and 'u < 1311' in effects and 'u < 64004' in effects
print('PASS: Pivot load restoration, top-right Defaults, preview shadow and independent-pixel sand grain')
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
assert 'MaxWidth = 1100' in dialog and 'Grid.SetColumn(pickerRail, 1)' in dialog
assert 'EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, callback, IntPtr.Zero)' in desktop_preview
assert 'WallpaperPreviewCrossfade' in desktop_preview
assert '_watcher.Changed += OnEnvironmentChanged' in desktop_preview
assert '_watcher.Changed -= OnEnvironmentChanged' in desktop_preview
assert 'new DesktopEnvironmentWatcher' not in desktop_preview
print('PASS: unified source tint/alpha picker, theme presets, proportional monitor layout, shared wallpaper lifecycle')
flat = (root / 'GlassSettings/Controls/FlatGlassPreviewLayer.cs').read_text()
shared_state = (root / 'GlassSettings/Services/CompositionPreviewRegistryStates.cs').read_text()
stylization = (root / 'GlassSettings/Controls/StylizationLayeredPreview.xaml.cs').read_text()
assert desktop_preview.index('layers.Children.Add(a)') < desktop_preview.index('layers.Children.Add(glass)')
assert desktop_preview.index('layers.Children.Add(glass)') < desktop_preview.index('layers.Children.Add(reflectionCanvas)')
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
assert 'bounds.Add(new Windows.Graphics.RectInt32(rect.Left, rect.Top,' in desktop_preview
assert 'rect.Right - rect.Left, rect.Bottom - rect.Top' in desktop_preview
assert 'return bounds.ToArray();' in desktop_preview
print('PASS: monitor snapshot uses native full monitor rectangles, without WinRT DisplayArea calls')

assert 'layers.Clip = new RectangleGeometry' in desktop_preview
assert 'CreateGeometricClip(' not in desktop_preview
assert 'GetElementVisual(layers)' not in desktop_preview
assert 'layers.Loaded +=' not in desktop_preview
print('PASS: XAML owns monitor clipping; no geometric Composition clip on its visual')

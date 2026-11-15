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
assert 'RenderConfiguredAsync(source, 384, 216, Values())' in dialog
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
assert 'MaxHeight = 620' in dialog and 'HostedDialogSpec' in dialog
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
assert 'int NoiseType = 0' in state and 'Math.Clamp(NoiseType, 0, 3)' in state
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

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
assert 'ReflectionPixelEffects.ApplyAlpha(tile, tw, th, settings, wrap: true)' in service
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
assert 'int NoiseSize = 1' in state and 'Math.Clamp(NoiseSize, 1, 32)' in state
assert 'initial.NoiseSize' in dialog and 'noiseSize.ValueChanged' in dialog
assert 'noiseSize.Value = 1' in dialog
for language in ('ru-RU', 'en-US'):
    assert 'Reflection_SettingNoiseSize' in (root / f'GlassSettings/Strings/{language}/Resources.resw').read_text()
print('PASS: grain size control, defaults, bounds, preview and reset wiring')

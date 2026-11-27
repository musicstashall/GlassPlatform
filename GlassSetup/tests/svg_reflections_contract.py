"""Portable source wiring checks only. SVG.NET/WinUI execution requires Windows."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
renderer = (root / 'GlassSettings/Services/SvgReflectionRenderer.cs').read_text()
service = (root / 'GlassSettings/Services/ReflectionTextureService.cs').read_text()
catalog = (root / 'GlassSettings/Services/ThemeImageCatalog.cs').read_text()
control = (root / 'GlassSettings/Controls/ReflectionImageTile.xaml.cs').read_text()
paths = (root / 'GlassSettings/Core/Platform/PlatformPaths.cs').read_text()
project = ET.parse(root / 'GlassSettings/GlassSettings.csproj')
assert project.find(".//PackageReference[@Include='Svg']").get('Version') == '3.4.7'
assert 'SvgReflectionRenderer.IsPattern(file)' in catalog
assert 'SvgReflectionRenderer.IsPattern(filePath)' in catalog
assert 'SvgReflectionRenderer.CreatePreviewAsync(FilePath)' in control
assert 'revision != _imageRevision' in control
assert 'Task.Run(() => SvgReflectionRenderer.RenderPng(bytes, width, height))' in service
assert 'RenderPng(source, 148, 92)' in renderer
assert 'new Svg.SvgUnit(width)' in renderer and 'document.Draw(width, height)' in renderer
assert 'bitmap.Save(output, ImageFormat.Png)' in renderer
assert 'ReflectionRenderState.Save(source, output' in service
assert 'profile.Key + ".png"' in service
assert 'Path.GetFullPath(state.Source)' in service
assert 'ReflectionPatternAppliedDirectory' not in service
assert '"OpenGlass", "Presets", "Pattern"' not in paths
assert 'var directory = Path.GetFullPath(PlatformPaths.ReflectionAppliedDirectory);' in service
assert 'DtdProcessing.Prohibit' in renderer and 'XmlResolver = null' in renderer
assert 'OfType<XProcessingInstruction>()' in renderer
assert 'nodes.Length > 512' in renderer and 'MaximumSourceBytes = 262144' in renderer
assert 'Only local SVG paint references' in renderer
# Positive allowlists, rather than a list of known-dangerous tags/attributes.
elements = set(re.findall(r'"([a-zA-Z]+)"', renderer.split('Elements =')[1].split('};')[0]))
attributes = set(re.findall(r'"([a-zA-Z0-9-]+)"', renderer.split('Attributes =')[1].split('};')[0]))
assert {'path', 'linearGradient', 'radialGradient', 'stop'} <= elements
assert not {'script', 'image', 'use', 'text', 'style', 'foreignObject', 'filter'} & elements
assert not {'href', 'style', 'onload', 'onclick'} & attributes
print('PASS: SVG catalog/preview/PNG pipeline, unified generated output, registry/source matching and restricted SVG policy wiring')
print('NOTE: no Windows compilation, SVG.NET render, WIC or DWM runtime exercised by this test')

# Unified vector sources retain the exact geometry/alpha of the former Dark asset.
ns = '{http://www.w3.org/2000/svg}'
assets = root / 'OpenGlass/Resources/GlassReflection/Pattern'
assert all(path.suffix.lower() == '.svg' for path in assets.iterdir() if path.is_file())
assert not list(assets.glob('*_Dark.*')) and not list(assets.glob('*_Lite.*'))
for name in ('SilkWave', 'FoldedVeil', 'PrismGlints', 'LiquidCaustics'):
    path = assets / f'{name}.svg'
    assert path.stat().st_size <= 262144
    svg = ET.parse(path).getroot()
    assert svg.tag == ns + 'svg'
    assert svg.get('viewBox') == '0 0 1920 1080'
    assert svg.get('preserveAspectRatio') == 'none'
    nodes = list(svg.iter())
    assert len(nodes) <= 512 and any(node.tag == ns + 'stop' for node in nodes)
    for node in nodes:
        assert node.tag.startswith(ns) and node.tag[len(ns):] in elements
        assert set(node.attrib) <= attributes
        for key, value in node.attrib.items():
            if 'url' in value.lower():
                assert value.startswith('url(#') and value.endswith(')')
            if key == 'stop-color':
                assert value == '#000000'
print('PASS: single SVG source per composition complies with renderer profile; color is generated')

# Every shipped Pattern SVG must pass the same restricted profile the renderer enforces
# (element/attribute allowlists, 256 KiB, 512 nodes, viewBox) so unsupported markup
# such as <use> never reaches GlassSettings.
svg_ns = '{http://www.w3.org/2000/svg}'
for svg in sorted((root / 'OpenGlass/Resources/GlassReflection/Pattern').glob('*.svg')):
    data = svg.read_bytes()
    assert len(data) <= 262144, f'{svg.name}: {len(data)} bytes > 256 KiB'
    tree = ET.parse(svg)
    nodes = list(tree.getroot().iter())
    assert len(nodes) <= 512, f'{svg.name}: {len(nodes)} nodes > 512'
    assert tree.getroot().get('viewBox'), f'{svg.name}: missing viewBox'
    for node in nodes:
        assert node.tag.startswith(svg_ns), f'{svg.name}: foreign element {node.tag}'
        name = node.tag[len(svg_ns):]
        assert name in elements, f'{svg.name}: element <{name}> not in renderer allowlist'
        for attr in node.attrib:
            assert attr in attributes, f'{svg.name}: attribute {attr} on <{name}> not in renderer allowlist'
print('PASS: all Pattern SVG files conform to the renderer element/attribute/size profile')

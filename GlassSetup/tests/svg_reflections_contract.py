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
assert 'Task.Run(() => SvgReflectionRenderer.RenderPng(sourceBytes, width, height))' in service
assert 'RenderPng(source, 148, 92)' in renderer
assert 'new Svg.SvgUnit(width)' in renderer and 'document.Draw(width, height)' in renderer
assert 'bitmap.Save(output, ImageFormat.Png)' in renderer
assert 'SvgReflectionRenderer.CacheVersion' in service
assert 'Path.ChangeExtension(source, ".png")' in service
assert 'Path.ChangeExtension(item.FilePath, ".png")' in service
assert 'PlatformPaths.ReflectionPatternAppliedDirectory' in service
assert '"OpenGlass", "Presets", "Pattern"' in paths
assert 'isSvg ? "Pattern" : "Textured"' in service
assert 'DtdProcessing.Prohibit' in renderer and 'XmlResolver = null' in renderer
assert 'OfType<XProcessingInstruction>()' in renderer
assert 'nodes.Length > 512' in renderer and 'MaximumSourceBytes = 262144' in renderer
assert 'Only local SVG paint references' in renderer
# Positive allowlists, rather than a list of known-dangerous tags/attributes.
elements = set(re.findall(r'"([a-zA-Z]+)"', renderer.split('Elements =')[1].split('};')[0]))
attributes = set(re.findall(r'"([a-zA-Z-]+)"', renderer.split('Attributes =')[1].split('};')[0]))
assert {'path', 'linearGradient', 'radialGradient', 'stop'} <= elements
assert not {'script', 'image', 'use', 'text', 'style', 'foreignObject', 'filter'} & elements
assert not {'href', 'style', 'onload', 'onclick'} & attributes
print('PASS: SVG catalog/preview/PNG pipeline, dedicated Pattern output, cache/source matching and restricted SVG policy wiring')
print('NOTE: no Windows compilation, SVG.NET render, WIC or DWM runtime exercised by this test')

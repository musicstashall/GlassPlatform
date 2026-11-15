"""Portable asset/source contracts. Does not execute WinRT, ACLs or OG rendering."""
from pathlib import Path
import struct
import hashlib
import zlib
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def decode_rgba(path, expected_size):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, compressed, count = 8, bytearray(), 0
    while offset < len(data):
        size = struct.unpack_from('>I', data, offset)[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + size]
        crc = struct.unpack_from('>I', data, offset + 8 + size)[0]
        assert zlib.crc32(kind + payload) & 0xffffffff == crc
        if kind == b'IHDR':
            w, h, depth, color, comp, filt, interlace = struct.unpack('>IIBBBBB', payload)
            assert (w, h, depth, color, comp, filt, interlace) == (*expected_size, 8, 6, 0, 0, 0)
        if kind == b'IDAT':
            compressed.extend(payload)
        count += 1
        offset += size + 12
        if kind == b'IEND':
            break
    assert offset == len(data) and count <= 4096
    raw = zlib.decompress(compressed)
    stride = w * 4
    assert len(raw) == (stride + 1) * h
    previous = bytearray(stride)
    pixels = bytearray()
    for y in range(h):
        start = y * (stride + 1)
        mode = raw[start]
        assert mode in range(5)
        row = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            a = row[x - 4] if x >= 4 else 0
            b = previous[x]
            c = previous[x - 4] if x >= 4 else 0
            p = a + b - c
            distances = (abs(p - a), abs(p - b), abs(p - c))
            paeth = (a, b, c)[distances.index(min(distances))]
            predictor = (0, a, b, (a + b) // 2, paeth)[mode]
            row[x] = (row[x] + predictor) & 255
        pixels.extend(row)
        previous = row
    return pixels


assets = ROOT / 'OpenGlass/Resources/GlassReflection/Textured'
for name in ('GlassReflection_01', 'GlassReflection_02'):
    size = (8, 8) if name == 'GlassReflection_02' else (18, 16)
    dark = decode_rgba(assets / (name + '_Dark.png'), size)
    lite = decode_rgba(assets / (name + '_Lite.png'), size)
    assert dark[3::4] == lite[3::4]
    assert len(set(dark[3::4])) > 1
    if name == 'GlassReflection_02':
        # Golden canvas hashes: approved geometry; Lite recolored to #B8B8B8.
        for pixels, expected_hash in (
            (dark, '6ca50312be4ef2ab027cc04f7cd8e9c0d081a62c7262a971e8bf32f5a041fc35'),
            (lite, '30a57629a033d712f0b00ed82293855c84f585d42f4bab631f5cae13c124585d'),
        ):
            canvas = b''.join(bytes(pixels[(y % 8) * 32:(y % 8 + 1) * 32]) * 240
                              for y in range(1080))
            assert hashlib.sha256(canvas).hexdigest() == expected_hash
        # No smaller axial period can reproduce the cell.
        for axis in (0, 1):
            for period in range(1, 8):
                assert any(dark[(y * 8 + x) * 4 + 3] !=
                           dark[((y if axis == 0 else y % period) * 8 +
                                 (x % period if axis == 0 else x)) * 4 + 3]
                           for y in range(8) for x in range(8))
    for channel in range(3):
        assert set(dark[channel::4]) == {0}
        assert set(lite[channel::4]) == {184}
print('PASS: RGBA pairs, 02 minimal 8x8 cell; full canvases match approved 256px tiles')

paths = (ROOT / 'GlassSettings/Core/Platform/PlatformPaths.cs').read_text()
service = (ROOT / 'GlassSettings/Services/ReflectionTextureService.cs').read_text()
page = (ROOT / 'GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs').read_text()
assert 'SpecialFolder.CommonApplicationData' in paths
assert '"OpenGlass", "Presets", "Textured"' in paths
assert service.count('return Publish(target);') == 2  # cache hits must publish/clean too
assert 'PlatformPaths.ReflectionAppliedDirectory' in service
assert service.index('Inventory(directory);') < service.index('File.Delete(file)')
assert service.index('File.Delete(file)') < service.index('File.Copy(cachedImage, temporary')
assert service.index('File.Copy(cachedImage, temporary') < service.index('File.Move(temporary, target', service.index('private static string Publish'))
assert 'FileAttributes.ReparsePoint' in service
assert 'Directory.Delete(directory' not in service
assert 'preparationError = error' in page
for language in ('ru-RU', 'en-US'):
    tree = ET.parse(ROOT / f'GlassSettings/Strings/{language}/Resources.resw')
    for key in ('Dialog_ReflectionPrepareFailedTitle', 'Dialog_ReflectionPrepareFailedBody'):
        assert tree.find(f"data[@name='{key}']/value").text
print('PASS: ProgramData destination, cleanup before copying including cache hits, localized failure UI')

control = (ROOT / 'GlassSettings/Controls/ReflectionImageTile.xaml.cs').read_text()
assert 'ReflectionTextureService.CreatePreviewAsync(FilePath)' in control
assert 'revision != _imageRevision' in control
assert 'RepeatPixels(pixels.DetachPixelData()' in service
assert 'Task.Run(() => RepeatPixels(tile' in service
print('PASS: texture previews and applied canvas share unscaled pixel repetition; stale previews guarded')

assert 'TransparencyCheckerboard.CreateImage(148, 92, colored: true)' in control
xaml = (ROOT / 'GlassSettings/Controls/ReflectionImageTile.xaml').read_text()
assert xaml.index('x:Name="CheckerboardImage"') < xaml.index('x:Name="PreviewImage"')
print('PASS: shared atlas checkerboard placed behind reflection preview')

checker = (ROOT / 'GlassSettings/Services/TransparencyCheckerboard.cs').read_text()
assert 'bool colored = false' in checker
assert '157, 174, 177' in checker and '177, 163, 157' in checker
print('PASS: muted colored reflection substrate, original default retained for atlases')

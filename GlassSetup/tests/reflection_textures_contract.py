"""Portable asset/source contracts. Does not execute WinRT, ACLs or OG rendering."""
from pathlib import Path
import struct
import hashlib
import zlib
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def decode_rgba(path, expected_size):
    # Preserve historical alpha/canvas goldens by synthesizing the two old paint
    # presets from the single asset; paired resource files must no longer exist.
    lite_preset = path.stem.endswith('_Lite')
    for suffix in ('_Dark', '_Lite'):
        if path.stem.endswith(suffix):
            path = path.with_name(path.stem[:-len(suffix)] + path.suffix)
            break
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
    if lite_preset:
        for i in range(0, len(pixels), 4):
            pixels[i:i + 3] = bytes((160, 160, 160))
    return pixels


assets = ROOT / 'OpenGlass/Resources/GlassReflection/Textured'
for name in ('TextureFrostWeave', 'TextureDiagonalWeave'):
    size = (8, 8) if name == 'TextureDiagonalWeave' else (21, 18)
    dark = decode_rgba(assets / (name + '_Dark.png'), size)
    lite = decode_rgba(assets / (name + '_Lite.png'), size)
    assert dark[3::4] == lite[3::4]
    assert len(set(dark[3::4])) > 1
    if name == 'TextureDiagonalWeave':
        # Golden canvas hashes: 160% geometry; Lite recolored to #A0A0A0.
        for pixels, expected_hash in (
            (dark, '6ca50312be4ef2ab027cc04f7cd8e9c0d081a62c7262a971e8bf32f5a041fc35'),
            (lite, '01c198d706c162045ecfd75ba8c110ccfea6b6408152829feb0d158ec716696f'),
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
        assert set(lite[channel::4]) == {160}
print('PASS: unified source/preset colors, 02 minimal 8x8 cell and 160% canvas regression hashes')

paths = (ROOT / 'GlassSettings/Core/Platform/PlatformPaths.cs').read_text()
service = (ROOT / 'GlassSettings/Services/ReflectionTextureService.cs').read_text()
page = (ROOT / 'GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs').read_text()
assert 'SpecialFolder.CommonApplicationData' in paths
assert '"OpenGlass", "Presets", "Textured"' in paths
assert 'var output = Publish(target);' in service
assert 'PlatformPaths.ReflectionAppliedDirectory' in service
assert 'Inventory(directory)' not in service and 'File.Delete(file)' not in service
assert 'profile.Key + ".png"' in service
assert service.index('File.Copy(cachedImage, temporary') < service.index('File.Move(temporary, target', service.index('private static string Publish'))
assert 'FileAttributes.ReparsePoint' in service
assert 'Directory.Delete(directory' not in service
assert 'preparationError = error' in page
for language in ('ru-RU', 'en-US'):
    tree = ET.parse(ROOT / f'GlassSettings/Strings/{language}/Resources.resw')
    for key in ('Dialog_ReflectionPrepareFailedTitle', 'Dialog_ReflectionPrepareFailedBody'):
        assert tree.find(f"data[@name='{key}']/value").text
print('PASS: ProgramData destination, profile-preserving atomic copy, localized failure UI')

control = (ROOT / 'GlassSettings/Controls/ReflectionImageTile.xaml.cs').read_text()
assert 'ReflectionTextureService.CreatePreviewAsync(FilePath)' in control
assert 'revision != _imageRevision' in control
assert 'RepeatPixels(pixels.DetachPixelData()' in service
assert 'var repeated = RepeatPixels(tile, tw, th, width, height);' in service
assert 'return repeated;' in service
print('PASS: texture previews and applied canvas share unscaled pixel repetition; stale previews guarded')

xaml = (ROOT / 'GlassSettings/Controls/ReflectionImageTile.xaml').read_text()
assert 'CheckerboardImage' not in xaml and 'TransparencyCheckerboard' not in control
assert 'Background="#FF909090"' in xaml
print('PASS: flat neutral reflection substrate, no checkerboard')

# Approved MicroBasket geometry and alpha must match the original approval swatch.
basket_dark = decode_rgba(assets / 'TextureMicroBasket_Dark.png', (8, 8))
basket_lite = decode_rgba(assets / 'TextureMicroBasket_Lite.png', (8, 8))
expected_alpha = bytes(150 if ((x // 2 + y // 2) % 2 == 0 and x % 2 == 0)
                       or ((x // 2 + y // 2) % 2 == 1 and y % 2 == 0) else 65
                       for y in range(8) for x in range(8))
assert basket_dark[3::4] == basket_lite[3::4] == expected_alpha
for channel in range(3):
    assert set(basket_dark[channel::4]) == {0}
    assert set(basket_lite[channel::4]) == {160}
print('PASS: approved MicroBasket 8x8 geometry, single alpha source and synthesized Dark/Lite presets')

carbon_dark = decode_rgba(assets / 'TextureCarbonWeave_Dark.png', (16, 16))
carbon_lite = decode_rgba(assets / 'TextureCarbonWeave_Lite.png', (16, 16))
# Round 13 approval: the 8x8 checkerboard enlarged twice, pixel-exact blocks.
expected_carbon = bytes(168 if (x // 2 + y // 2) % 2 == 0 else 42
                        for y in range(16) for x in range(16))
assert carbon_dark[3::4] == carbon_lite[3::4] == expected_carbon
for channel in range(3):
    assert set(carbon_dark[channel::4]) == {0}
    assert set(carbon_lite[channel::4]) == {160}
print('PASS: approved CarbonWeave 16x16 (2x) geometry, single alpha source and synthesized Dark/Lite presets')

fine_dark = decode_rgba(assets / 'TextureFineDiagonal_Dark.png', (9, 9))
fine_lite = decode_rgba(assets / 'TextureFineDiagonal_Lite.png', (9, 9))


def _fine_weights(t):
    # Overlap of the target pixel footprint [t/1.5, (t+1)/1.5) with source
    # phase cells mod 3: the approved 1.5x geometry of the 3x3 diagonal.
    lo, hi = t / 1.5, (t + 1) / 1.5
    acc = {}
    for s in range(int(lo), int(hi - 1e-12) + 2):
        w = min(hi, s + 1) - max(lo, s)
        if w > 1e-12:
            acc[s % 3] = acc.get(s % 3, 0.0) + w
    tot = sum(acc.values())
    return [(i, w / tot) for i, w in sorted(acc.items())]


expected_fine_alpha = bytes(
    round(sum(wx * wy * (60, 95, 125)[(i + j) % 3]
              for i, wx in _fine_weights(x) for j, wy in _fine_weights(y)))
    for y in range(9) for x in range(9))
assert fine_dark[3::4] == fine_lite[3::4] == expected_fine_alpha
for channel in range(3):
    assert set(fine_dark[channel::4]) == {0}
    assert set(fine_lite[channel::4]) == {160}
print('PASS: approved FineDiagonal 9x9 (1.5x) geometry, single alpha source and synthesized Dark/Lite presets')

dragon_dark = decode_rgba(assets / 'TextureDragonScale_Dark.png', (8, 8))
dragon_lite = decode_rgba(assets / 'TextureDragonScale_Lite.png', (8, 8))
# Regression alpha for the periodically filtered 2/3-size DragonScale.
expected_dragon = bytes.fromhex(
    "4f342d3e4f342d3e564e3b55564e3b552f47473d2f47473d2d32492d2d32492d2d3e4f342d3e4f343b55564e3b55564e473d2f47473d2f47492d2d32492d2d32"
)
assert dragon_dark[3::4] == dragon_lite[3::4] == expected_dragon
for channel in range(3):
    assert set(dragon_dark[channel::4]) == {0}
    assert set(dragon_lite[channel::4]) == {160}
print('PASS: DragonScale 8x8 geometry, single alpha source and synthesized Dark/Lite presets')


stitch_dark = decode_rgba(assets / 'TextureCrossStitch_Dark.png', (8, 8))
stitch_lite = decode_rgba(assets / 'TextureCrossStitch_Lite.png', (8, 8))
expected_stitch = bytearray()
for y in range(8):
    for x in range(8):
        u, v = (x % 4, y % 4) if (x // 4 + y // 4) % 2 == 0 else (y % 4, x % 4)
        alpha = 45
        if v <= 2:
            if u == 1:
                alpha = 155 if v == 1 else 130
            elif u == 2:
                alpha = 85
        expected_stitch.append(alpha)
assert stitch_dark[3::4] == stitch_lite[3::4] == expected_stitch
for channel in range(3):
    assert set(stitch_dark[channel::4]) == {0}
    assert set(stitch_lite[channel::4]) == {160}
print('PASS: approved dense CrossStitch geometry and synthesized theme presets')

twill_dark = decode_rgba(assets / 'TextureTwillWeave_Dark.png', (8, 8))
twill_lite = decode_rgba(assets / 'TextureTwillWeave_Lite.png', (8, 8))
expected_twill = bytearray()
for y in range(8):
    for x in range(8):
        over = (x - y) % 4 != 0
        alpha = (((145, 100)[x % 2] if y % 2 == 0 else (125, 80)[x % 2])
                 if over else (55 if y % 2 == 0 else 110))
        expected_twill.append(alpha)
assert twill_dark[3::4] == twill_lite[3::4] == expected_twill
for channel in range(3):
    assert set(twill_dark[channel::4]) == {0}
    assert set(twill_lite[channel::4]) == {160}
print('PASS: approved TwillWeave geometry and synthesized theme presets')

assert not list(assets.glob('*_Dark.*')) and not list(assets.glob('*_Lite.*'))
print('PASS: current sources have no theme suffix pairs')

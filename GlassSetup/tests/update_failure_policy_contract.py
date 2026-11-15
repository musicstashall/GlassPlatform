"""Portable update failure/interval wiring checks; no Windows registry or live HTTP."""
from pathlib import Path
from datetime import datetime, timezone
import xml.etree.ElementTree as ET
root = Path(__file__).resolve().parents[2]
read = lambda p: (root / p).read_text()
coordinator = read('GlassSettings/Services/ProductUpdateCoordinator.cs')
glass = read('GlassSettings/Services/GlassReleaseUpdateService.cs')
og = read('GlassSettings/Services/OpenGlassReleaseUpdateService.cs')
registry = read('GlassSettings/Core/Registry/GlassSettingsRegistry.GlassUpdate.cs')
common = read('GlassSettings/Core/Registry/GlassSettingsRegistry.cs')
assert 'ShouldShowOfflineNotice' not in coordinator and 'OfflineNoticeText' not in coordinator
for page in ('HomePage', 'SystemPage'):
    text = read(f'GlassSettings/Pages/{page}.xaml.cs')
    assert 'ShouldShowOfflineNotice' not in text and 'OfflineNoticeText' not in text
    assert 'ShouldShowGlassNotice' in text and 'ShouldShowOpenGlassNotice' in text
for language in ('ru-RU', 'en-US'):
    resources = ET.fromstring(read(f'GlassSettings/Strings/{language}/Resources.resw'))
    assert resources.find("data[@name='Home_UpdateCheckOffline']") is None
assert 'if (check.Kind != GlassCatalogCheckKind.Ready)\n            return;' in coordinator
assert 'if (!check.Reached)\n            return;' in coordinator
assert '[UpdateCheck]' in glass and '[UpdateCheck]' in og
assert 'Glass catalog HTTP {0}' in glass and 'schema is unsupported' in glass
assert 'catch (JsonException error)' in glass
assert 'OpenGlass releases HTTP {0}' in og
assert 'ReadUpdateCheckUtc(GlassUpdateLastCheckUtcValue)' in registry
assert 'ReadUpdateCheckUtc(OpenGlassUpdateLastCheckUtcValue)' in common
assert 'DateTime.TryParseExact(text, "O"' in registry
assert 'DateTimeStyles.RoundtripKind' in registry
assert 'utc.ToUniversalTime().ToString("O"' in registry
assert 'unchecked((uint)utc.ToFileTimeUtc())' not in registry + common
# Demonstrate the previous DWORD format could never retain a modern FILETIME.
now = datetime(2026, 11, 15, tzinfo=timezone.utc)
filetime = int((now - datetime(1601, 1, 1, tzinfo=timezone.utc)).total_seconds() * 10_000_000)
assert filetime > 0xFFFFFFFF and (filetime & 0xFFFFFFFF) / 10_000_000 < 430
assert datetime.fromisoformat(now.isoformat()) == now
print('PASS: no failure banners, real offers retained, diagnostics distinguish failures, full UTC timestamps for both products')
print('NOTE: contracts and reference time arithmetic only; Windows HTTP/registry not executed')

"""Source contracts only: no .NET/WinUI/runtime launch is exercised."""
from pathlib import Path
import xml.etree.ElementTree as ET
root = Path(__file__).resolve().parents[2]
coordinator = (root/'GlassSettings/Services/ProductUpdateCoordinator.cs').read_text()
launcher = (root/'GlassSettings/Services/GlassMaintenanceSetupLaunchService.cs').read_text()
def function(text, token):
    start=text.index(token); brace=text.index('{',start); depth=0
    for i in range(brace,len(text)):
        if text[i]=='{': depth+=1
        elif text[i]=='}':
            depth-=1
            if not depth: return text[start:i+1]
    raise ValueError(token)
for name in ('OpenPendingGlassDownloadAsync()', 'OpenPendingOpenGlassDownloadAsync()'):
    body=function(coordinator,name)
    assert 'GlassMaintenanceSetupLaunchService.TryLaunch()' in body
    assert 'Launcher' not in body and 'Http' not in body and 'Download' not in body[body.index('{'):]
assert 'Launcher.LaunchUriAsync' not in coordinator
assert 'Arguments = "--modify"' in launcher
assert 'if (!process.WaitForExit(60_000))' in launcher
assert 'Path.GetFullPath(NativeDllName)' not in launcher
assert 'return process is not null;' in launcher
for lang, expected in [('ru-RU','Открыть обслуживание'),('en-US','Open maintenance')]:
    xml=ET.parse(root/f'GlassSettings/Strings/{lang}/Resources.resw')
    assert xml.find("./data[@name='System_OpenGlassUpdateAction']/value").text == expected
print('PASS: existing update actions launch maintenance only, timeout/launch result/CWD guards, RU/EN labels (source contracts)')

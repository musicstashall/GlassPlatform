using Microsoft.Win32;

namespace GlassSettings.Core.Registry;

/// <summary>BlockList — процессы без инъекции Glass (HKCU\...\BlackWindowManager\BlockList + встроенный список).</summary>
public static class BlockListRegistry
{
    private const string SubKey = "BlockList";

    public sealed record BlockListEntry(string ExeName, bool IsBuiltIn);

    /// <summary>Зеркало kStaticBlockList из ProcessBlockList.cpp.</summary>
    private static readonly string[] BuiltInExeNames =
    [
        "applicationframehost.exe",
        "backgroundtaskhost.exe",
        "corewidgetprovider.exe",
        "dwm.exe",
        "filecoauth.exe",
        "gamebar.exe",
        "gamebarftserver.exe",
        "mblctr.exe",
        "microsoft.sharepoint.exe",
        "mobsync.exe",
        "msedge.exe",
        "msedgewebview2.exe",
        "onedrivestandaloneupdater.exe",
        "perfwatson2.exe",
        "runtimebroker.exe",
        "sdxhelper.exe",
        "searchhost.exe",
        "servicehub.threadedwaitdialog.exe",
        "servicehub.vsdetouredhost.exe",
        "shellexperiencehost.exe",
        "sihost.exe",
        "smartscreen.exe",
        "splwow64.exe",
        "spoolsv.exe",
        "startmenuexperiencehost.exe",
        "svchost.exe",
        "taskhostw.exe",
        "werfault.exe",
        "widgetservice.exe",
        "widgets.exe",
        "windowspackagemanagerserver.exe",
        "wkstartcpl64.exe",
        "wkstartcpl86.exe",
        "wshost.exe",
        "avpui.exe",
        "devenv.exe",
        "msvsmon.exe",
        "vsdebugger.exe",
        "servicehub.host.dotnet.arm64.exe",
        "servicehub.host.dotnet.x64.exe",
        "apimonitor-x64.exe",
        "apimonitor-x86.exe",
        "dbgview.exe",
        "dbgview64.exe",
        "procexp.exe",
        "procexp64.exe",
        "procmon.exe",
        "procmon64.exe",
        "windbg.exe",
        "x32dbg.exe",
        "x64dbg.exe",
        "logviewer.exe",
        "glassconfig.exe",
        "glasssettings.exe",
    ];

    private static readonly HashSet<string> BuiltInSet =
        new(BuiltInExeNames, StringComparer.OrdinalIgnoreCase);

    public static string NormalizeExeName(string exeName) =>
        AttachListRegistry.NormalizeExeName(exeName);

    public static bool IsBuiltIn(string exeName) =>
        BuiltInSet.Contains(NormalizeExeName(exeName));

    public static bool IsBlocked(string exeName) =>
        IsBuiltIn(exeName) || IsInRegistry(exeName);

    public static IReadOnlyList<BlockListEntry> GetUserEntries() =>
        GetRegistryExeNames()
            .Select(static exe => new BlockListEntry(exe, false))
            .OrderBy(static e => e.ExeName, StringComparer.OrdinalIgnoreCase)
            .ToList();

    public static IReadOnlyList<BlockListEntry> GetAllEntries()
    {
        var byExe = new Dictionary<string, BlockListEntry>(StringComparer.OrdinalIgnoreCase);

        foreach (var exe in BuiltInExeNames)
        {
            var normalized = NormalizeExeName(exe);
            byExe[normalized] = new BlockListEntry(normalized, true);
        }

        foreach (var exe in GetRegistryExeNames())
        {
            var normalized = NormalizeExeName(exe);
            byExe[normalized] = new BlockListEntry(normalized, BuiltInSet.Contains(normalized));
        }

        return byExe.Values
            .OrderBy(static e => e.ExeName, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    public static IReadOnlyList<string> GetRegistryExeNames()
    {
        var path = RegistryPaths.GlassRoot + "\\" + SubKey;
        using var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(path, false);
        if (key is null)
            return [];

        var list = new List<string>();
        foreach (var name in key.GetValueNames())
        {
            if (!ReadBlockedFlag(key, name))
                continue;

            list.Add(NormalizeExeName(name));
        }

        return list;
    }

    public static void Add(string exeName) =>
        GlassRegistry.WriteDword(SubKey, NormalizeExeName(exeName), 1);

    public static void Remove(string exeName) =>
        GlassRegistry.DeleteValue(SubKey, NormalizeExeName(exeName));

    public static bool CanRemove(string exeName) =>
        !IsBuiltIn(exeName) && IsInRegistry(exeName);

    private static bool IsInRegistry(string exeName)
    {
        var raw = GlassRegistry.ReadDword(SubKey, NormalizeExeName(exeName));
        return raw is > 0;
    }

    private static bool ReadBlockedFlag(RegistryKey key, string name) =>
        key.GetValue(name) switch
        {
            int i => i != 0,
            uint u => u != 0,
            _ => false
        };
}

using GlassSettings.Services;
using Microsoft.Win32;

namespace GlassSettings.Core.Registry;

/// <summary>BlockList — процессы без полной инъекции Glass (HKCU\...\BlockList + встроенный список из ProcessBlockList.h).</summary>
public static class BlockListRegistry
{
    private const string SubKey = "BlockList";

    public sealed record BlockListEntry(string ExeName, bool IsBuiltIn);

    public static string NormalizeExeName(string exeName) =>
        AttachListRegistry.NormalizeExeName(exeName);

    public static bool IsBuiltIn(string exeName)
    {
        var normalized = NormalizeExeName(exeName);
        return normalized.Length > 0 && NativeHostInterop.IsBuiltInBlockList(normalized);
    }

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

        foreach (var exe in EnumerateBuiltInExeNames())
            byExe[exe] = new BlockListEntry(exe, true);

        foreach (var exe in GetRegistryExeNames())
        {
            var normalized = NormalizeExeName(exe);
            byExe[normalized] = new BlockListEntry(normalized, IsBuiltIn(normalized));
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

    private static IEnumerable<string> EnumerateBuiltInExeNames()
    {
        var count = NativeHostInterop.GetBuiltInBlockListCount();
        if (count <= 0)
            yield break;

        var buffer = new char[260];
        for (var i = 0; i < count; i++)
        {
            Array.Clear(buffer);
            if (!NativeHostInterop.TryCopyBuiltInBlockListAt(i, buffer))
                continue;
            var end = Array.IndexOf(buffer, '\0');
            var raw = end >= 0 ? new string(buffer, 0, end) : new string(buffer);
            var normalized = NormalizeExeName(raw);
            if (normalized.Length > 0)
                yield return normalized;
        }
    }

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

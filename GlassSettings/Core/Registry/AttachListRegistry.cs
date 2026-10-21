using GlassSettings.Strings;
using Microsoft.Win32;

namespace GlassSettings.Core.Registry;

/// <summary>Список процессов AttachList (HKCU\...\GlassPlatform\AttachList).</summary>
public static class AttachListRegistry
{
    private const string SubKey = "AttachList";

    public sealed record AttachListStats(int Total, int Full, int Popup, int Forbidden);

    public sealed record AttachListEntry(string ExeName, int RawValue, AttachProcessMode Mode);

    public static AttachProcessMode ParseRaw(int raw) =>
        raw >= 2 ? AttachProcessMode.Full :
        raw == 1 ? AttachProcessMode.PopupOnly :
        AttachProcessMode.Forbidden;

    public static uint ModeToRaw(AttachProcessMode mode) => (uint)mode;

    public static string NormalizeExeName(string exeName) =>
        exeName.Trim().ToLowerInvariant();

    public static IReadOnlyList<AttachListEntry> GetEntries()
    {
        var path = RegistryPaths.GlassRoot + "\\" + SubKey;
        using var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(path, false);
        if (key is null)
            return Array.Empty<AttachListEntry>();

        var list = new List<AttachListEntry>();
        foreach (var name in key.GetValueNames())
        {
            if (!TryReadRaw(key, name, out var raw))
                continue;

            var normalized = NormalizeExeName(name);
            list.Add(new AttachListEntry(normalized, raw, ParseRaw(raw)));
        }

        return list;
    }

    private static bool TryReadRaw(RegistryKey key, string name, out int raw)
    {
        raw = 0;
        return key.GetValue(name) switch
        {
            int i => AssignRaw(i, out raw),
            uint u => AssignRaw(unchecked((int)u), out raw),
            byte[] bytes when bytes.Length >= 4 => AssignRaw(BitConverter.ToInt32(bytes, 0), out raw),
            _ => false
        };
    }

    private static bool AssignRaw(int value, out int raw)
    {
        raw = value;
        return true;
    }

    public static bool Contains(string exeName)
    {
        var normalized = NormalizeExeName(exeName);
        return GetEntries().Any(e => e.ExeName == normalized);
    }

    public static void SetMode(string exeName, AttachProcessMode mode) =>
        GlassRegistry.WriteDword(SubKey, NormalizeExeName(exeName), ModeToRaw(mode));

    public static void Remove(string exeName) =>
        GlassRegistry.DeleteValue(SubKey, NormalizeExeName(exeName));

    public static AttachListStats GetStats()
    {
        var entries = GetEntries();
        var total = entries.Count;
        var full = 0;
        var popup = 0;
        var forbidden = 0;

        foreach (var entry in entries)
        {
            switch (entry.Mode)
            {
                case AttachProcessMode.Full:
                    full++;
                    break;
                case AttachProcessMode.PopupOnly:
                    popup++;
                    break;
                default:
                    forbidden++;
                    break;
            }
        }

        return new AttachListStats(total, full, popup, forbidden);
    }

    public static string SummarizeLine()
    {
        var s = GetStats();
        if (s.Total == 0)
            return GlassSettingsResources.Get("Attach_EmptyList");

        return GlassSettingsResources.CountForm(
            s.Total,
            "Attach_Process_One",
            "Attach_Process_Few",
            "Attach_Process_Many");
    }

    public static string DetailLine()
    {
        var s = GetStats();
        if (s.Total == 0)
            return string.Empty;

        var parts = new List<string>();
        if (s.Full > 0) parts.Add(GlassSettingsResources.Format("Attach_DetailFull", s.Full));
        if (s.Popup > 0) parts.Add(GlassSettingsResources.Format("Attach_DetailPopup", s.Popup));
        if (s.Forbidden > 0) parts.Add(GlassSettingsResources.Format("Attach_DetailForbidden", s.Forbidden));
        return string.Join(GlassSettingsResources.Get("Attach_DetailSeparator"), parts);
    }

    public static string ModeLabel(AttachProcessMode mode) => mode switch
    {
        AttachProcessMode.Full => GlassSettingsResources.Get("Attach_ModeFull"),
        AttachProcessMode.PopupOnly => GlassSettingsResources.Get("Attach_ModePopup"),
        _ => GlassSettingsResources.Get("Attach_ModeForbidden")
    };
}

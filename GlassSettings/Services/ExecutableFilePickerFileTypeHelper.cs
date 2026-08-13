using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>Системная строка типа файла (колонка «Тип» проводника).</summary>
internal static class ExecutableFilePickerFileTypeHelper
{
    private const uint FileAttributeNormal = 0x00000080;
    private const uint ShgfiTypename = 0x000000400;
    private const uint ShgfiUseFileAttributes = 0x000000010;

    private static readonly ConcurrentDictionary<string, string> ExtensionCache = new(StringComparer.OrdinalIgnoreCase);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Shfileinfo
    {
        public IntPtr hIcon;
        public int iIcon;
        public uint dwAttributes;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string szDisplayName;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)]
        public string szTypeName;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SHGetFileInfo(
        string pszPath,
        uint dwFileAttributes,
        ref Shfileinfo psfi,
        uint cbFileInfo,
        uint uFlags);

    public static string GetTypeLabel(string filePath)
    {
        var extension = Path.GetExtension(filePath);
        if (string.IsNullOrEmpty(extension))
            return Path.GetFileName(filePath);

        if (ExtensionCache.TryGetValue(extension, out var cached))
            return cached;

        var label = QueryTypeName(filePath) ?? extension.ToUpperInvariant();
        ExtensionCache[extension] = label;
        return label;
    }

    private static string? QueryTypeName(string path)
    {
        var shInfo = new Shfileinfo();
        var result = SHGetFileInfo(path, 0, ref shInfo, (uint)Marshal.SizeOf<Shfileinfo>(), ShgfiTypename);
        if (result != IntPtr.Zero && !string.IsNullOrWhiteSpace(shInfo.szTypeName))
            return shInfo.szTypeName.Trim();

        var extension = Path.GetExtension(path);
        if (string.IsNullOrEmpty(extension))
            return null;

        shInfo = new Shfileinfo();
        result = SHGetFileInfo(
            extension,
            FileAttributeNormal,
            ref shInfo,
            (uint)Marshal.SizeOf<Shfileinfo>(),
            ShgfiTypename | ShgfiUseFileAttributes);
        if (result == IntPtr.Zero || string.IsNullOrWhiteSpace(shInfo.szTypeName))
            return null;

        return shInfo.szTypeName.Trim();
    }
}

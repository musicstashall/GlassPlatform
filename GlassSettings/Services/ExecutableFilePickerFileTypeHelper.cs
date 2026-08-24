using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>Системная строка типа файла (колонка «Тип» проводника).</summary>
internal static class ExecutableFilePickerFileTypeHelper
{
    private const uint FileAttributeNormal = 0x00000080;
    private const uint ShgfiTypename = 0x000000400;
    private const uint ShgfiUseFileAttributes = 0x000000010;
    private const int MaxPath = 260;

    private static readonly ConcurrentDictionary<string, string> ExtensionCache = new(StringComparer.OrdinalIgnoreCase);

    private static int ShFileInfoByteSize =>
        IntPtr.Size + sizeof(int) + sizeof(uint) + (MaxPath * 2) + (80 * 2);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SHGetFileInfo(
        string pszPath,
        uint dwFileAttributes,
        IntPtr psfi,
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
        var fromPath = QueryTypeNameCore(path, 0, ShgfiTypename);
        if (!string.IsNullOrWhiteSpace(fromPath))
            return fromPath.Trim();

        var extension = Path.GetExtension(path);
        if (string.IsNullOrEmpty(extension))
            return null;

        var fromExt = QueryTypeNameCore(
            extension,
            FileAttributeNormal,
            ShgfiTypename | ShgfiUseFileAttributes);
        return string.IsNullOrWhiteSpace(fromExt) ? null : fromExt.Trim();
    }

    private static string? QueryTypeNameCore(string path, uint fileAttributes, uint flags)
    {
        var cb = ShFileInfoByteSize;
        var buf = Marshal.AllocHGlobal(cb);
        try
        {
            var zeros = new byte[cb];
            Marshal.Copy(zeros, 0, buf, cb);
            var result = SHGetFileInfo(path, fileAttributes, buf, (uint)cb, flags);
            if (result == IntPtr.Zero)
                return null;

            var typeOffset = IntPtr.Size + sizeof(int) + sizeof(uint) + (MaxPath * 2);
            return Marshal.PtrToStringUni(buf + typeOffset);
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
    }
}

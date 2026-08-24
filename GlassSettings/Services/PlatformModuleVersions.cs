using System.Diagnostics;
using System.Runtime.InteropServices;
using GlassSettings.Core.Platform;
using GlassSettings.Strings;

namespace GlassSettings.Services;

public static class PlatformModuleVersions
{
    private static readonly string[] ModuleNames =
    [
        "GlassCompat.dll",
        "GlassCore.dll",
        "WndCompat.dll",
        "ExplorerCompat.dll",
        "TaskbarCompat.dll",
        "UICompat.dll",
    ];

    public static string GetVersion(string dllFileName)
    {
        var path = ResolveDllPath(dllFileName);
        return path is null
            ? GlassSettingsResources.Get("Modules_NotFound")
            : GetVersionFromPath(path);
    }

    public static string GetVersionFromPath(string path)
    {
        if (!File.Exists(path))
            return GlassSettingsResources.Get("Modules_NotFound");

        try
        {
            if (TryReadVersionString(path) is { } version)
                return version;

            if (TryReadFixedFileVersion(path) is { } fixedVer)
                return fixedVer;

            var info = FileVersionInfo.GetVersionInfo(path);
            if (!string.IsNullOrWhiteSpace(info.FileVersion))
                return info.FileVersion.Trim();
            if (!string.IsNullOrWhiteSpace(info.ProductVersion))
                return info.ProductVersion.Trim();
        }
        catch
        {
            /* ignore */
        }

        return GlassSettingsResources.Get("Modules_Unavailable");
    }

    public static IReadOnlyList<(string Name, string Version)> GetAll() =>
        GetAllDetailed().Select(item => (item.Name, item.FileVersion)).ToList();

    /// <summary>
    /// Версия файла и продукта только для модулей, реально найденных рядом с GS / в процессе.
    /// Отсутствующие DLL (например ExplorerCompat/TaskbarCompat на Win10) в список не попадают.
    /// </summary>
    public static IReadOnlyList<(string Name, string FileVersion, string ProductVersion)> GetAllDetailed()
    {
        var list = new List<(string Name, string FileVersion, string ProductVersion)>(ModuleNames.Length);
        foreach (var n in ModuleNames)
        {
            var path = ResolveDllPath(n);
            if (path is null || !File.Exists(path))
                continue;

            list.Add((Path.GetFileNameWithoutExtension(n), ReadFileVersion(path), ReadProductVersion(path)));
        }

        return list;
    }

    /// <summary>Версия продукта (единый источник истины) — берётся из любого присутствующего модуля.</summary>
    public static string GetProductVersion()
    {
        foreach (var module in ModuleNames)
        {
            var path = ResolveDllPath(module);
            if (path is null)
                continue;

            var product = ReadProductVersion(path);
            if (!IsPlaceholder(product))
                return product;
        }

        return GlassSettingsResources.Get("Modules_Unavailable");
    }

    private static string ReadFileVersion(string path)
    {
        if (!File.Exists(path))
            return GlassSettingsResources.Get("Modules_NotFound");

        try
        {
            if (TryReadStringValue(path, "FileVersion") is { } s)
                return s;
            if (TryReadFixedFileVersion(path) is { } f)
                return f;
        }
        catch { /* ignore */ }

        return GlassSettingsResources.Get("Modules_Unavailable");
    }

    private static string ReadProductVersion(string path)
    {
        if (!File.Exists(path))
            return GlassSettingsResources.Get("Modules_NotFound");

        try
        {
            if (TryReadStringValue(path, "ProductVersion") is { } s)
                return s;

            var info = FileVersionInfo.GetVersionInfo(path);
            if (!string.IsNullOrWhiteSpace(info.ProductVersion))
                return info.ProductVersion.Trim();
        }
        catch { /* ignore */ }

        return GlassSettingsResources.Get("Modules_Unavailable");
    }

    private static bool IsPlaceholder(string value) =>
        string.Equals(value, GlassSettingsResources.Get("Modules_NotFound"), StringComparison.Ordinal)
        || string.Equals(value, GlassSettingsResources.Get("Modules_Unavailable"), StringComparison.Ordinal);

    private static string? ResolveDllPath(string dllFileName)
    {
        var baseName = Path.GetFileNameWithoutExtension(dllFileName);
        try
        {
            foreach (ProcessModule module in Process.GetCurrentProcess().Modules)
            {
                // ModuleName обычно с ".dll"; сравниваем и с именем файла, и с baseName.
                var moduleBase = Path.GetFileNameWithoutExtension(module.ModuleName);
                if ((string.Equals(module.ModuleName, dllFileName, StringComparison.OrdinalIgnoreCase)
                        || string.Equals(moduleBase, baseName, StringComparison.OrdinalIgnoreCase))
                    && !string.IsNullOrWhiteSpace(module.FileName)
                    && File.Exists(module.FileName))
                    return module.FileName;
            }
        }
        catch
        {
            /* ignore */
        }

        return PlatformPaths.ResolvePlatformModuleNative(dllFileName);
    }

    private static string? TryReadVersionString(string path) =>
        TryReadStringValue(path, "FileVersion") ?? TryReadStringValue(path, "ProductVersion");

    private static string? TryReadStringValue(string path, string valueName)
    {
        var size = GetFileVersionInfoSize(path, out _);
        if (size == 0)
            return null;

        var buffer = Marshal.AllocHGlobal((int)size);
        try
        {
            if (!GetFileVersionInfo(path, 0, size, buffer))
                return null;

            foreach (var subBlock in new[]
                     {
                         $@"\StringFileInfo\040904b0\{valueName}",
                         $@"\StringFileInfo\040904B0\{valueName}",
                         $@"\StringFileInfo\040904E4\0419\{valueName}",
                     })
            {
                if (VerQueryValue(buffer, subBlock, out var valuePtr, out var len) && len > 1)
                {
                    var value = Marshal.PtrToStringUni(valuePtr)?.Trim();
                    if (!string.IsNullOrWhiteSpace(value))
                        return value;
                }
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }

        return null;
    }

    private static string? TryReadFixedFileVersion(string path)
    {
        var size = GetFileVersionInfoSize(path, out _);
        if (size == 0)
            return null;

        var buffer = Marshal.AllocHGlobal((int)size);
        try
        {
            if (!GetFileVersionInfo(path, 0, size, buffer))
                return null;

            if (!VerQueryValue(buffer, "\\", out var infoPtr, out var len) || len == 0)
                return null;

            var info = Marshal.PtrToStructure<VS_FIXEDFILEINFO>(infoPtr);
            if (info.dwSignature != 0xFEEF04BD)
                return null;

            return $"{info.dwFileVersionMS >> 16}.{info.dwFileVersionMS & 0xFFFF}." +
                   $"{info.dwFileVersionLS >> 16}.{info.dwFileVersionLS & 0xFFFF}";
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    [DllImport("version.dll", CharSet = CharSet.Unicode)]
    private static extern uint GetFileVersionInfoSize(string lptstrFilename, out uint lpdwHandle);

    [DllImport("version.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetFileVersionInfo(string lptstrFilename, uint dwHandle, uint dwLen, IntPtr lpData);

    [DllImport("version.dll", CharSet = CharSet.Unicode)]
    private static extern bool VerQueryValue(IntPtr pBlock, string lpSubBlock, out IntPtr lplpBuffer, out uint puLen);

    [StructLayout(LayoutKind.Sequential)]
    private struct VS_FIXEDFILEINFO
    {
        public uint dwSignature;
        public uint dwStrucVersion;
        public uint dwFileVersionMS;
        public uint dwFileVersionLS;
        public uint dwProductVersionMS;
        public uint dwProductVersionLS;
        public uint dwFileFlagsMask;
        public uint dwFileFlags;
        public uint dwFileOS;
        public uint dwFileType;
        public uint dwFileSubtype;
        public uint dwFileDateMS;
        public uint dwFileDateLS;
    }
}

using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>Win32: пути из реестра/API и извлечение HICON (донор BWM ListViewOperation / TreeViewOperation).</summary>
internal static class NativeShellIconInterop
{
    private const uint ShgfiIcon = 0x000000100;
    private const uint ShgfiLargeIcon = 0x000000000;
    private const uint ShgfiUseFileAttributes = 0x000000010;
    private const uint FileAttributeNormal = 0x00000080;

    private const uint AssocfOpenByExename = 0x00000002;
    private const int AssocstrDefaultIcon = 15;
    private const int AssocstrExecutable = 2;

    private const string AppCompatStoreKey =
        @"SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Compatibility Assistant\Store";

    private const string MuiCacheKey =
        @"Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache";

    private const string AppPathsKey =
        @"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths";

    private const string FakeExeFileName = "_.exe";

    private static readonly string[] MuiCachePathSuffixes =
    [
        ".FriendlyAppName",
        ".ApplicationCompany",
    ];

    private static readonly object PathCacheLock = new();
    private static Dictionary<string, string>? _muiCacheByFileName;
    private static Dictionary<string, string>? _appCompatByFileName;
    private static bool _pathCachesWarmed;

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

    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool PathFindOnPathW([In, Out] StringBuilder pszFile, string[]? ppszOtherDirs);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SHGetFileInfo(
        string pszPath,
        uint dwFileAttributes,
        ref Shfileinfo psfi,
        uint cbFileInfo,
        uint uFlags);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr ExtractAssociatedIcon(IntPtr hInst, StringBuilder lpIconPath, ref ushort lpiIcon);

    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode, EntryPoint = "AssocQueryStringW", ExactSpelling = true, PreserveSig = true)]
    private static extern int AssocQueryString(
        uint flags,
        int str,
        string pszAssoc,
        string? pszExtra,
        StringBuilder? pszOut,
        ref uint pcchOut);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool DestroyIcon(IntPtr hIcon);

    /// <summary>Индекс MuiCache/AppCompat только для имён из AttachList; ранний выход.</summary>
    public static void WarmPathCaches(IReadOnlyCollection<string> exeNames)
    {
        var wanted = new HashSet<string>(
            exeNames.Select(static n => Path.GetFileName(n)).Where(static n => !string.IsNullOrEmpty(n)),
            StringComparer.OrdinalIgnoreCase);

        if (wanted.Count == 0)
            return;

        lock (PathCacheLock)
        {
            if (_pathCachesWarmed)
                return;

            _muiCacheByFileName = BuildMuiCacheIndex(wanted);
            _appCompatByFileName = BuildAppCompatIndex(wanted);
            _pathCachesWarmed = true;
        }
    }

    public static void InvalidatePathCaches()
    {
        lock (PathCacheLock)
        {
            _muiCacheByFileName = null;
            _appCompatByFileName = null;
            _pathCachesWarmed = false;
        }
    }

    /// <summary>Зарегистрировать известный полный путь exe в in-memory индексах и HKCU MuiCache/AppCompat.</summary>
    internal static void EnsureRegistryPathCacheEntry(string fullPath)
    {
        if (string.IsNullOrWhiteSpace(fullPath))
            return;

        string normalizedPath;
        try
        {
            normalizedPath = Path.GetFullPath(fullPath);
        }
        catch
        {
            return;
        }

        if (!File.Exists(normalizedPath))
            return;

        var fileName = Path.GetFileName(normalizedPath);
        if (string.IsNullOrEmpty(fileName))
            return;

        lock (PathCacheLock)
        {
            EnsurePathCaches();

            if (IsKnownPathForFileName(fileName, normalizedPath))
                return;

            _muiCacheByFileName![fileName] = normalizedPath;
            _appCompatByFileName![fileName] = normalizedPath;
        }

        EnsureMuiCacheFriendlyAppName(normalizedPath);
        EnsureAppCompatStoreEntry(normalizedPath);
    }

    private static bool IsKnownPathForFileName(string fileName, string normalizedPath)
    {
        if (_muiCacheByFileName!.TryGetValue(fileName, out var fromMui)
            && PathsEqual(fromMui, normalizedPath))
            return true;

        if (_appCompatByFileName!.TryGetValue(fileName, out var fromStore)
            && PathsEqual(fromStore, normalizedPath))
            return true;

        var lazyMui = ResolvePathFromMuiCacheDirect(fileName);
        if (lazyMui is not null && PathsEqual(lazyMui, normalizedPath))
        {
            _muiCacheByFileName[fileName] = lazyMui;
            return true;
        }

        var lazyStore = ResolvePathFromAppCompatStoreDirect(fileName);
        if (lazyStore is not null && PathsEqual(lazyStore, normalizedPath))
        {
            _appCompatByFileName[fileName] = lazyStore;
            return true;
        }

        return false;
    }

    private static bool PathsEqual(string left, string right)
    {
        try
        {
            return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return string.Equals(left, right, StringComparison.OrdinalIgnoreCase);
        }
    }

    private static void EnsureMuiCacheFriendlyAppName(string fullPath)
    {
        try
        {
            var valueName = fullPath + ".FriendlyAppName";
            using var key = Registry.CurrentUser.OpenSubKey(MuiCacheKey, writable: true)
                            ?? Registry.CurrentUser.CreateSubKey(MuiCacheKey);
            if (key is null)
                return;

            foreach (var existing in key.GetValueNames())
            {
                if (string.Equals(existing, valueName, StringComparison.OrdinalIgnoreCase))
                    return;
            }

            key.SetValue(valueName, ResolveFriendlyAppName(fullPath), RegistryValueKind.String);
        }
        catch
        {
            // MuiCache недоступен
        }
    }

    private static void EnsureAppCompatStoreEntry(string fullPath)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(AppCompatStoreKey, writable: true)
                            ?? Registry.CurrentUser.CreateSubKey(AppCompatStoreKey);
            if (key is null)
                return;

            foreach (var existing in key.GetValueNames())
            {
                if (string.Equals(existing, fullPath, StringComparison.OrdinalIgnoreCase))
                    return;
            }

            key.SetValue(fullPath, string.Empty, RegistryValueKind.String);
        }
        catch
        {
            // AppCompat Store недоступен
        }
    }

    private static string ResolveFriendlyAppName(string fullPath)
    {
        try
        {
            var info = FileVersionInfo.GetVersionInfo(fullPath);
            if (!string.IsNullOrWhiteSpace(info.FileDescription))
                return info.FileDescription;
            if (!string.IsNullOrWhiteSpace(info.ProductName))
                return info.ProductName;
        }
        catch
        {
            // метаданные недоступны
        }

        return Path.GetFileNameWithoutExtension(fullPath);
    }

    public static string? ResolveExecutablePath(string exeName)
    {
        if (string.IsNullOrWhiteSpace(exeName))
            return null;

        var fileName = Path.GetFileName(exeName);
        if (string.IsNullOrEmpty(fileName))
            return null;

        if (Path.IsPathRooted(exeName) && File.Exists(exeName))
            return Path.GetFullPath(exeName);

        EnsurePathCaches();

        var fromAppPaths = ResolvePathFromAppPaths(fileName);
        if (fromAppPaths is not null && File.Exists(fromAppPaths))
            return fromAppPaths;

        var fromAssoc = ResolvePathFromAssociationExecutable(fileName);
        if (fromAssoc is not null && File.Exists(fromAssoc))
            return fromAssoc;

        if (_muiCacheByFileName!.TryGetValue(fileName, out var fromMui) && File.Exists(fromMui))
            return fromMui;

        if (_appCompatByFileName!.TryGetValue(fileName, out var fromStore) && File.Exists(fromStore))
            return fromStore;

        var lazyStore = ResolvePathFromAppCompatStoreDirect(fileName);
        if (lazyStore is not null && File.Exists(lazyStore))
        {
            _appCompatByFileName[fileName] = lazyStore;
            return lazyStore;
        }

        var lazyMui = ResolvePathFromMuiCacheDirect(fileName);
        if (lazyMui is not null && File.Exists(lazyMui))
        {
            _muiCacheByFileName[fileName] = lazyMui;
            return lazyMui;
        }

        var search = new StringBuilder(fileName);
        if (PathFindOnPathW(search, null) && File.Exists(search.ToString()))
            return search.ToString();

        var systemRoot = Environment.GetFolderPath(Environment.SpecialFolder.System);
        var system32 = Path.Combine(systemRoot, fileName);
        if (File.Exists(system32))
            return system32;

        var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        var sysWow64 = Path.Combine(windows, "SysWOW64", fileName);
        if (File.Exists(sysWow64))
            return sysWow64;

        var ime = Path.Combine(systemRoot, "IME", "IMEJP", fileName);
        if (File.Exists(ime))
            return ime;

        return null;
    }

    public static IntPtr GetIconHandle(string fileName, out bool mustDestroy)
    {
        var icon = GetExecutableIconHandle(fileName, out mustDestroy);
        if (icon != IntPtr.Zero)
            return icon;

        mustDestroy = true;
        return GetRegisteredFileTypeIconFromFileName(fileName);
    }

    /// <summary>Иконка файла по полному пути — SHGetFileInfo (как в проводнике), затем ассоциация по расширению.</summary>
    public static IntPtr GetFileIconHandle(string fullPath, out bool mustDestroy)
    {
        mustDestroy = true;
        if (string.IsNullOrWhiteSpace(fullPath))
            return IntPtr.Zero;

        var fromPath = TryShGetFileInfo(fullPath, 0);
        if (fromPath != IntPtr.Zero)
            return fromPath;

        return GetRegisteredFileTypeIconFromFileName(Path.GetFileName(fullPath));
    }

    /// <summary>Иконка конкретного файла — только из найденного пути, без подмены ассоциацией по имени.</summary>
    public static IntPtr GetExecutableIconHandle(string fileName, out bool mustDestroy)
    {
        mustDestroy = true;
        var path = ResolveExecutablePath(fileName);
        if (path is null)
            return IntPtr.Zero;

        var fromPath = TryExtractFromExistingFile(path);
        if (fromPath != IntPtr.Zero)
            return fromPath;

        return TryExtractFromSiblingFiles(path);
    }

    public static IntPtr GetRegisteredFileTypeIconFromFileName(string fileName)
    {
        var extension = GetNormalizedExtension(fileName);
        return GetRegisteredFileTypeIcon(extension);
    }

    public static IntPtr GetRegisteredFileTypeIcon(string extension)
    {
        if (string.IsNullOrWhiteSpace(extension))
            return GetRegisteredExtensionIconHandle(".exe");

        var ext = extension.Trim();
        if (!ext.StartsWith('.'))
            ext = "." + ext;

        if (string.Equals(ext, ".exe", StringComparison.OrdinalIgnoreCase))
            return GetRegisteredExeTypeIconHandle();

        return GetRegisteredExtensionIconHandle(ext);
    }

    /// <summary>Иконка зарегистрированного типа .exe — как у безымянного exe в проводнике.</summary>
    public static IntPtr GetRegisteredExeTypeIconHandle()
    {
        var fromFakeExe = TryShGetFileInfo(FakeExeFileName, FileAttributeNormal);
        if (fromFakeExe != IntPtr.Zero)
            return fromFakeExe;

        var defaultIcon = QueryAssocString(".exe", AssocstrDefaultIcon, 0)
                          ?? QueryAssocString("exefile", AssocstrDefaultIcon, 0);
        if (defaultIcon is not null)
        {
            var hIcon = ExtractIconFromAssociationString(defaultIcon);
            if (hIcon != IntPtr.Zero)
                return hIcon;
        }

        return IntPtr.Zero;
    }

    public static string GetNormalizedExtension(string fileName)
    {
        var ext = Path.GetExtension(Path.GetFileName(fileName));
        return string.IsNullOrEmpty(ext) ? ".exe" : ext;
    }

    public static void ReleaseIcon(IntPtr hIcon, bool mustDestroy)
    {
        if (mustDestroy && hIcon != IntPtr.Zero)
            DestroyIcon(hIcon);
    }

    private static void EnsurePathCaches()
    {
        if (_pathCachesWarmed)
            return;

        lock (PathCacheLock)
        {
            if (_pathCachesWarmed)
                return;

            _muiCacheByFileName = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            _appCompatByFileName = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            _pathCachesWarmed = true;
        }
    }

    private static string? ResolvePathFromMuiCacheDirect(string fileName)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(MuiCacheKey, false);
            if (key is null)
                return null;

            foreach (var valueName in key.GetValueNames())
            {
                if (TryParseMuiCacheValueName(valueName) is not { } path)
                    continue;

                if (!string.Equals(Path.GetFileName(path), fileName, StringComparison.OrdinalIgnoreCase))
                    continue;

                return path;
            }
        }
        catch
        {
            // MuiCache недоступен
        }

        return null;
    }

    private static Dictionary<string, string> BuildMuiCacheIndex(HashSet<string> wantedFileNames)
    {
        var dict = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var remaining = new HashSet<string>(wantedFileNames, StringComparer.OrdinalIgnoreCase);

        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(MuiCacheKey, false);
            if (key is null)
                return dict;

            foreach (var valueName in key.GetValueNames())
            {
                if (remaining.Count == 0)
                    break;

                if (TryParseMuiCacheValueName(valueName) is not { } path)
                    continue;

                var name = Path.GetFileName(path);
                if (string.IsNullOrEmpty(name) || !remaining.Contains(name))
                    continue;

                dict[name] = path;
                remaining.Remove(name);
            }
        }
        catch
        {
            // MuiCache недоступен
        }

        return dict;
    }

    private static Dictionary<string, string> BuildAppCompatIndex(HashSet<string> wantedFileNames)
    {
        var dict = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var remaining = new HashSet<string>(wantedFileNames, StringComparer.OrdinalIgnoreCase);

        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(AppCompatStoreKey, false);
            if (key is null)
                return dict;

            foreach (var valueName in key.GetValueNames())
            {
                if (remaining.Count == 0)
                    break;

                var path = TryParseAppCompatStorePath(valueName, key.GetValue(valueName));
                if (path is null)
                    continue;

                var name = Path.GetFileName(path);
                if (string.IsNullOrEmpty(name) || !remaining.Contains(name))
                    continue;

                dict[name] = path;
                remaining.Remove(name);
            }
        }
        catch
        {
            // Store недоступен
        }

        return dict;
    }

    /// <summary>AppCompat Store (HKCU = HKEY_USERS\SID текущего пользователя): путь в имени значения или REG_SZ.</summary>
    private static string? ResolvePathFromAppCompatStoreDirect(string fileName)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(AppCompatStoreKey, false);
            if (key is null)
                return null;

            foreach (var valueName in key.GetValueNames())
            {
                var path = TryParseAppCompatStorePath(valueName, key.GetValue(valueName));
                if (path is null)
                    continue;

                if (!string.Equals(Path.GetFileName(path), fileName, StringComparison.OrdinalIgnoreCase))
                    continue;

                return path;
            }
        }
        catch
        {
            // Store недоступен
        }

        return null;
    }

    private static string? TryParseAppCompatStorePath(string valueName, object? value)
    {
        var fromName = TryNormalizeRegistryFilePath(valueName);
        if (fromName is not null)
            return fromName;

        if (value is string text && !string.IsNullOrWhiteSpace(text))
            return TryNormalizeRegistryFilePath(text);

        return null;
    }

    private static string? TryNormalizeRegistryFilePath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return null;

        path = Environment.ExpandEnvironmentVariables(path.Trim('"').Trim());
        if (path.Length < 4)
            return null;

        if (!Path.IsPathRooted(path))
            return null;

        return path;
    }

    private static string? TryParseMuiCacheValueName(string valueName)
    {
        foreach (var suffix in MuiCachePathSuffixes)
        {
            if (!valueName.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                continue;

            var path = valueName[..^suffix.Length];
            return string.IsNullOrWhiteSpace(path) ? null : path;
        }

        return null;
    }

    private static string? ResolvePathFromAppPaths(string fileName)
    {
        foreach (var hive in new[] { Registry.LocalMachine, Registry.CurrentUser })
        {
            try
            {
                using var key = hive.OpenSubKey(Path.Combine(AppPathsKey, fileName), false);
                if (key?.GetValue(null) is not string path || string.IsNullOrWhiteSpace(path))
                    continue;

                path = Environment.ExpandEnvironmentVariables(path.Trim('"'));
                return path;
            }
            catch
            {
                // App Paths недоступен
            }
        }

        return null;
    }

    private static string? ResolvePathFromAssociationExecutable(string fileName)
    {
        var executable = QueryAssocString(fileName, AssocstrExecutable, AssocfOpenByExename);
        if (executable is null)
            return null;

        return Environment.ExpandEnvironmentVariables(executable.Trim('"'));
    }

    private static IntPtr GetRegisteredExtensionIconHandle(string extension)
    {
        if (string.IsNullOrWhiteSpace(extension))
            return IntPtr.Zero;

        var ext = extension.Trim();
        if (!ext.StartsWith('.'))
            ext = "." + ext;

        var fakeName = "_" + ext;
        var fromFake = TryShGetFileInfo(fakeName, FileAttributeNormal);
        if (fromFake != IntPtr.Zero)
            return fromFake;

        var defaultIcon = QueryAssocString(ext, AssocstrDefaultIcon, 0);
        if (defaultIcon is not null)
        {
            var hIcon = ExtractIconFromAssociationString(defaultIcon);
            if (hIcon != IntPtr.Zero)
                return hIcon;
        }

        return IntPtr.Zero;
    }

    private static IntPtr TryExtractFromExistingFile(string path)
    {
        if (!File.Exists(path))
            return IntPtr.Zero;

        var fromAssoc = ExtractIconFromPath(path);
        if (fromAssoc != IntPtr.Zero)
            return fromAssoc;

        return TryShGetFileInfo(path, 0);
    }

    private static IntPtr TryExtractFromSiblingFiles(string path)
    {
        try
        {
            var directory = Path.GetDirectoryName(path);
            if (string.IsNullOrEmpty(directory) || !Directory.Exists(directory))
                return IntPtr.Zero;

            foreach (var pattern in new[] { "*.ico", "*.exe" })
            {
                foreach (var candidate in Directory.EnumerateFiles(directory, pattern))
                {
                    if (!File.Exists(candidate))
                        continue;

                    var icon = ExtractIconFromPath(candidate);
                    if (icon != IntPtr.Zero)
                        return icon;
                }
            }
        }
        catch
        {
            // каталог недоступен
        }

        return IntPtr.Zero;
    }

    private static IntPtr TryShGetFileInfo(string path, uint fileAttributes)
    {
        var shInfo = new Shfileinfo();
        var flags = ShgfiIcon | ShgfiLargeIcon;
        if (fileAttributes != 0)
            flags |= ShgfiUseFileAttributes;

        var result = SHGetFileInfo(path, fileAttributes, ref shInfo, (uint)Marshal.SizeOf<Shfileinfo>(), flags);
        return result != IntPtr.Zero && shInfo.hIcon != IntPtr.Zero ? shInfo.hIcon : IntPtr.Zero;
    }

    private static IntPtr ExtractIconFromPath(string path)
    {
        var sb = new StringBuilder(path);
        ushort index = 0;
        return ExtractAssociatedIcon(IntPtr.Zero, sb, ref index);
    }

    private static IntPtr ExtractIconFromAssociationString(string assocString)
    {
        var comma = assocString.LastIndexOf(',');
        if (comma >= 0)
        {
            var filePart = assocString[..comma].Trim();
            var indexPart = assocString[(comma + 1)..].Trim();
            if (int.TryParse(indexPart, out var index))
            {
                filePart = Environment.ExpandEnvironmentVariables(filePart);
                if (File.Exists(filePart))
                {
                    var sb = new StringBuilder(filePart);
                    ushort iconIndex = (ushort)Math.Max(0, index);
                    var hIcon = ExtractAssociatedIcon(IntPtr.Zero, sb, ref iconIndex);
                    if (hIcon != IntPtr.Zero)
                        return hIcon;
                }
            }
        }

        if (File.Exists(assocString))
            return ExtractIconFromPath(assocString);

        return IntPtr.Zero;
    }

    private static string? QueryAssocString(string assoc, int assocStr, uint flags)
    {
        try
        {
            uint length = 0;
            var hr = AssocQueryString(flags, assocStr, assoc, null, null, ref length);
            if (hr < 0 || length == 0)
                return null;

            var sb = new StringBuilder((int)length);
            hr = AssocQueryString(flags, assocStr, assoc, null, sb, ref length);
            if (hr < 0)
                return null;

            return sb.ToString();
        }
        catch
        {
            return null;
        }
    }
}

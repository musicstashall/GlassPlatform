using System.Globalization;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>
/// Каталог минидампов: resolve папок, разбор имён, enumerate/delete.
/// Формат имени (WER LocalDumps / CrashGuard): {exe}.{pid}.dmp
/// Дата/время — из метаданных ФС (CreationTime).
/// </summary>
public static class CrashDumpCatalog
{
    private const string DefaultPlatformFolderTpl = @"%LOCALAPPDATA%\GlassPlatform\Crashes";
    private const string DefaultSystemFolderTpl = @"%LOCALAPPDATA%\CrashDumps";

    private static readonly string[] DumpFolderRegPaths =
    [
        @"Software\StanislavKa\BlackWindowManager\Crashes",
        @"Software\StanislavKa\BlackWindowManager",
        @"Software\Glass\CrashGuard",
    ];

    /// <summary>Папка дампов платформы (CrashGuard / DumpFolder).</summary>
    public static string ResolvePlatformFolder()
    {
        foreach (var path in DumpFolderRegPaths)
        {
            if (TryReadDumpFolder(path, out var folder) && !string.IsNullOrWhiteSpace(folder))
                return folder;
        }

        return Environment.ExpandEnvironmentVariables(DefaultPlatformFolderTpl);
    }

    /// <summary>Системная папка WER LocalDumps.</summary>
    public static string ResolveSystemFolder() =>
        Environment.ExpandEnvironmentVariables(DefaultSystemFolderTpl);

    /// <summary>Совместимость: то же, что <see cref="ResolvePlatformFolder"/>.</summary>
    public static string ResolveFolder() => ResolvePlatformFolder();

    public static IReadOnlyList<CrashDumpEntry> Enumerate() =>
        Enumerate(ResolvePlatformFolder());

    public static IReadOnlyList<CrashDumpEntry> Enumerate(string folder)
    {
        if (string.IsNullOrWhiteSpace(folder) || !Directory.Exists(folder))
            return Array.Empty<CrashDumpEntry>();

        string[] files;
        try
        {
            files = Directory.GetFiles(folder, "*.dmp", SearchOption.TopDirectoryOnly);
        }
        catch (Exception)
        {
            return Array.Empty<CrashDumpEntry>();
        }

        var list = new List<CrashDumpEntry>(files.Length);
        foreach (var path in files)
        {
            var entry = TryCreateEntry(path);
            if (entry is not null)
                list.Add(entry);
        }

        list.Sort(static (a, b) => b.Timestamp.CompareTo(a.Timestamp));
        return list;
    }

    public static bool DeleteOne(string fullPath)
    {
        if (string.IsNullOrWhiteSpace(fullPath))
            return false;

        try
        {
            if (!File.Exists(fullPath))
                return false;
            File.Delete(fullPath);
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>Удаляет все *.dmp в каталоге дампов платформы. Возвращает число удалённых.</summary>
    public static int DeleteAll() => DeleteAll(ResolvePlatformFolder());

    /// <summary>Удаляет все *.dmp в указанном каталоге. Возвращает число удалённых.</summary>
    public static int DeleteAll(string folder)
    {
        if (string.IsNullOrWhiteSpace(folder) || !Directory.Exists(folder))
            return 0;

        string[] files;
        try
        {
            files = Directory.GetFiles(folder, "*.dmp", SearchOption.TopDirectoryOnly);
        }
        catch (Exception)
        {
            return 0;
        }

        var deleted = 0;
        foreach (var path in files)
        {
            try
            {
                File.Delete(path);
                deleted++;
            }
            catch (Exception)
            {
                // пропускаем занятые/защищённые
            }
        }

        return deleted;
    }

    public static string FormatSize(long bytes)
    {
        var culture = CultureInfo.CurrentCulture;
        const double kb = 1024d;
        const double mb = kb * 1024d;
        const double gb = mb * 1024d;

        if (bytes >= gb)
            return string.Format(culture, "{0:0.##} GB", bytes / gb);
        if (bytes >= mb)
            return string.Format(culture, "{0:0.##} MB", bytes / mb);
        if (bytes >= kb)
            return string.Format(culture, "{0:0.##} KB", bytes / kb);
        return string.Format(culture, "{0} B", bytes);
    }

    /// <summary>
    /// {processName}.{pid}.dmp — последний сегмент после точки = PID, остальное = имя процесса.
    /// Пример: explorer.exe.41104.dmp → process=explorer.exe, pid=41104.
    /// </summary>
    internal static bool TryParseFileName(
        string fileName,
        out string processName,
        out uint pid)
    {
        processName = string.Empty;
        pid = 0;

        if (string.IsNullOrWhiteSpace(fileName))
            return false;

        var trimmed = fileName.Trim();
        if (trimmed.EndsWith(".dmp", StringComparison.OrdinalIgnoreCase))
            trimmed = trimmed[..^4];

        var lastDot = trimmed.LastIndexOf('.');
        if (lastDot <= 0 || lastDot >= trimmed.Length - 1)
            return false;

        var pidPart = trimmed[(lastDot + 1)..];
        if (!uint.TryParse(pidPart, NumberStyles.Integer, CultureInfo.InvariantCulture, out pid))
            return false;

        processName = trimmed[..lastDot];
        return processName.Length > 0;
    }

    private static CrashDumpEntry? TryCreateEntry(string fullPath)
    {
        FileInfo info;
        try
        {
            info = new FileInfo(fullPath);
            if (!info.Exists)
                return null;
        }
        catch (Exception)
        {
            return null;
        }

        var fileName = info.Name;
        var timestamp = PreferCreationTime(info);
        string processName;
        uint? pid = null;

        if (TryParseFileName(fileName, out var parsedProcess, out var parsedPid))
        {
            processName = parsedProcess;
            pid = parsedPid;
        }
        else
        {
            // Нестандартное имя: процесс = имя без расширения, PID неизвестен.
            processName = Path.GetFileNameWithoutExtension(fileName);
            if (string.IsNullOrEmpty(processName))
                processName = fileName;
        }

        var displayName = processName.EndsWith(".dmp", StringComparison.OrdinalIgnoreCase)
            ? processName
            : processName + ".dmp";

        return new CrashDumpEntry
        {
            FullPath = fullPath,
            FileName = fileName,
            DisplayName = displayName,
            ProcessName = processName,
            Timestamp = timestamp,
            Pid = pid,
            SizeBytes = info.Length,
            DateText = timestamp.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.CurrentCulture),
            SizeText = FormatSize(info.Length),
            PidText = pid is uint p ? p.ToString(CultureInfo.CurrentCulture) : "—",
            TickText = "—",
        };
    }

    private static DateTime PreferCreationTime(FileInfo info)
    {
        try
        {
            var created = info.CreationTime;
            if (created > DateTime.MinValue)
                return created;
        }
        catch (Exception)
        {
            // ignore
        }

        try
        {
            return info.LastWriteTime;
        }
        catch (Exception)
        {
            return DateTime.Now;
        }
    }

    private static bool TryReadDumpFolder(string regPath, out string folder)
    {
        folder = string.Empty;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(regPath, writable: false);
            var rawObj = key?.GetValue("DumpFolder", null, RegistryValueOptions.DoNotExpandEnvironmentNames);
            if (rawObj is not string raw || string.IsNullOrWhiteSpace(raw))
                return false;

            var expanded = Environment.ExpandEnvironmentVariables(raw.Trim());
            if (string.IsNullOrWhiteSpace(expanded))
                return false;

            folder = expanded.TrimEnd('\\', '/');
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }
}

public sealed class CrashDumpEntry
{
    public required string FullPath { get; init; }
    public required string FileName { get; init; }
    /// <summary>Имя для UI: {упавший.exe}.dmp</summary>
    public required string DisplayName { get; init; }
    public required string ProcessName { get; init; }
    public DateTime Timestamp { get; init; }
    public uint? Pid { get; init; }
    public long SizeBytes { get; init; }
    public required string DateText { get; init; }
    public required string SizeText { get; init; }
    public required string PidText { get; init; }
    public required string TickText { get; init; }
}

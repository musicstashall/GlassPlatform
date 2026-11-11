using System.Text;

namespace GlassSettings.Services;

/// <summary>Очередь команды «открыть резервную копию» для уже запущенного или стартующего окна.</summary>
public static class SettingsBackupOpenRequest
{
    private static string? _path;

    public static void Set(string path)
    {
        if (!string.IsNullOrWhiteSpace(path))
            _path = path;
    }

    public static bool TryPeek(out string path)
    {
        path = _path ?? "";
        return _path is not null;
    }

    public static bool TryConsume(out string path)
    {
        path = _path ?? "";
        if (_path is null)
            return false;
        _path = null;
        return true;
    }

    public static void Clear() => _path = null;

    public static string? FromCommandLine()
    {
        var args = Environment.GetCommandLineArgs();
        for (var i = 1; i < args.Length; i++)
        {
            var candidate = args[i].Trim().Trim('"');
            if (candidate.EndsWith(SettingsRegistryBackupService.FileExtension, StringComparison.OrdinalIgnoreCase)
                && File.Exists(candidate))
                return Path.GetFullPath(candidate);
        }

        return null;
    }

    public static string MapName =>
        Environment.Is64BitProcess
            ? @"Local\GlassSettings.BackupOpen.x64"
            : @"Local\GlassSettings.BackupOpen.x86";

    /// <summary>Второй экземпляр кладёт путь туда, откуда первое окно прочитает его вместе с WM_COPYDATA.</summary>
    public static void Publish(string path)
    {
        var bytes = Encoding.Unicode.GetBytes(path);
        using var map = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateOrOpen(MapName, 4096);
        using var view = map.CreateViewAccessor(0, 4096);
        view.Write(0, bytes.Length);
        view.WriteArray(4, bytes, 0, bytes.Length);
    }

    public static string? TryReadPublished()
    {
        try
        {
            using var map = System.IO.MemoryMappedFiles.MemoryMappedFile.OpenExisting(MapName);
            using var view = map.CreateViewAccessor(0, 4096, System.IO.MemoryMappedFiles.MemoryMappedFileAccess.Read);
            var length = view.ReadInt32(0);
            if (length <= 0 || length > 4000 || (length % 2) != 0)
                return null;
            var bytes = new byte[length];
            view.ReadArray(4, bytes, 0, length);
            var path = Encoding.Unicode.GetString(bytes).TrimEnd('\0');
            return path.EndsWith(SettingsRegistryBackupService.FileExtension, StringComparison.OrdinalIgnoreCase)
                && File.Exists(path)
                ? path
                : null;
        }
        catch (FileNotFoundException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }
}

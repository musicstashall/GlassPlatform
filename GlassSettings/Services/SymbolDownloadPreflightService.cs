using GlassSettings.Core.Platform;

namespace GlassSettings.Services;

/// <summary>
/// Preflight: наличие dbghelp.dll и symsrv.dll в каталоге OpenGlass или System32.
/// </summary>
public static class SymbolDownloadPreflightService
{
    private static readonly string[] RequiredDlls = ["dbghelp.dll", "symsrv.dll"];

    public static bool TryEnsureSymbolTools(out IReadOnlyList<string> missing)
    {
        foreach (var directory in EnumerateSearchDirectories())
        {
            if (HasCompletePair(directory, out _))
            {
                missing = Array.Empty<string>();
                return true;
            }
        }

        missing = GetMissingFromBestDirectory();
        return false;
    }

    /// <summary>Каталог, где найдена полная пара dbghelp.dll + symsrv.dll.</summary>
    public static bool TryGetResolvedToolsDirectory(out string? directory)
    {
        foreach (var candidate in EnumerateSearchDirectories())
        {
            if (HasCompletePair(candidate, out _))
            {
                directory = candidate;
                return true;
            }
        }

        directory = null;
        return false;
    }

    private static IEnumerable<string> EnumerateSearchDirectories()
    {
        var dllPath = PlatformPaths.OpenGlassDllPath;
        if (!string.IsNullOrWhiteSpace(dllPath))
        {
            var dllDir = Path.GetDirectoryName(dllPath);
            if (!string.IsNullOrWhiteSpace(dllDir))
                yield return dllDir;
        }

        var systemDir = Environment.GetFolderPath(Environment.SpecialFolder.System);
        if (!string.IsNullOrWhiteSpace(systemDir))
            yield return systemDir;
    }

    private static bool HasCompletePair(string directory, out IReadOnlyList<string> missing)
    {
        var localMissing = new List<string>(RequiredDlls.Length);
        foreach (var dll in RequiredDlls)
        {
            if (!File.Exists(Path.Combine(directory, dll)))
                localMissing.Add(dll);
        }

        missing = localMissing;
        return localMissing.Count == 0;
    }

    private static IReadOnlyList<string> GetMissingFromBestDirectory()
    {
        foreach (var directory in EnumerateSearchDirectories())
        {
            if (HasCompletePair(directory, out var missing) || missing.Count > 0)
                return missing;
        }

        return RequiredDlls;
    }
}

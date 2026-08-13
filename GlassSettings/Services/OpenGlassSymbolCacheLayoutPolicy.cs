namespace GlassSettings.Services;

/// <summary>
/// Раскладка кэша PDB на диске зависит от версии OpenGlass.
/// Порог 2.5.0 — provisional до подтверждения у maintainer OpenGlass.
/// </summary>
public enum OpenGlassSymbolCacheLayoutKind
{
    Flat = 0,
    SymSrvNested = 1,
}

public static class OpenGlassSymbolCacheLayoutPolicy
{
    private static readonly Version FlatLayoutMinimumVersion = new(2, 5, 0);

    public static OpenGlassSymbolCacheLayoutKind ResolveLayout(string? installedVersion)
    {
        if (!TryParseVersion(installedVersion, out var version))
            return OpenGlassSymbolCacheLayoutKind.Flat;

        return version >= FlatLayoutMinimumVersion
            ? OpenGlassSymbolCacheLayoutKind.Flat
            : OpenGlassSymbolCacheLayoutKind.SymSrvNested;
    }

    private static bool TryParseVersion(string? text, out Version version)
    {
        version = new Version(0, 0);
        if (string.IsNullOrWhiteSpace(text))
            return false;

        var normalized = text.Trim();
        var plusIndex = normalized.IndexOf('+');
        if (plusIndex >= 0)
            normalized = normalized[..plusIndex];

        var dashIndex = normalized.IndexOf('-');
        if (dashIndex >= 0)
            normalized = normalized[..dashIndex];

        var parts = normalized.Split('.', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 0)
            return false;

        if (parts.Length >= 4
            && int.TryParse(parts[0], out var major)
            && int.TryParse(parts[1], out var minor)
            && int.TryParse(parts[2], out var build)
            && int.TryParse(parts[3], out var revision))
        {
            version = new Version(major, minor, build, revision);
            return true;
        }

        if (!Version.TryParse(normalized, out var parsed) || parsed is null)
            return false;

        version = parsed;
        return true;
    }
}

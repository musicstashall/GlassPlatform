namespace GlassSettings.Services;

/// <summary>Stable source identity across the former Dark/Lite resource pairs.</summary>
public static class ReflectionSourceNaming
{
    public static string Stem(string path)
    {
        var name = Path.GetFileNameWithoutExtension(path);
        foreach (var suffix in new[] { "_Dark", "_Lite" })
            if (name.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                name = name[..^suffix.Length];
        return name.ToLowerInvariant() switch
        {
            "glassreflection_01" => "FrostWeave",
            "glassreflection_02" => "DiagonalWeave",
            _ => name
        };
    }
}

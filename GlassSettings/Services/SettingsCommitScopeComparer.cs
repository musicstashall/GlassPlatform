using GlassSettings.Core.Snapshot;

namespace GlassSettings.Services;

/// <summary>Сравнение scope для флагов вне реестра.</summary>
internal sealed class SettingsCommitScopeComparer : IEqualityComparer<SettingsCommitScope>
{
    public static SettingsCommitScopeComparer Instance { get; } = new();

    public bool Equals(SettingsCommitScope x, SettingsCommitScope y)
    {
        if (x.Kind != y.Kind)
            return false;

        var sameHive = x.DwmHive == y.DwmHive;
        return x.Kind switch
        {
            SettingsCommitScopeKind.Area => x.Area == y.Area
                                           && (!IsDwmArea(x.Area) || sameHive),
            SettingsCommitScopeKind.GlassSubKey =>
                x.GlassSubKeyMatch == y.GlassSubKeyMatch
                && string.Equals(x.GlassSubKey, y.GlassSubKey, StringComparison.OrdinalIgnoreCase)
                && GlassSubKeySetsEqual(x.GlassSubKeys, y.GlassSubKeys),
            SettingsCommitScopeKind.FlyoutParameterGroup =>
                x.FlyoutGroup == y.FlyoutGroup
                && string.Equals(x.GlassSubKey, y.GlassSubKey, StringComparison.OrdinalIgnoreCase),
            SettingsCommitScopeKind.DwmKeySubset =>
                sameHive && DwmKeySetsEqual(x.DwmValueNames, y.DwmValueNames),
            SettingsCommitScopeKind.GlassValueSubset =>
                GlassValueSetsEqual(x.GlassValueNames, y.GlassValueNames),
            SettingsCommitScopeKind.WindowMetricsKeySubset =>
                WindowMetricsKeySetsEqual(x.WindowMetricsValueNames, y.WindowMetricsValueNames),
            SettingsCommitScopeKind.DwmPlatform => sameHive,
            _ => true
        };
    }

    public int GetHashCode(SettingsCommitScope obj) =>
        obj.Kind switch
        {
            SettingsCommitScopeKind.Area => HashCode.Combine(
                obj.Kind,
                obj.Area,
                IsDwmArea(obj.Area) ? obj.DwmHive : null),
            SettingsCommitScopeKind.GlassSubKey => HashCode.Combine(
                obj.Kind,
                obj.GlassSubKeyMatch,
                obj.GlassSubKey?.ToUpperInvariant() ?? string.Empty,
                GlassSubKeySetHash(obj.GlassSubKeys)),
            SettingsCommitScopeKind.FlyoutParameterGroup => HashCode.Combine(obj.Kind,
                obj.FlyoutGroup, obj.GlassSubKey?.ToUpperInvariant() ?? string.Empty),
            SettingsCommitScopeKind.DwmKeySubset => HashCode.Combine(obj.Kind,
                DwmKeySetHash(obj.DwmValueNames),
                obj.DwmHive),
            SettingsCommitScopeKind.GlassValueSubset => HashCode.Combine(obj.Kind,
                GlassValueSetHash(obj.GlassValueNames)),
            SettingsCommitScopeKind.WindowMetricsKeySubset => HashCode.Combine(obj.Kind,
                WindowMetricsKeySetHash(obj.WindowMetricsValueNames)),
            SettingsCommitScopeKind.DwmPlatform => HashCode.Combine(obj.Kind, obj.DwmHive),
            _ => (int)obj.Kind
        };

    private static bool DwmKeySetsEqual(string[]? a, string[]? b)
    {
        if (ReferenceEquals(a, b))
            return true;
        if (a is null || b is null || a.Length != b.Length)
            return false;

        return a.OrderBy(static k => k, StringComparer.OrdinalIgnoreCase)
            .SequenceEqual(b.OrderBy(static k => k, StringComparer.OrdinalIgnoreCase), StringComparer.OrdinalIgnoreCase);
    }

    private static int DwmKeySetHash(string[]? names)
    {
        if (names is null || names.Length == 0)
            return 0;

        var hash = new HashCode();
        foreach (var name in names.OrderBy(static k => k, StringComparer.OrdinalIgnoreCase))
            hash.Add(name, StringComparer.OrdinalIgnoreCase);
        return hash.ToHashCode();
    }

    private static bool GlassSubKeySetsEqual(string[]? a, string[]? b) => DwmKeySetsEqual(a, b);

    private static int GlassSubKeySetHash(string[]? names) => DwmKeySetHash(names);

    private static bool GlassValueSetsEqual(string[]? a, string[]? b)
    {
        if (ReferenceEquals(a, b))
            return true;
        if (a is null || b is null || a.Length != b.Length)
            return false;

        return a.OrderBy(static k => k, StringComparer.OrdinalIgnoreCase)
            .SequenceEqual(b.OrderBy(static k => k, StringComparer.OrdinalIgnoreCase), StringComparer.OrdinalIgnoreCase);
    }

    private static int GlassValueSetHash(string[]? names) => DwmKeySetHash(names);

    private static bool WindowMetricsKeySetsEqual(string[]? a, string[]? b) => DwmKeySetsEqual(a, b);

    private static int WindowMetricsKeySetHash(string[]? names) => DwmKeySetHash(names);

    private static bool IsDwmArea(GlassSettings.Core.Customization.CustomizationAreaId? area) =>
        area is GlassSettings.Core.Customization.CustomizationAreaId.DwmColorization
            or GlassSettings.Core.Customization.CustomizationAreaId.DwmGlass
            or GlassSettings.Core.Customization.CustomizationAreaId.DwmTheme;
}

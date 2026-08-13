using GlassSettings.Core.Customization;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Flyout;
using GlassSettings.Core.Menu;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Text;
using Microsoft.Win32;

namespace GlassSettings.Core.Snapshot;

public sealed class SettingsSnapshot
{
    private readonly Dictionary<GlassRegistryPartition, List<RegEntry>> _glassPartitions = new();
    private readonly Dictionary<DwmSnapshotPartition, List<RegEntry>> _dwmPartitions = new();
    private List<RegEntry> _dwmFullSnapshot = [];
    private Dictionary<string, byte[]> _windowMetricsSnapshot = new(StringComparer.OrdinalIgnoreCase);

    public void Capture()
    {
        foreach (GlassRegistryPartition partition in Enum.GetValues<GlassRegistryPartition>())
            _glassPartitions[partition] = CaptureGlassPartition(partition);

        _dwmFullSnapshot = CaptureAllDwmValues();
        foreach (var partition in DwmSnapshotCatalog.AllPartitions)
            _dwmPartitions[partition] = FilterDwmPartition(_dwmFullSnapshot, partition);

        _windowMetricsSnapshot = CaptureAllWindowMetrics();
    }

    public bool HasUnsavedChanges()
    {
        if (HasAnyDwmDirty())
            return true;

        foreach (GlassRegistryPartition partition in Enum.GetValues<GlassRegistryPartition>())
        {
            if (HasGlassPartitionDirty(partition))
                return true;
        }

        if (HasAnyWindowMetricsDirty())
            return true;

        return false;
    }

    /// <summary>Несохранённые изменения в любой DWM-партиции снимка (semantic, как HasDwmKeysDirty).</summary>
    public bool HasAnyDwmDirty()
    {
        foreach (var partition in DwmSnapshotCatalog.AllPartitions)
        {
            var keys = DwmSnapshotCatalog.GetKeys(partition);
            foreach (var hive in EnumerateDwmHives())
            {
                if (HasDwmKeysDirty(keys, hive))
                    return true;
            }
        }

        return false;
    }

    /// <summary>Несохранённые изменения в BWM-разделе Root.</summary>
    public bool HasRootDirty() => HasGlassPartitionDirty(GlassRegistryPartition.Root);

    /// <summary>Несохранённые изменения в любой BWM-партиции (платформа Glass).</summary>
    public bool HasAnyGlassDirty()
    {
        foreach (GlassRegistryPartition partition in Enum.GetValues<GlassRegistryPartition>())
        {
            if (HasGlassPartitionDirty(partition))
                return true;
        }

        return false;
    }

    /// <summary>REG_BINARY из baseline GeneralRoot; null если значения не было в снимке.</summary>
    public byte[]? TryGetGeneralRootSnapshotBytes(string valueName)
    {
        if (!_glassPartitions.TryGetValue(GlassRegistryPartition.GeneralRoot, out var list))
            return null;

        foreach (var entry in list)
        {
            if (!string.Equals(entry.ValueName, valueName, StringComparison.OrdinalIgnoreCase))
                continue;
            if (!string.Equals(entry.SubKey, RegistryPaths.GlassRoot, StringComparison.OrdinalIgnoreCase))
                continue;
            return entry.Data;
        }

        return null;
    }

    /// <summary>Имена DWM-значений, отличающихся от снимка (до <see cref="Revert"/>).</summary>
    public IReadOnlyList<string> GetDirtyDwmValueNames()
    {
        var dirty = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var hive in EnumerateDwmHives())
        {
            var currentByName = CaptureAllDwmValues(hive)
                .ToDictionary(static e => e.ValueName, static e => e, StringComparer.OrdinalIgnoreCase);
            var snapByName = _dwmFullSnapshot
                .Where(e => e.DwmHive == hive)
                .ToDictionary(static e => e.ValueName, static e => e, StringComparer.OrdinalIgnoreCase);

            foreach (var name in snapByName.Keys.Union(currentByName.Keys, StringComparer.OrdinalIgnoreCase))
            {
                snapByName.TryGetValue(name, out var snapEntry);
                currentByName.TryGetValue(name, out var curEntry);
                if (!DwmDefaults.IsSemanticallyEqual(name, snapEntry, curEntry, EntryEquals))
                    dirty.Add(name);
            }
        }

        return dirty.ToList();
    }

    /// <summary>Для [RegWatch]: semantic-dirty DWM — hive, value, snapshot vs registry.</summary>
    internal IReadOnlyList<string> DescribeSemanticDirtyDwmEntries()
    {
        var lines = new List<string>();
        foreach (var hive in EnumerateDwmHives())
        {
            var hiveLabel = hive == DwmRegistryHive.Hkcu ? "HKCU" : "HKLM";
            var keyPath = $@"{hiveLabel}\{RegistryPaths.DwmRoot}";
            var currentByName = CaptureAllDwmValues(hive)
                .ToDictionary(static e => e.ValueName, static e => e, StringComparer.OrdinalIgnoreCase);
            var snapByName = _dwmFullSnapshot
                .Where(e => e.DwmHive == hive)
                .ToDictionary(static e => e.ValueName, static e => e, StringComparer.OrdinalIgnoreCase);

            foreach (var name in snapByName.Keys.Union(currentByName.Keys, StringComparer.OrdinalIgnoreCase).OrderBy(static n => n, StringComparer.OrdinalIgnoreCase))
            {
                snapByName.TryGetValue(name, out var snapEntry);
                currentByName.TryGetValue(name, out var curEntry);
                if (DwmDefaults.IsSemanticallyEqual(name, snapEntry, curEntry, EntryEquals))
                    continue;

                lines.Add($"{keyPath}\\{name}: snap={FormatDwmEntryForWatch(name, snapEntry)} reg={FormatDwmEntryForWatch(name, curEntry)}");
            }
        }

        return lines;
    }

    /// <summary>Есть ли в baseline-снимке хотя бы одно отслеживаемое значение Menu kind.</summary>
    public bool HasMenuKindSnapshotBaseline(MenuItemKind kind)
    {
        foreach (var parameter in MenuItemRegistry.GetParameterRefs(kind))
        {
            if (FindSnapshotEntry(parameter) is not null)
                return true;
        }

        return false;
    }

    /// <summary>Другие Menu kind с сохранёнными параметрами в baseline (не текущий реестр).</summary>
    public bool HasOtherMenuKindsSnapshotBaseline(MenuItemKind exceptKind)
    {
        foreach (MenuItemKind kind in Enum.GetValues<MenuItemKind>())
        {
            if (kind == exceptKind)
                continue;
            if (HasMenuKindSnapshotBaseline(kind))
                return true;
        }

        return false;
    }

    public bool HasUnsavedInScope(SettingsCommitScope scope)
    {
        if (scope.Kind == SettingsCommitScopeKind.All)
            return HasUnsavedChanges();

        if (scope.Kind is SettingsCommitScopeKind.System or SettingsCommitScopeKind.Processes)
            return false;

        if (scope.Kind == SettingsCommitScopeKind.FlyoutParameterGroup
            && scope.GlassSubKey is { } regSub
            && scope.FlyoutGroup is { } group)
        {
            return HasFlyoutParameterGroupDirty(regSub, group);
        }

        if (scope.Kind == SettingsCommitScopeKind.GlassSubKey
            && scope.GlassSubKeyMatch is GlassSubKeyMatchMode.Exact
                or GlassSubKeyMatchMode.Descendants
                or GlassSubKeyMatchMode.PartitionExceptDescendants)
        {
            return HasGlassSubKeyFilterDirty(scope);
        }

        if (scope.Kind == SettingsCommitScopeKind.DwmKeySubset
            && scope.DwmValueNames is { Length: > 0 } dwmKeys)
        {
            return HasDwmKeysDirty(dwmKeys, scope.DwmHive);
        }

        if (scope.Kind == SettingsCommitScopeKind.GlassValueSubset
            && scope.GlassValueNames is { Length: > 0 } glassKeys)
        {
            foreach (var name in glassKeys)
            {
                if (IsParameterDirty(RegistryParameterRef.Glass("", name)))
                    return true;
            }

            return false;
        }

        if (scope.Kind == SettingsCommitScopeKind.WindowMetricsKeySubset
            && scope.WindowMetricsValueNames is { Length: > 0 } wmKeys)
        {
            return HasWindowMetricsKeysDirty(wmKeys);
        }

        if (scope.Kind == SettingsCommitScopeKind.Area && scope.Area == CustomizationAreaId.Text)
        {
            if (HasWindowMetricsKeysDirty(SystemFontsCatalog.RegistryValueNames))
                return true;
        }

        foreach (var partition in CommitScopeCatalog.GetGlassPartitions(scope))
        {
            if (HasGlassPartitionDirty(partition))
                return true;
        }

        foreach (var partition in CommitScopeCatalog.GetDwmPartitions(scope))
        {
            if (HasDwmPartitionDirty(partition, scope.DwmHive))
                return true;
        }

        foreach (var parameter in CommitScopeCatalog.GetTrackedGlassParameters(scope))
        {
            if (IsParameterDirty(parameter))
                return true;
        }

        return false;
    }

    /// <summary>Отпечаток текущего DWM-реестра (HKCU+HKLM) для ожидания стабилизации после старта платформы.</summary>
    public string ComputeDwmRegistryFingerprint()
    {
        var parts = new List<string>();
        foreach (var entry in CaptureAllDwmValues()
                     .OrderBy(e => e.DwmHive)
                     .ThenBy(e => e.ValueName, StringComparer.OrdinalIgnoreCase))
        {
            parts.Add($"{entry.DwmHive}:{entry.ValueName}={Convert.ToHexString(entry.Data)}");
        }

        return string.Join("|", parts);
    }

    /// <summary>Отпечаток HKCU\WindowMetrics (6 LOGFONT) для RegWatch.</summary>
    public string ComputeWindowMetricsFingerprint()
    {
        var current = CaptureAllWindowMetrics();
        var parts = SystemFontsCatalog.RegistryValueNames
            .OrderBy(static n => n, StringComparer.OrdinalIgnoreCase)
            .Select(name =>
            {
                current.TryGetValue(name, out var data);
                return $"{name}={Convert.ToHexString(data ?? [])}";
            });
        return string.Join("|", parts);
    }

    public void CaptureScope(SettingsCommitScope scope)
    {
        if (scope.Kind == SettingsCommitScopeKind.All)
        {
            Capture();
            return;
        }

        if (scope.Kind is SettingsCommitScopeKind.System or SettingsCommitScopeKind.Processes)
            return;

        if (scope.Kind == SettingsCommitScopeKind.FlyoutParameterGroup
            && scope.GlassSubKey is { } regSub
            && scope.FlyoutGroup is { } group)
        {
            CaptureFlyoutParameterGroup(regSub, group);
            return;
        }

        if (scope.Kind == SettingsCommitScopeKind.GlassSubKey
            && scope.GlassSubKeyMatch is GlassSubKeyMatchMode.Exact
                or GlassSubKeyMatchMode.Descendants
                or GlassSubKeyMatchMode.PartitionExceptDescendants)
        {
            CaptureGlassSubKeyFilter(scope);
            return;
        }

        if (scope.Kind == SettingsCommitScopeKind.DwmKeySubset
            && scope.DwmValueNames is { Length: > 0 } dwmKeys)
        {
            CaptureDwmKeySubset(dwmKeys, scope.DwmHive);
            return;
        }

        if (scope.Kind == SettingsCommitScopeKind.GlassValueSubset)
        {
            SyncTrackedGlassParameters(scope);
            return;
        }

        if (scope.Kind == SettingsCommitScopeKind.WindowMetricsKeySubset
            && scope.WindowMetricsValueNames is { Length: > 0 } wmKeys)
        {
            CaptureWindowMetricsKeySubset(wmKeys);
            return;
        }

        if (scope.Kind == SettingsCommitScopeKind.Area && scope.Area == CustomizationAreaId.Text)
        {
            CaptureWindowMetricsKeySubset(SystemFontsCatalog.RegistryValueNames);
        }

        foreach (var partition in CommitScopeCatalog.GetGlassPartitions(scope))
            _glassPartitions[partition] = CaptureGlassPartition(partition);

        var touchedDwm = CommitScopeCatalog.GetDwmPartitions(scope).Any();
        if (touchedDwm || scope.Kind == SettingsCommitScopeKind.All)
        {
            if (scope.DwmHive is { } selectedHive)
            {
                var fromOtherHives = _dwmFullSnapshot
                    .Where(e => e.DwmHive != selectedHive)
                    .ToList();
                fromOtherHives.AddRange(CaptureAllDwmValues(selectedHive));
                _dwmFullSnapshot = fromOtherHives;
            }
            else
            {
                _dwmFullSnapshot = CaptureAllDwmValues();
            }

            foreach (var partition in DwmSnapshotCatalog.AllPartitions)
                _dwmPartitions[partition] = FilterDwmPartition(_dwmFullSnapshot, partition);
        }

        SyncTrackedGlassParameters(scope);
    }

    private void SyncTrackedGlassParameters(SettingsCommitScope scope)
    {
        foreach (var parameter in CommitScopeCatalog.GetTrackedGlassParameters(scope))
            UpsertParameterSnapshot(parameter);
    }

    /// <summary>Обновить в снимке только параметры группы flyout (после «Сохранить» на detail).</summary>
    private void CaptureFlyoutParameterGroup(string regSub, FlyoutParameterGroup group)
    {
        foreach (var parameter in FlyoutScopeCatalog.GetParameters(regSub, group))
            UpsertParameterSnapshot(parameter);
    }

    /// <summary>Был ли ключ в baseline-снимке (для inherit/delete-on-rollback на Adjacent).</summary>
    public bool HasGlassSnapshotValue(string relativeSubKey, string valueName) =>
        FindSnapshotEntry(RegistryParameterRef.Glass(relativeSubKey, valueName)) is not null;

    /// <summary>Подтянуть один Glass-ключ в baseline из текущего реестра (тихая миграция schema).</summary>
    public void AbsorbGlassParameter(string relativeSubKey, string valueName) =>
        UpsertParameterSnapshot(RegistryParameterRef.Glass(relativeSubKey, valueName));

    private void UpsertParameterSnapshot(RegistryParameterRef parameter)
    {
        if (parameter.Store != RegistryStoreKind.Glass)
            return;

        var partition = MapGlassSubKeyToPartition(parameter.GlassSubKey ?? "");
        if (partition is null)
            return;

        var subKey = ResolveGlassSubKey(parameter.GlassSubKey);
        var current = ReadCurrentEntry(parameter);

        if (!_glassPartitions.TryGetValue(partition.Value, out var list))
            list = CaptureGlassPartition(partition.Value);
        else
            list = [.. list];

        list.RemoveAll(e =>
            string.Equals(e.SubKey, subKey, StringComparison.OrdinalIgnoreCase)
            && string.Equals(e.ValueName, parameter.ValueName, StringComparison.OrdinalIgnoreCase));

        if (current is not null)
            list.Add(current);

        _glassPartitions[partition.Value] = list;
    }

    public void Revert()
    {
        foreach (GlassRegistryPartition partition in Enum.GetValues<GlassRegistryPartition>())
            RevertGlassPartition(partition);

        RevertDwmAll();
        RevertWindowMetricsAll();
    }

    private bool HasFlyoutParameterGroupDirty(string regSub, FlyoutParameterGroup group)
    {
        foreach (var parameter in FlyoutScopeCatalog.GetParameters(regSub, group))
        {
            if (IsParameterDirty(parameter))
                return true;
        }

        return false;
    }

    private bool IsParameterDirty(RegistryParameterRef parameter)
    {
        var current = ReadCurrentEntry(parameter);
        var snap = FindSnapshotEntry(parameter);
        if (current is null && snap is null)
            return false;
        if (current is null || snap is null)
            return true;
        return !EntryEquals(current, snap);
    }

    private RegEntry? FindSnapshotEntry(RegistryParameterRef parameter)
    {
        if (parameter.Store != RegistryStoreKind.Glass)
            return null;

        var partition = MapGlassSubKeyToPartition(parameter.GlassSubKey ?? "");
        if (partition is null || !_glassPartitions.TryGetValue(partition.Value, out var list))
            return null;

        var subKey = ResolveGlassSubKey(parameter.GlassSubKey);
        return list.FirstOrDefault(e =>
            string.Equals(e.SubKey, subKey, StringComparison.OrdinalIgnoreCase)
            && string.Equals(e.ValueName, parameter.ValueName, StringComparison.OrdinalIgnoreCase));
    }

    private static RegEntry? ReadCurrentEntry(RegistryParameterRef parameter)
    {
        if (parameter.Store != RegistryStoreKind.Glass)
            return null;

        var subKey = ResolveGlassSubKey(parameter.GlassSubKey);
        using var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(subKey, false);
        if (key is null)
            return null;

        var val = key.GetValue(parameter.ValueName);
        return val is null ? null : ToEntry(subKey, parameter.ValueName, val);
    }

    private static string ResolveGlassSubKey(string? relativeSubKey) =>
        string.IsNullOrEmpty(relativeSubKey)
            ? RegistryPaths.GlassRoot
            : RegistryPaths.GlassRoot + "\\" + relativeSubKey.Replace('/', '\\');

    private static GlassRegistryPartition? MapGlassSubKeyToPartition(string relativeSubKey)
    {
        var normalized = relativeSubKey.Replace('/', '\\');
        if (normalized.StartsWith("Root\\", StringComparison.OrdinalIgnoreCase))
            return GlassRegistryPartition.Root;

        var first = normalized.Split('\\')[0];
        if (MenuItemRegistry.IsMenuItemSubKey(normalized))
            return GlassRegistryPartition.Menu;

        return first.ToUpperInvariant() switch
        {
            "MENU" => GlassRegistryPartition.Menu,
            "DROPDOWN" => GlassRegistryPartition.DropDown,
            "TOOLTIP" => GlassRegistryPartition.ToolTip,
            "ROOT" => GlassRegistryPartition.Root,
            "TRAY" => GlassRegistryPartition.Tray,
            "" => GlassRegistryPartition.GeneralRoot,
            _ => null
        };
    }

    private bool HasGlassPartitionDirty(GlassRegistryPartition partition)
    {
        var current = CaptureGlassPartition(partition);
        _glassPartitions.TryGetValue(partition, out var snap);
        snap ??= [];
        return !SameEntries(current, snap);
    }

    private bool HasGlassSubKeyFilterDirty(SettingsCommitScope scope)
    {
        foreach (var partition in EnumeratePartitionsForSubKeyFilter(scope))
        {
            var current = FilterEntriesBySubKeyMatch(CaptureGlassPartition(partition), scope);
            _glassPartitions.TryGetValue(partition, out var snap);
            var filteredSnap = FilterEntriesBySubKeyMatch(snap ?? [], scope);
            if (!SameEntries(current, filteredSnap))
                return true;
        }

        return false;
    }

    private void CaptureGlassSubKeyFilter(SettingsCommitScope scope)
    {
        foreach (var partition in EnumeratePartitionsForSubKeyFilter(scope))
        {
            var currentAll = CaptureGlassPartition(partition);
            _glassPartitions.TryGetValue(partition, out var snap);
            snap ??= [];

            var kept = snap
                .Where(e => !EntryMatchesSubKeyFilter(e, scope))
                .ToList();
            kept.AddRange(FilterEntriesBySubKeyMatch(currentAll, scope));
            _glassPartitions[partition] = kept;
        }
    }

    private static IEnumerable<GlassRegistryPartition> EnumeratePartitionsForSubKeyFilter(
        SettingsCommitScope scope)
    {
        if (scope.GlassSubKeyMatch == GlassSubKeyMatchMode.PartitionExceptDescendants)
        {
            yield return GlassRegistryPartition.Tray;
            yield break;
        }

        var seen = new HashSet<GlassRegistryPartition>();
        foreach (var relative in EnumerateRelativeSubKeysForFilter(scope))
        {
            if (MapGlassSubKeyToPartition(relative) is not { } partition)
                continue;
            if (seen.Add(partition))
                yield return partition;
        }
    }

    private static IEnumerable<string> EnumerateRelativeSubKeysForFilter(SettingsCommitScope scope)
    {
        if (scope.GlassSubKeys is { Length: > 0 } keys)
        {
            foreach (var key in keys)
                yield return key.Replace('/', '\\');
            yield break;
        }

        if (scope.GlassSubKey is { } single)
            yield return single.Replace('/', '\\');
    }

    private static List<RegEntry> FilterEntriesBySubKeyMatch(
        IReadOnlyList<RegEntry> entries,
        SettingsCommitScope scope) =>
        entries.Where(e => EntryMatchesSubKeyFilter(e, scope)).ToList();

    private static bool EntryMatchesSubKeyFilter(RegEntry entry, SettingsCommitScope scope)
    {
        var full = entry.SubKey;
        return scope.GlassSubKeyMatch switch
        {
            GlassSubKeyMatchMode.Exact =>
                EnumerateRelativeSubKeysForFilter(scope)
                    .Any(rel => string.Equals(full, ResolveGlassSubKey(rel), StringComparison.OrdinalIgnoreCase)),
            GlassSubKeyMatchMode.Descendants =>
                scope.GlassSubKey is { } parent
                && full.StartsWith(
                    ResolveGlassSubKey(parent) + "\\",
                    StringComparison.OrdinalIgnoreCase),
            GlassSubKeyMatchMode.PartitionExceptDescendants =>
                scope.GlassSubKey is { } excludeParent
                && !full.StartsWith(
                    ResolveGlassSubKey(excludeParent) + "\\",
                    StringComparison.OrdinalIgnoreCase),
            _ => false
        };
    }

    private bool HasDwmPartitionDirty(DwmSnapshotPartition partition, DwmRegistryHive? hive = null) =>
        HasDwmKeysDirty(DwmSnapshotCatalog.GetKeys(partition), hive);

    private bool HasDwmKeysDirty(IReadOnlyList<string> valueNames, DwmRegistryHive? hive = null)
    {
        var allowed = new HashSet<string>(valueNames, StringComparer.OrdinalIgnoreCase);
        var hives = hive is { } selectedHive ? [selectedHive] : EnumerateDwmHives();
        foreach (var currentHive in hives)
        {
            var currentByName = CaptureAllDwmValues(currentHive)
                .Where(e => allowed.Contains(e.ValueName))
                .ToDictionary(static e => e.ValueName, static e => e, StringComparer.OrdinalIgnoreCase);
            var snapByName = _dwmFullSnapshot
                .Where(e => e.DwmHive == currentHive && allowed.Contains(e.ValueName))
                .ToDictionary(static e => e.ValueName, static e => e, StringComparer.OrdinalIgnoreCase);

            foreach (var name in allowed)
            {
                snapByName.TryGetValue(name, out var snapEntry);
                currentByName.TryGetValue(name, out var curEntry);
                if (!DwmDefaults.IsSemanticallyEqual(name, snapEntry, curEntry, EntryEquals))
                    return true;
            }
        }

        return false;
    }

    private void CaptureDwmKeySubset(IReadOnlyList<string> valueNames, DwmRegistryHive? hive = null)
    {
        var list = _dwmFullSnapshot.ToList();
        var hives = hive is { } selectedHive ? [selectedHive] : EnumerateDwmHives();
        foreach (var currentHive in hives)
        {
            foreach (var name in valueNames)
            {
                list.RemoveAll(e =>
                    e.DwmHive == currentHive
                    && string.Equals(e.ValueName, name, StringComparison.OrdinalIgnoreCase));
                var entry = TryReadDwmValueEntry(currentHive, name);
                if (entry is not null)
                    list.Add(entry);
            }
        }

        _dwmFullSnapshot = list;
        foreach (var partition in DwmSnapshotCatalog.AllPartitions)
            _dwmPartitions[partition] = FilterDwmPartition(_dwmFullSnapshot, partition);
    }

    /// <summary>Ключ уже в полном DWM-снимке (в т.ч. subset для исключённых value).</summary>
    public bool ContainsDwmKey(string valueName, DwmRegistryHive? hive = null) =>
        _dwmFullSnapshot.Any(e =>
            (hive is null || e.DwmHive == hive)
            && string.Equals(e.ValueName, valueName, StringComparison.OrdinalIgnoreCase));

    /// <summary>Зафиксировать текущее значение перед записью в исключённый из автоснимка ключ.</summary>
    public void PrimeDwmKeysFromRegistry(DwmRegistryHive hive, IReadOnlyList<string> valueNames) =>
        CaptureDwmKeySubset(valueNames, hive);

    private static RegEntry? TryReadDwmValueEntry(DwmRegistryHive hive, string valueName)
    {
        using var key = DwmRegistry.OpenSubKey(hive, writable: false);
        if (key is null)
            return null;

        var val = key.GetValue(valueName);
        return val is null ? null : ToEntry(RegistryPaths.DwmRoot, valueName, val, hive);
    }

    private static List<RegEntry> CaptureGlassPartition(GlassRegistryPartition partition)
    {
        var result = new List<RegEntry>();
        if (partition == GlassRegistryPartition.GeneralRoot)
            ReadGlassRootValuesOnly(result);
        else
            EnumGlassRecursive(GlassRegistryPartitionPaths.GetFullPath(partition), result);

        return FilterPlatformEntries(result);
    }

    private static List<RegEntry> FilterDwmPartition(IReadOnlyList<RegEntry> allValues, DwmSnapshotPartition partition)
    {
        var allowed = new HashSet<string>(DwmSnapshotCatalog.GetKeys(partition), StringComparer.OrdinalIgnoreCase);
        return allValues.Where(e => allowed.Contains(e.ValueName)).ToList();
    }

    private static List<RegEntry> CaptureAllDwmValues(DwmRegistryHive? hive = null)
    {
        var result = new List<RegEntry>();
        var hives = hive is { } selectedHive ? [selectedHive] : EnumerateDwmHives();
        foreach (var currentHive in hives)
        {
            using var key = DwmRegistry.OpenSubKey(currentHive, writable: false);
            if (key is null)
                continue;

            foreach (var name in key.GetValueNames())
            {
                if (DwmSnapshotExclusions.IsExcludedDwmValue(name))
                    continue;

                var val = key.GetValue(name);
                if (val is not null)
                    result.Add(ToEntry(RegistryPaths.DwmRoot, name, val, currentHive));
            }
        }

        return result;
    }

    private void RevertGlassPartition(GlassRegistryPartition partition)
    {
        _glassPartitions.TryGetValue(partition, out var snapEntries);
        snapEntries ??= [];

        var curEntries = CaptureGlassPartition(partition);
        var subKeys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in snapEntries)
            subKeys.Add(e.SubKey);
        foreach (var e in curEntries)
            subKeys.Add(e.SubKey);

        if (partition == GlassRegistryPartition.GeneralRoot)
            subKeys.Add(RegistryPaths.GlassRoot);

        var snapByKey = snapEntries
            .GroupBy(e => e.SubKey, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(g => g.Key, g => g.ToList(), StringComparer.OrdinalIgnoreCase);

        foreach (var sk in subKeys)
        {
            if (ShouldSkipRevertSubKey(sk))
                continue;

            snapByKey.TryGetValue(sk, out var snapList);
            var curList = curEntries.Where(e => string.Equals(e.SubKey, sk, StringComparison.OrdinalIgnoreCase)).ToList();
            using var key = OpenWritableSubKey(sk);
            if (key is null)
                continue;

            foreach (var cur in curList)
            {
                if (RegistryPaths.IsNeverSnapshotValueName(cur.ValueName))
                    continue;

                var snap = snapList?.FirstOrDefault(s =>
                    string.Equals(s.ValueName, cur.ValueName, StringComparison.OrdinalIgnoreCase));
                if (snap is null)
                    key.DeleteValue(cur.ValueName, false);
                else if (!EntryEquals(cur, snap))
                    RestoreGlassValue(key, snap);
            }

            if (snapList is null)
                continue;

            foreach (var snap in snapList)
            {
                if (curList.Any(c => string.Equals(c.ValueName, snap.ValueName, StringComparison.OrdinalIgnoreCase)))
                    continue;
                RestoreGlassValue(key, snap);
            }
        }

        if (partition != GlassRegistryPartition.GeneralRoot)
            PruneOrphanSubKeyTrees(GlassRegistryPartitionPaths.GetFullPath(partition), snapEntries);
    }

    private static void PruneOrphanSubKeyTrees(string rootPath, IReadOnlyList<RegEntry> snapEntries)
    {
        using var root = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(rootPath);
        if (root is null)
            return;

        foreach (var childName in root.GetSubKeyNames())
        {
            var childPath = rootPath + "\\" + childName;
            var hasSnapUnderChild = snapEntries.Any(e =>
                string.Equals(e.SubKey, childPath, StringComparison.OrdinalIgnoreCase)
                || e.SubKey.StartsWith(childPath + "\\", StringComparison.OrdinalIgnoreCase));

            if (!hasSnapUnderChild)
                GlassRegistry.DeleteSubKeyTree(ToRelativeGlassSubKey(childPath));
        }
    }

    private static string ToRelativeGlassSubKey(string fullSubKeyPath)
    {
        if (string.Equals(fullSubKeyPath, RegistryPaths.GlassRoot, StringComparison.OrdinalIgnoreCase))
            return string.Empty;

        var prefix = RegistryPaths.GlassRoot + "\\";
        return fullSubKeyPath.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
            ? fullSubKeyPath[prefix.Length..]
            : fullSubKeyPath;
    }

    private void RevertDwmAll()
    {
        foreach (var hive in EnumerateDwmHives())
            RevertDwmHive(hive);
    }

    private void RevertDwmHive(DwmRegistryHive hive)
    {
        var snapByName = _dwmFullSnapshot
            .Where(e => e.DwmHive == hive)
            .ToDictionary(e => e.ValueName, e => e, StringComparer.OrdinalIgnoreCase);

        using var key = DwmRegistry.OpenSubKey(hive, writable: true) ?? DwmRegistry.CreateSubKey(hive);
        if (key is null)
            return;

        var currentNames = key.GetValueNames()
            .Where(n => !DwmSnapshotExclusions.IsExcludedDwmValue(n))
            .ToList();

        foreach (var name in currentNames)
        {
            if (snapByName.TryGetValue(name, out var snap))
                RestoreDwmValue(hive, key, snap);
            else
                key.DeleteValue(name, false);
        }

        foreach (var snap in snapByName.Values)
        {
            if (currentNames.Any(n => string.Equals(n, snap.ValueName, StringComparison.OrdinalIgnoreCase)))
                continue;

            RestoreDwmValue(hive, key, snap);
        }
    }

    private static void RestoreDwmValue(DwmRegistryHive hive, RegistryKey key, RegEntry snap)
    {
        switch ((RegistryValueKind)snap.Type)
        {
            case RegistryValueKind.DWord when snap.Data.Length >= 4:
                DwmRegistry.WriteDword(hive, snap.ValueName, BitConverter.ToUInt32(snap.Data, 0));
                break;
            case RegistryValueKind.String:
                var text = System.Text.Encoding.Unicode.GetString(snap.Data);
                if (text.EndsWith('\0'))
                    text = text[..^1];
                key.SetValue(snap.ValueName, text, RegistryValueKind.String);
                break;
            case RegistryValueKind.Binary:
                key.SetValue(snap.ValueName, snap.Data, RegistryValueKind.Binary);
                break;
            default:
                key.SetValue(snap.ValueName, snap.Data, (RegistryValueKind)snap.Type);
                break;
        }
    }

    /// <summary>
    /// Как RestoreDwmValue: SetValue(DWord, byte[]) бросает ArgumentException — нужен int/uint.
    /// </summary>
    private static void RestoreGlassValue(RegistryKey key, RegEntry snap)
    {
        switch ((RegistryValueKind)snap.Type)
        {
            case RegistryValueKind.DWord when snap.Data.Length >= 4:
                key.SetValue(snap.ValueName, unchecked((int)BitConverter.ToUInt32(snap.Data, 0)), RegistryValueKind.DWord);
                break;
            case RegistryValueKind.QWord when snap.Data.Length >= 8:
                key.SetValue(snap.ValueName, BitConverter.ToInt64(snap.Data, 0), RegistryValueKind.QWord);
                break;
            case RegistryValueKind.String:
                var text = System.Text.Encoding.Unicode.GetString(snap.Data);
                if (text.EndsWith('\0'))
                    text = text[..^1];
                key.SetValue(snap.ValueName, text, RegistryValueKind.String);
                break;
            case RegistryValueKind.ExpandString:
                var expand = System.Text.Encoding.Unicode.GetString(snap.Data);
                if (expand.EndsWith('\0'))
                    expand = expand[..^1];
                key.SetValue(snap.ValueName, expand, RegistryValueKind.ExpandString);
                break;
            case RegistryValueKind.MultiString:
                var multi = System.Text.Encoding.Unicode.GetString(snap.Data)
                    .Split('\0', StringSplitOptions.RemoveEmptyEntries);
                key.SetValue(snap.ValueName, multi, RegistryValueKind.MultiString);
                break;
            case RegistryValueKind.Binary:
                key.SetValue(snap.ValueName, snap.Data, RegistryValueKind.Binary);
                break;
            default:
                if (snap.Data.Length > 0)
                    key.SetValue(snap.ValueName, snap.Data, RegistryValueKind.Binary);
                break;
        }
    }

    private static RegistryKey? OpenWritableSubKey(string subKeyPath)
    {
        try
        {
            return global::Microsoft.Win32.Registry.CurrentUser.CreateSubKey(
                subKeyPath,
                global::Microsoft.Win32.RegistryKeyPermissionCheck.ReadWriteSubTree);
        }
        catch
        {
            return null;
        }
    }

    private static bool ShouldSkipRevertSubKey(string subKey) =>
        RegistryPaths.IsNeverSnapshotFullSubKey(subKey);

    private static void ReadGlassRootValuesOnly(List<RegEntry> outEntries)
    {
        using var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RegistryPaths.GlassRoot, false);
        if (key is null)
            return;

        foreach (var name in key.GetValueNames())
        {
            var val = key.GetValue(name);
            if (val is not null)
                outEntries.Add(ToEntry(RegistryPaths.GlassRoot, name, val));
        }
    }

    private static void EnumGlassRecursive(string path, List<RegEntry> outEntries, HashSet<string>? subKeys = null)
    {
        subKeys?.Add(path);
        using var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(path, false);
        if (key is null)
            return;

        foreach (var name in key.GetValueNames())
        {
            var val = key.GetValue(name);
            if (val is not null)
                outEntries.Add(ToEntry(path, name, val));
        }

        foreach (var child in key.GetSubKeyNames())
        {
            if (IsNeverSnapshotRelativePath(path, child))
                continue;
            EnumGlassRecursive(path + "\\" + child, outEntries, subKeys);
        }
    }

    private static bool IsNeverSnapshotRelativePath(string parentPath, string childName)
    {
        if (string.Equals(childName, RegistryPaths.GlassDisabledListSegment, StringComparison.OrdinalIgnoreCase))
            return true;

        if (!string.Equals(parentPath, RegistryPaths.GlassRoot, StringComparison.OrdinalIgnoreCase))
            return false;

        return RegistryPaths.GlassNeverSnapshotSubKeys.Contains(childName, StringComparer.OrdinalIgnoreCase);
    }

    private static RegEntry ToEntry(string subKey, string valueName, object val, DwmRegistryHive? hive = null) => val switch
    {
        int i => new RegEntry { SubKey = subKey, ValueName = valueName, DwmHive = hive, Type = (int)RegistryValueKind.DWord, Data = BitConverter.GetBytes(i) },
        uint u => new RegEntry { SubKey = subKey, ValueName = valueName, DwmHive = hive, Type = (int)RegistryValueKind.DWord, Data = BitConverter.GetBytes(u) },
        byte[] b => new RegEntry { SubKey = subKey, ValueName = valueName, DwmHive = hive, Type = (int)RegistryValueKind.Binary, Data = (byte[])b.Clone() },
        string s => new RegEntry { SubKey = subKey, ValueName = valueName, DwmHive = hive, Type = (int)RegistryValueKind.String, Data = System.Text.Encoding.Unicode.GetBytes(s + "\0") },
        _ => new RegEntry { SubKey = subKey, ValueName = valueName, DwmHive = hive, Type = (int)RegistryValueKind.Unknown, Data = [] }
    };

    private static bool SameEntries(List<RegEntry> a, List<RegEntry> b)
    {
        var sa = a.OrderBy(e => e.DwmHive)
            .ThenBy(e => e.SubKey, StringComparer.OrdinalIgnoreCase)
            .ThenBy(e => e.ValueName, StringComparer.OrdinalIgnoreCase).ToList();
        var sb = b.OrderBy(e => e.DwmHive)
            .ThenBy(e => e.SubKey, StringComparer.OrdinalIgnoreCase)
            .ThenBy(e => e.ValueName, StringComparer.OrdinalIgnoreCase).ToList();
        if (sa.Count != sb.Count)
            return false;

        for (var i = 0; i < sa.Count; i++)
        {
            if (!EntryEquals(sa[i], sb[i]))
                return false;
        }

        return true;
    }

    private static bool EntryEquals(RegEntry a, RegEntry b)
        => a.DwmHive == b.DwmHive
           && string.Equals(a.SubKey, b.SubKey, StringComparison.OrdinalIgnoreCase)
           && string.Equals(a.ValueName, b.ValueName, StringComparison.OrdinalIgnoreCase)
           && a.Type == b.Type && a.Data.AsSpan().SequenceEqual(b.Data);

    private static string FormatDwmEntryForWatch(string valueName, RegEntry? entry)
    {
        if (entry is null)
        {
            if (DwmDefaults.UsesAbsentAsDefault(valueName))
                return $"absent(≡default:0x{DwmDefaults.DefaultWhenKeyAbsent(valueName):X8})";
            return "absent";
        }

        if (entry.Type == (int)RegistryValueKind.DWord && entry.Data.Length >= 4)
            return $"dword:0x{BitConverter.ToUInt32(entry.Data, 0):X8}";

        if (entry.Type == (int)RegistryValueKind.String && entry.Data.Length >= 2)
        {
            var text = System.Text.Encoding.Unicode.GetString(entry.Data).TrimEnd('\0');
            if (text.Length > 48)
                text = text[..48] + "…";
            return $"string:\"{text}\"";
        }

        var previewLen = Math.Min(entry.Data.Length, 8);
        return $"type{entry.Type}:0x{Convert.ToHexString(entry.Data.AsSpan(0, previewLen))}{(entry.Data.Length > previewLen ? "…" : string.Empty)}";
    }

    private static DwmRegistryHive[] EnumerateDwmHives() =>
        [DwmRegistryHive.Hkcu, DwmRegistryHive.Hklm];

    private static List<RegEntry> FilterPlatformEntries(List<RegEntry> entries) =>
        entries.Where(e =>
                !string.Equals(e.SubKey, RegistryPaths.GlassSettingsPlacementKey, StringComparison.OrdinalIgnoreCase)
                && !RegistryPaths.IsNeverSnapshotValueName(e.ValueName))
            .ToList();

    private bool HasAnyWindowMetricsDirty() =>
        HasWindowMetricsKeysDirty(SystemFontsCatalog.RegistryValueNames);

    private bool HasWindowMetricsKeysDirty(IReadOnlyList<string> valueNames)
    {
        var current = CaptureAllWindowMetrics();
        foreach (var name in valueNames)
        {
            current.TryGetValue(name, out var cur);
            _windowMetricsSnapshot.TryGetValue(name, out var snap);
            if (!WindowMetricsBytesEqual(snap, cur))
                return true;
        }

        return false;
    }

    private void CaptureWindowMetricsKeySubset(IReadOnlyList<string> valueNames)
    {
        var current = CaptureAllWindowMetrics();
        foreach (var name in valueNames)
        {
            if (current.TryGetValue(name, out var data))
                _windowMetricsSnapshot[name] = data;
            else
                _windowMetricsSnapshot.Remove(name);
        }
    }

    private static Dictionary<string, byte[]> CaptureAllWindowMetrics()
    {
        var result = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        foreach (var name in SystemFontsCatalog.RegistryValueNames)
        {
            var raw = WindowMetricsFontRegistry.TryReadRawBytes(name);
            if (raw is { Length: >= 92 })
                result[name] = raw.Length == 92 ? raw : raw[..92].ToArray();
        }

        return result;
    }

    private void RevertWindowMetricsAll()
    {
        using var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(SystemFontsCatalog.RegistrySubKey, true)
                        ?? global::Microsoft.Win32.Registry.CurrentUser.CreateSubKey(SystemFontsCatalog.RegistrySubKey, true);
        if (key is null)
            return;

        var currentNames = key.GetValueNames().ToList();
        foreach (var name in SystemFontsCatalog.RegistryValueNames)
        {
            if (_windowMetricsSnapshot.TryGetValue(name, out var snap))
                key.SetValue(name, snap, RegistryValueKind.Binary);
            else if (currentNames.Any(n => string.Equals(n, name, StringComparison.OrdinalIgnoreCase)))
                key.DeleteValue(name, false);
        }
    }

    private static bool WindowMetricsBytesEqual(byte[]? a, byte[]? b)
    {
        if (a is null && b is null)
            return true;
        if (a is null || b is null)
            return false;
        var len = Math.Min(a.Length, b.Length);
        if (len < 92)
            return a.AsSpan(0, len).SequenceEqual(b.AsSpan(0, len));
        return a.AsSpan(0, 92).SequenceEqual(b.AsSpan(0, 92));
    }
}

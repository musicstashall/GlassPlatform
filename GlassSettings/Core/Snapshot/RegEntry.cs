namespace GlassSettings.Core.Snapshot;

using GlassSettings.Core.Registry;

internal sealed class RegEntry

{

    public required string SubKey { get; init; }

    public required string ValueName { get; init; }

    public DwmRegistryHive? DwmHive { get; init; }

    public int Type { get; init; }

    public byte[] Data { get; init; } = [];

}


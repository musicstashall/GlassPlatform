namespace GlassSettings.Core.Registry;

public static partial class GlassSettingsRegistry
{
    public const string OpenGlassUpdatePendingChannelValue = "OpenGlassUpdatePendingChannel";
    public const string OpenGlassUpdatePendingRepairValue = "OpenGlassUpdatePendingRepair";
    public const string OpenGlassUpdatePendingWindowsBuildValue = "OpenGlassUpdatePendingWindowsBuild";
    public const string OpenGlassUpdateLastCheckOkValue = "OpenGlassUpdateLastCheckOk";

    /// <summary>current, legacy или null, если канал ещё не записан.</summary>
    public static string? ReadOpenGlassUpdateChannelId()
    {
        var text = ReadString(OpenGlassUpdatePendingChannelValue);
        if (string.Equals(text, "legacy", StringComparison.OrdinalIgnoreCase))
            return "legacy";
        if (string.Equals(text, "current", StringComparison.OrdinalIgnoreCase))
            return "current";
        return null;
    }

    public static bool IsOpenGlassUpdateRepair() =>
        GlassRegistry.ReadDword(SubKey, OpenGlassUpdatePendingRepairValue) == 1;

    public static int ReadOpenGlassUpdateWindowsBuild()
    {
        var raw = GlassRegistry.ReadDword(SubKey, OpenGlassUpdatePendingWindowsBuildValue);
        return raw.HasValue ? unchecked((int)raw.Value) : 0;
    }

    /// <summary>
    /// Ключ закрытия: канал и версия. Ремонт после смены сборки Windows добавляет номер сборки,
    /// чтобы та же числовая версия другого канала уведомила снова.
    /// </summary>
    public static string? ReadOpenGlassPendingIdentity()
    {
        var version = ReadOpenGlassUpdatePendingVersion();
        if (string.IsNullOrWhiteSpace(version))
            return null;

        var channel = ReadOpenGlassUpdateChannelId() ?? "legacy";
        if (IsOpenGlassUpdateRepair())
            return "repair|" + channel + "|" + version + "|" + ReadOpenGlassUpdateWindowsBuild();

        return channel + "|" + version;
    }

    /// <summary>
    /// Канал и признак ремонта пишутся после версии. Смена канала или сборки при той же
    /// числовой версии сбрасывает закрытие уведомления.
    /// </summary>
    public static void WriteOpenGlassUpdateOffer(
        string version,
        string htmlUrl,
        string channel,
        bool repair,
        int windowsBuild)
    {
        var previous = ReadOpenGlassPendingIdentity();
        WriteOpenGlassUpdatePending(version, htmlUrl);
        GlassRegistry.WriteString(SubKey, OpenGlassUpdatePendingChannelValue, channel);
        GlassRegistry.WriteDword(SubKey, OpenGlassUpdatePendingRepairValue, repair ? 1u : 0u);
        GlassRegistry.WriteDword(SubKey, OpenGlassUpdatePendingWindowsBuildValue, unchecked((uint)windowsBuild));
        var next = ReadOpenGlassPendingIdentity();
        if (!string.Equals(previous, next, StringComparison.OrdinalIgnoreCase))
            ClearOpenGlassUpdateNoticeDismissedVersion();
    }

    public static void ClearOpenGlassUpdateChannel()
    {
        GlassRegistry.DeleteValue(SubKey, OpenGlassUpdatePendingChannelValue);
        GlassRegistry.DeleteValue(SubKey, OpenGlassUpdatePendingRepairValue);
        GlassRegistry.DeleteValue(SubKey, OpenGlassUpdatePendingWindowsBuildValue);
    }

    public static void WriteOpenGlassUpdateLastCheckOk(bool ok) =>
        GlassRegistry.WriteDword(SubKey, OpenGlassUpdateLastCheckOkValue, ok ? 1u : 0u);

    public static bool IsOpenGlassUpdateLastCheckFailed() =>
        GlassRegistry.ReadDword(SubKey, OpenGlassUpdateLastCheckOkValue) == 0;
}

using GlassSettings.Core.Customization;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;

namespace GlassSettings.Core.Menu;

/// <summary>Master-switch Menu Advanced: EnableCustomRendering и wipe всех kind-параметров.</summary>
public static class MenuCustomRenderingCoordinator
{
    public const string MenuScopeSubKey = "Menu";
    public const string EnableCustomRenderingValueName = "EnableCustomRendering";

    public static RegistryParameterRef MasterParameterRef =>
        RegistryParameterRef.Glass(MenuScopeSubKey, EnableCustomRenderingValueName);

    public static bool IsGlobalGateOnFromRegistry() =>
        GlassRegistry.ReadDword(MenuScopeSubKey, EnableCustomRenderingValueName) == 1;

    /// <summary>Есть ли подключи под Menu\ (Hot, DisabledHot, …), не считая значения в корне Menu.</summary>
    public static bool HasMenuSubkeys()
    {
        var path = GlassRegistry.SubKeyPath(MenuScopeSubKey);
        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(path, writable: false);
        return key?.GetSubKeyNames().Length > 0;
    }

    /// <summary>Диалог gate OFF — только если в baseline других kind есть сохранённые параметры.</summary>
    public static bool ShouldConfirmMenuGateOff(MenuItemKind currentKind) =>
        SettingsSession.Current.RegistrySnapshot.HasOtherMenuKindsSnapshotBaseline(currentKind);

    public static bool IsAnyKindPresent()
    {
        foreach (MenuItemKind kind in Enum.GetValues<MenuItemKind>())
        {
            if (MenuItemRegistry.IsAnyPresent(kind))
                return true;
        }

        return false;
    }

    public static void EnableGlobalGate(bool liveFlyout)
    {
        SettingsSession.Current.WriteGlassDword(
            MenuScopeSubKey,
            EnableCustomRenderingValueName,
            1,
            liveFlyout: liveFlyout);
    }

    public static void DisableGlobalGate(bool liveFlyout)
    {
        SettingsSession.Current.DeleteGlassValue(
            MenuScopeSubKey,
            EnableCustomRenderingValueName,
            liveFlyout: liveFlyout);
    }

    public static void DeleteAllMenuCustomRendering(bool liveFlyout)
    {
        DisableGlobalGate(liveFlyout);

        foreach (MenuItemKind kind in Enum.GetValues<MenuItemKind>())
            MenuItemRegistry.DeleteAll(kind, liveFlyout);
    }

    /// <summary>После wipe Menu Advanced — переснять baseline и снять dirty (реестр = состояние до правок).</summary>
    public static void SyncSessionBaseline()
    {
        SettingsSession.Current.SyncScopeBaseline(SettingsCommitScope.ForMenuCustomRenderingMaster());
        foreach (MenuItemKind kind in Enum.GetValues<MenuItemKind>())
            SettingsSession.Current.SyncScopeBaseline(SettingsCommitScope.ForMenuItemKind(kind));
    }
}

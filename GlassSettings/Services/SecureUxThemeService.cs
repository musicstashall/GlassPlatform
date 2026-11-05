using GlassSettings.Core.Platform;
using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Состояние собственной Glass-интеграции SecureUxTheme4 и препятствий.</summary>
public enum SecureUxThemeState
{
    Unavailable,
    Inactive,
    ActivationPendingRestart,
    Active,
    DeactivationPendingRestart,
    ResidualRegistration,
    OfficialV4External, // Legacy enum value; official v4 is no longer external.
    LegacyOfficialInstall,
    PartialOrConflict,
}

public enum SecureUxThemeObstacle
{
    None,
    MissingDll,
    Wow64Process,
    WrongArchitecture,
    OfficialV4,
    LegacyInstall,
    Conflict,
}

public sealed record SecureUxThemeSnapshot(
    SecureUxThemeState State,
    SecureUxThemeObstacle Obstacle,
    bool LocalDllPresent,
    bool SwitchEnabled,
    bool SwitchOn,
    bool BlocksCustomThemeApply,
    string LocalDllPath,
    string? Detail);

/// <summary>
/// MSI presence, payload availability and enabled registration are independent.
/// Files retained by the author are not an active hook or an installation conflict.
/// </summary>
public static class SecureUxThemeService
{
    private const uint FlgApplicationVerifier = 0x100;

    public static SecureUxThemeSnapshot Query()
    {
        var hr = NativeHostInterop.SecureUxThemeQuery(out var raw);
        if (hr < 0)
        {
            return new SecureUxThemeSnapshot(
                SecureUxThemeState.PartialOrConflict,
                SecureUxThemeObstacle.Conflict,
                false,
                false,
                false,
                false,
                ExpectedSystemDllPath(),
                unchecked((uint)hr).ToString("X8"));
        }

        var localPresent = raw.LocalDllPresent != 0;
        var localPath = string.IsNullOrWhiteSpace(raw.LocalDllPath)
            ? ExpectedSystemDllPath()
            : raw.LocalDllPath;

        var ourIfeo = raw.WinlogonHasOurToken != 0 && (raw.WinlogonGlobalFlag & FlgApplicationVerifier) != 0;
        var loaded = raw.LoadedEventPresent != 0;
        var msiInstalled = SecureUxThemeMsiService.IsInstalled();

        SecureUxThemeState state;
        SecureUxThemeObstacle obstacle;
        if (HasLegacyOfficialInstall(raw))
        {
            state = SecureUxThemeState.LegacyOfficialInstall;
            obstacle = SecureUxThemeObstacle.LegacyInstall;
        }
        else if (ourIfeo && loaded)
        {
            state = SecureUxThemeState.Active;
            obstacle = SecureUxThemeObstacle.None;
        }
        else if (ourIfeo)
        {
            state = SecureUxThemeState.ActivationPendingRestart;
            obstacle = SecureUxThemeObstacle.None;
        }
        else if (loaded)
        {
            state = SecureUxThemeState.DeactivationPendingRestart;
            obstacle = SecureUxThemeObstacle.None;
        }
        else if (raw.ComProxyHijacked != 0)
        {
            state = SecureUxThemeState.ResidualRegistration;
            obstacle = SecureUxThemeObstacle.None;
        }
        else if (localPresent || msiInstalled || raw.ThemeUiProxyPresent != 0)
        {
            state = SecureUxThemeState.Inactive;
            obstacle = SecureUxThemeObstacle.None;
        }
        else
        {
            state = SecureUxThemeState.Unavailable;
            obstacle = SecureUxThemeObstacle.MissingDll;
        }

        var externalInstall = obstacle is SecureUxThemeObstacle.OfficialV4 or SecureUxThemeObstacle.LegacyInstall;
        var switchEnabled = !externalInstall
            && (localPresent || ourIfeo || loaded || raw.ComProxyHijacked != 0);
        var switchOn = !externalInstall && obstacle == SecureUxThemeObstacle.None && (ourIfeo || raw.ComProxyHijacked != 0);
        var blocksCustom = state is SecureUxThemeState.ActivationPendingRestart
            or SecureUxThemeState.DeactivationPendingRestart
            or SecureUxThemeState.ResidualRegistration;

        return new SecureUxThemeSnapshot(
            state,
            obstacle,
            localPresent,
            switchEnabled,
            switchOn,
            blocksCustom,
            localPath,
            raw.OfficialVersion);
    }

    public static SecureUxThemeObstacle EvaluateActivationObstacle(SecureUxThemeSnapshot snapshot)
    {
        if (snapshot.Obstacle is SecureUxThemeObstacle.OfficialV4
            or SecureUxThemeObstacle.LegacyInstall
            or SecureUxThemeObstacle.Conflict)
        {
            return snapshot.Obstacle;
        }

        if (!snapshot.LocalDllPresent)
            return SecureUxThemeObstacle.MissingDll;

        var hr = NativeHostInterop.SecureUxThemeQuery(out var raw);
        if (hr < 0)
            return SecureUxThemeObstacle.Conflict;

        if (raw.Wow64Process != 0)
            return SecureUxThemeObstacle.Wow64Process;

        if (raw.LocalDllArchMatchesOs == 0 && raw.LocalDllPresent != 0)
            return SecureUxThemeObstacle.WrongArchitecture;

        if (raw.NativeMachine != NativeHostInterop.ImageFileMachineI386
            && raw.NativeMachine != NativeHostInterop.ImageFileMachineAmd64)
        {
            return SecureUxThemeObstacle.WrongArchitecture;
        }

        return SecureUxThemeObstacle.None;
    }

    public static int Activate(SecureUxThemeSnapshot snapshot, bool reactivate = false)
    {
        _ = snapshot;
        _ = reactivate;
        return NativeHostInterop.SecureUxThemeActivate(string.Empty, reactivate);
    }

    public static int Deactivate() => NativeHostInterop.SecureUxThemeDeactivate();

    public sealed record DeactivateResult(int Hr, bool RebootRequired);

    // Compatibility name: cleanup now means registration cleanup, never MSI/files.
    public static DeactivateResult DeactivateWithCleanup()
    {
        var before = Query();
        var hr = Deactivate();
        return new DeactivateResult(hr, hr >= 0 &&
            (before.SwitchOn || before.State == SecureUxThemeState.DeactivationPendingRestart));
    }

    public static bool ActivationRequiresRestart(SecureUxThemeSnapshot snapshot, bool reactivate)
    {
        if (reactivate)
            return false;

        return snapshot.State is not SecureUxThemeState.Active
            and not SecureUxThemeState.ActivationPendingRestart
            and not SecureUxThemeState.DeactivationPendingRestart;
    }

    public static bool CheckCleanupPending() => false;

    public static bool TryEnsureLoaded()
    {
        try
        {
            return NativeHostInterop.SecureUxThemeEnsureLoaded();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings SecureUxTheme.TryEnsureLoaded: {ex}");
            return false;
        }
    }

    public static bool ShouldBlockCustomThemeApply(VisualStyleThemeEntry entry)
    {
        if (entry is null || !entry.IsCustom)
            return false;

        return Query().BlocksCustomThemeApply;
    }

    public static VisualStyleThemeEntry? TryGetStockThemeEntry()
    {
        var aero = Path.Combine(VisualStyleCatalog.SystemThemesDirectory, "aero.theme");
        var entry = VisualStyleCatalog.TryGetEntryForThemeFile(aero);
        if (entry is not null && !entry.IsCustom)
            return entry;

        return VisualStyleCatalog.GetInstalledThemes()
            .FirstOrDefault(theme => !theme.IsCustom);
    }

    public static string ObstacleMessage(SecureUxThemeObstacle obstacle, string localDllPath) =>
        obstacle switch
        {
            SecureUxThemeObstacle.OfficialV4 =>
                GlassSettingsResources.Get("Dialog_SignatureOfficialBody"),
            SecureUxThemeObstacle.LegacyInstall =>
                GlassSettingsResources.Get("Dialog_SignatureLegacyBody"),
            SecureUxThemeObstacle.Wow64Process =>
                GlassSettingsResources.Get("Dialog_SignatureWow64Body"),
            SecureUxThemeObstacle.WrongArchitecture =>
                GlassSettingsResources.Get("Dialog_SignatureWrongArchBody"),
            SecureUxThemeObstacle.MissingDll =>
                GlassSettingsResources.Get("Dialog_SignatureMissingPayloadBody"),
            _ => GlassSettingsResources.Get("Dialog_SignatureConflictBody"),
        };

    private static bool HasLegacyOfficialInstall(NativeHostInterop.SecureUxThemeNativeQuery raw) =>
        IsOfficialHook(raw.WinlogonGlobalFlag, raw.WinlogonHasLegacyExact)
        || IsOfficialHook(raw.ExplorerGlobalFlag, raw.ExplorerHasLegacyExact)
        || IsOfficialHook(raw.SettingsGlobalFlag, raw.SettingsHasLegacyExact)
        || IsOfficialHook(raw.LogonUiGlobalFlag, raw.LogonUiHasLegacyExact)
        || IsOfficialHook(raw.DwmGlobalFlag, raw.DwmHasLegacyExact);

    private static bool IsOfficialHook(uint globalFlag, int exactToken) =>
        exactToken != 0 && (globalFlag & FlgApplicationVerifier) != 0;

    private static string ExpectedSystemDllPath()
    {
        var system = Environment.GetFolderPath(Environment.SpecialFolder.System);
        return Path.Combine(system, "SecureUxTheme4.dll");
    }
}

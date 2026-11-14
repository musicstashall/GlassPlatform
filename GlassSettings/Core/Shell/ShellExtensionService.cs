using GlassSettings.Core.Platform;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Shell;

/// <summary>Как зарегистрирован пункт меню (GlassSettings_GetShellMenuModeV1).</summary>
public enum ShellMenuMode
{
    None = 0,
    /// <summary>Классический IContextMenu (Win11: «Показать дополнительные параметры»).</summary>
    Classic = 1,
    /// <summary>Компактное меню Windows 11 через sparse-пакет.</summary>
    Compact = 2,
}

/// <summary>Доверие подписанту GlassShellSparse.msix у текущего пользователя (GlassSettings_GetShellSignerTrustV1).</summary>
public enum ShellSignerTrust
{
    Unknown = 0,
    Trusted = 1,
    Untrusted = 2,
    MsixMissing = 3,
    Unsigned = 4,
}

/// <summary>Итог GlassSettings_TrustShellSignerV1 (ShellSparsePackage.h: SparseSignerTrustResult; 7 = Windows 10).</summary>
public enum ShellSignerTrustResult
{
    Added = 0,
    AlreadyTrusted = 1,
    MsixMissing = 2,
    Unsigned = 3,
    SubjectMismatch = 4,
    StoreFailed = 5,
    Unknown = 6,
    NotWindows11 = 7,
}

public static class ShellExtensionService
{
    /// <summary>Publisher sparse-пакета = Subject сертификата разработчика (GlassShellMenuConstants.h: kSparsePublisher).</summary>
    public const string SparsePublisher = "CN=Stanislav K.";

    public const string AuthorCertificateReadme = ShellExtensionAvailability.AuthorCertificateReadme;

    public static bool IsBundledDllRegistered()
    {
        try
        {
            return NativeHostInterop.IsShellExtensionRegistered();
        }
        catch
        {
            return false;
        }
    }

    public static ShellMenuMode GetMenuMode()
    {
        try
        {
            return (ShellMenuMode)NativeHostInterop.GetShellMenuMode();
        }
        catch
        {
            return IsBundledDllRegistered() ? ShellMenuMode.Classic : ShellMenuMode.None;
        }
    }

    /// <summary>
    /// На Windows 11 без доверия сертификату разработчика sparse-пакет не ставится и переключатель
    /// управляет классическим меню; страница показывает уведомление об ограничении.
    /// Windows 10 и ошибки запроса → Unknown (уведомление не показывается).
    /// </summary>
    public static ShellSignerTrust GetSignerTrust(out string signerSubject)
    {
        signerSubject = string.Empty;
        if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000))
            return ShellSignerTrust.Unknown;
        try
        {
            return (ShellSignerTrust)NativeHostInterop.GetShellSignerTrust(out signerSubject);
        }
        catch
        {
            return ShellSignerTrust.Unknown;
        }
    }

    public static string GetAuthorCertificatePath() =>
        ShellExtensionAvailability.GetAuthorCertificatePath();

    public static bool IsBundledShellExtensionPresent() =>
        ShellExtensionAvailability.IsBundledShellExtensionPresent();

    public static bool CanToggleShellExtension =>
        IsBundledDllRegistered()
        || IsBundledShellExtensionPresent()
        || GlassAutorunTaskExists();

    public static bool CanRegisterShellExtension(bool autorunTaskExists) =>
        ShellExtensionAvailability.CanRegisterShellExtension(autorunTaskExists);

    public static bool IsShellExtensionOperationInProgress()
    {
        try
        {
            return NativeHostInterop.IsShellExtensionOperationInProgress();
        }
        catch
        {
            return false;
        }
    }

    public static async Task<bool> SetShellExtensionRegisteredAsync(bool register)
    {
        if (IsShellExtensionOperationInProgress())
            return false;

        var taskExists = GlassAutorunTaskExists();
        var currentlyRegistered = IsBundledDllRegistered();

        if (!register && !currentlyRegistered)
            return true;

        if (register && !CanRegisterShellExtension(taskExists))
            return false;

        if (!NativeHostInterop.BeginShellExtensionChange(register))
        {
            await ShowShellExtensionErrorAsync().ConfigureAwait(true);
            return false;
        }

        await WindowBusyOverlayService.RunAsync(async () =>
        {
            while (IsShellExtensionOperationInProgress())
                await Task.Delay(250).ConfigureAwait(false);
        }, showProgress: true).ConfigureAwait(true);

        // Успех — зарегистрирован хоть какой-то пункт меню (компактный или классический fallback
        // на Win11 без доверия сертификату); причина fallback остаётся в LastOpMessageId для страницы.
        var ok = IsBundledDllRegistered() == register;
        if (ok)
            NativeHostInterop.NotifyShellCachesAfterRegistrationChange();
        else
            await ShowShellExtensionErrorAsync().ConfigureAwait(true);

        return ok;
    }

    /// <summary>
    /// Один клик на странице «Система» (Windows 11): сертификат разработчика добавляется в
    /// LocalMachine\TrustedPeople тем же кодом, что и в установщике, после чего пункт меню
    /// перерегистрируется — sparse-пакет становится допустимым, классическую регистрацию helper
    /// снимает сам после успеха. Если переключатель выключен, перерегистрация не нужна: следующее
    /// включение сразу даст компактное меню. Ошибка показывается диалогом; ничего не откатывается.
    /// </summary>
    public static async Task<bool> TrustAuthorCertificateAsync(bool reregister)
    {
        if (IsShellExtensionOperationInProgress())
            return false;

        int result;
        var lastError = 0;
        try
        {
            result = NativeHostInterop.TrustShellSigner(out _, out lastError);
        }
        catch
        {
            result = (int)ShellSignerTrustResult.Unknown;
        }

        if (result is not ((int)ShellSignerTrustResult.Added or (int)ShellSignerTrustResult.AlreadyTrusted))
        {
            await ShowCertificateErrorAsync((ShellSignerTrustResult)result, lastError).ConfigureAwait(true);
            return false;
        }

        if (!reregister || !IsBundledDllRegistered())
            return true;

        return await SetShellExtensionRegisteredAsync(true).ConfigureAwait(true);
    }

    private static async Task ShowCertificateErrorAsync(ShellSignerTrustResult result, int lastError)
    {
        if (App.ShellWindow?.WindowXamlRoot is null)
            return;

        var reason = result switch
        {
            ShellSignerTrustResult.MsixMissing or ShellSignerTrustResult.Unsigned =>
                GlassSettingsResources.Get("System_ShellSparseNoticeMessage"),
            ShellSignerTrustResult.StoreFailed =>
                string.Format(GlassSettingsResources.Get("Error_ShellCertStoreFormat"), lastError),
            _ => string.Format(GlassSettingsResources.Get("Error_ShellCertResultFormat"), (int)result),
        };
        var dialog = new ContentDialog
        {
            Title = GlassSettingsResources.Get("Error_ShellCertTitle"),
            Content = GlassSettingsResources.Get("Error_ShellCertFailed") + " " + reason,
            CloseButtonText = GlassSettingsResources.Get("Dialog_Close"),
            XamlRoot = App.ShellWindow.WindowXamlRoot
        };
        _ = await dialog.ShowAsync();
    }

    private static bool GlassAutorunTaskExists() => GlassPlatformService.GlassAutorunTaskExists();

    private static async Task ShowShellExtensionErrorAsync()
    {
        var details = ResolveShellExtensionErrorMessage(NativeHostInterop.GetShellExtensionLastOpMessageId());
        if (string.IsNullOrWhiteSpace(details))
            details = GlassSettingsResources.Get("Error_ShellExtensionFailed");

        if (App.ShellWindow?.WindowXamlRoot is null)
            return;

        var dialog = new ContentDialog
        {
            Title = GlassSettingsResources.Get("Error_ShellExtensionTitle"),
            Content = details,
            CloseButtonText = GlassSettingsResources.Get("Dialog_Close"),
            XamlRoot = App.ShellWindow.WindowXamlRoot
        };

        _ = await dialog.ShowAsync();
    }

    private static string ResolveShellExtensionErrorMessage(int messageId) =>
        messageId switch
        {
            2 => GlassSettingsResources.Get("Error_ShellExtensionTimedOut"),
            3 => GlassSettingsResources.Get("Error_ShellExtensionInconsistentState"),
            4 => GlassSettingsResources.Get("Error_ShellExtensionFailedToStart"),
            5 => GlassSettingsResources.Get("Error_ShellExtensionFailedToApply"),
            6 => GlassSettingsResources.Get("Error_ShellExtensionNeedsExplorerRestart"),
            7 => GlassSettingsResources.Get("Error_ShellExtensionSparseArtifactsMissing"),
            8 => GlassSettingsResources.Get("Error_ShellExtensionSparseSignerNotTrusted"),
            9 => GlassSettingsResources.Get("Error_ShellExtensionSparseDeployFailed"),
            10 => GlassSettingsResources.Get("Error_ShellExtensionFailedToRegister"),
            11 => GlassSettingsResources.Get("Error_ShellExtensionFailedToUnregister"),
            12 => GlassSettingsResources.Get("Error_ShellExtensionSparseExternalLocationConflict"),
            _ => string.Empty,
        };
}

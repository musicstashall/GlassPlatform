using System.Runtime.InteropServices;
using System.Text;

namespace GlassSettings.Services;

/// <summary>Read-only MSI inventory. Settings never installs or uninstalls MSI.</summary>
public static class SecureUxThemeMsiService
{
    private const uint ErrorSuccess = 0;
    private const uint ErrorNoMoreItems = 259;
    private const uint InstallStateDefault = 5;
    private const string VersionStringProperty = "VersionString";

    private static readonly string[] UpgradeCodes =
    [
        "{965B5B74-4040-4E89-947F-AF8D99A76706}",
        "{3FF010A5-2E94-4621-A260-3A7ABCDDB37C}",
        "{E0E6C0EA-103B-4E8A-B1A9-1703853A49F4}"
    ];

    public static bool IsInstalled()
    {
        foreach (var upgrade in UpgradeCodes)
        {
            var product = new StringBuilder(39);
            if (MsiEnumRelatedProducts(upgrade, 0, 0, product) == 0)
                return true;
        }
        return false;
    }

    /// <summary>
    /// ProductVersion зарегистрированного MSI author-пакета. Для проверки версии
    /// FileVersion DLL не используется: канал обновлений SecureUxTheme — MSI release.
    /// Возвращает null, если продукт не установлен или версию нельзя прочитать.
    /// </summary>
    public static string? TryReadInstalledVersion()
    {
        string? best = null;
        foreach (var upgrade in UpgradeCodes)
        {
            for (uint index = 0; ; index++)
            {
                var product = new StringBuilder(39);
                var result = MsiEnumRelatedProducts(upgrade, 0, index, product);
                if (result == ErrorNoMoreItems)
                    break;
                if (result != ErrorSuccess)
                    return null;

                if (MsiQueryProductState(product.ToString()) != InstallStateDefault)
                    continue;

                var version = new StringBuilder(256);
                var size = (uint)version.Capacity;
                if (MsiGetProductInfo(product.ToString(), VersionStringProperty, version, ref size) != ErrorSuccess)
                    continue;
                if (string.IsNullOrWhiteSpace(version.ToString()))
                    continue;

                var candidate = version.ToString().Trim();
                if (best is null || GlassReleaseUpdateService.IsInstalledOlderThan(best, candidate))
                    best = candidate;
            }
        }

        return best;
    }

    [DllImport("msi.dll", CharSet = CharSet.Unicode, EntryPoint = "MsiEnumRelatedProductsW")]
    private static extern uint MsiEnumRelatedProducts(string upgradeCode, uint reserved, uint index, StringBuilder productCode);

    [DllImport("msi.dll", CharSet = CharSet.Unicode, EntryPoint = "MsiQueryProductStateW")]
    private static extern uint MsiQueryProductState(string productCode);

    [DllImport("msi.dll", CharSet = CharSet.Unicode, EntryPoint = "MsiGetProductInfoW")]
    private static extern uint MsiGetProductInfo(string productCode, string property, StringBuilder value, ref uint size);
}

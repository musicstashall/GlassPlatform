using System.Runtime.InteropServices;
using System.Text;

namespace GlassSettings.Services;

/// <summary>Read-only MSI inventory. Settings never installs or uninstalls MSI.</summary>
public static class SecureUxThemeMsiService
{
    public static bool IsInstalled()
    {
        foreach (var upgrade in new[] {
            "{965B5B74-4040-4E89-947F-AF8D99A76706}",
            "{3FF010A5-2E94-4621-A260-3A7ABCDDB37C}",
            "{E0E6C0EA-103B-4E8A-B1A9-1703853A49F4}" })
        {
            var product = new StringBuilder(39);
            if (MsiEnumRelatedProducts(upgrade, 0, 0, product) == 0)
                return true;
        }
        return false;
    }

    [DllImport("msi.dll", CharSet = CharSet.Unicode, EntryPoint = "MsiEnumRelatedProductsW")]
    private static extern uint MsiEnumRelatedProducts(string upgradeCode, uint reserved, uint index, StringBuilder productCode);
}

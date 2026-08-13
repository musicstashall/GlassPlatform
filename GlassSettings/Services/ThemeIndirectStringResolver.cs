using System.Runtime.InteropServices;
using System.Text;

namespace GlassSettings.Services;

/// <summary>Разбор строк вида @%SystemRoot%\...\file.dll,-123 из .theme.</summary>
internal static class ThemeIndirectStringResolver
{
    public static string Resolve(string rawValue)
    {
        if (string.IsNullOrWhiteSpace(rawValue))
            return string.Empty;

        var value = rawValue.Trim().Trim('"');
        if (!value.StartsWith('@'))
            return Environment.ExpandEnvironmentVariables(value);

        var expanded = Environment.ExpandEnvironmentVariables(value);
        var buffer = new StringBuilder(512);
        if (SHLoadIndirectString(expanded, buffer, (uint)buffer.Capacity, IntPtr.Zero) >= 0 &&
            buffer.Length > 0)
        {
            return buffer.ToString();
        }

        return Environment.ExpandEnvironmentVariables(value.TrimStart('@'));
    }

    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int SHLoadIndirectString(
        string pszSource,
        StringBuilder pszOutBuf,
        uint cchOutBuf,
        IntPtr ppvReserved);
}

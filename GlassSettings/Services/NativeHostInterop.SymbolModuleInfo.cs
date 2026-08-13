using System.Runtime.InteropServices;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    private const int MinPdbFileNameChars = 260;
    private const int MinSymbolIdentifierChars = 64;
    private const int MinDownloadUrlChars = 1024;

    public static int TryBuildSymbolModuleDownloadInfo(
        string modulePath,
        char[] pdbFileName,
        char[] symbolIdentifier,
        char[] downloadUrl)
    {
        if (pdbFileName.Length < MinPdbFileNameChars
            || symbolIdentifier.Length < MinSymbolIdentifierChars
            || downloadUrl.Length < MinDownloadUrlChars)
        {
            return unchecked((int)0x80070057); // E_INVALIDARG
        }

        return Native_TryBuildSymbolModuleDownloadInfo(
            modulePath,
            pdbFileName,
            (uint)pdbFileName.Length,
            symbolIdentifier,
            (uint)symbolIdentifier.Length,
            downloadUrl,
            (uint)downloadUrl.Length);
    }

    [DllImport(Dll, EntryPoint = "GlassSettings_TryBuildSymbolModuleDownloadInfo", CharSet = CharSet.Unicode)]
    private static extern int Native_TryBuildSymbolModuleDownloadInfo(
        string modulePath,
        [Out] char[] pdbFileName,
        uint cchPdbFileName,
        [Out] char[] symbolIdentifier,
        uint cchSymbolIdentifier,
        [Out] char[] downloadUrl,
        uint cchDownloadUrl);
}

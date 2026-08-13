namespace GlassSettings.Services;

/// <summary>PE CodeView/RSDS для symbol download — делегирует в GlassSettings.Native (порт донора).</summary>
internal static class PeSymbolModuleInfoReader
{
    private const int PdbFileNameBufferChars = 260;
    private const int SymbolIdentifierBufferChars = 64;
    private const int DownloadUrlBufferChars = 1024;

    internal readonly record struct ModuleDownloadInfo(
        string ModuleName,
        string ModulePath,
        string PdbFileName,
        string Identifier,
        string Url);

    internal static ModuleDownloadInfo? TryBuild(string modulePath, string moduleName)
    {
        var pdbBuffer = new char[PdbFileNameBufferChars];
        var identifierBuffer = new char[SymbolIdentifierBufferChars];
        var urlBuffer = new char[DownloadUrlBufferChars];

        var hr = NativeHostInterop.TryBuildSymbolModuleDownloadInfo(
            modulePath,
            pdbBuffer,
            identifierBuffer,
            urlBuffer);
        if (hr < 0)
            return null;

        var pdbFileName = NullTerminateTrim(pdbBuffer);
        var identifier = NullTerminateTrim(identifierBuffer);
        var url = NullTerminateTrim(urlBuffer);
        if (string.IsNullOrEmpty(pdbFileName)
            || string.IsNullOrEmpty(identifier)
            || string.IsNullOrEmpty(url))
        {
            return null;
        }

        return new ModuleDownloadInfo(moduleName, modulePath, pdbFileName, identifier, url);
    }

    private static string NullTerminateTrim(ReadOnlySpan<char> buffer)
    {
        var length = buffer.IndexOf('\0');
        if (length < 0)
            length = buffer.Length;

        return length == 0 ? string.Empty : buffer[..length].ToString();
    }
}

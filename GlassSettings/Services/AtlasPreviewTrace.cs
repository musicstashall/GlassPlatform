using System.Diagnostics;
using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>Подробный trace создания preview атласов (Output / Debug).</summary>
internal static class AtlasPreviewTrace
{
    private const string Prefix = "[AtlasPreview]";

    public static void CatalogStart()
    {
        Write("catalog: start");
        Write($"catalog: AppContext.BaseDirectory={AppContext.BaseDirectory}");
        Write($"catalog: process arch={RuntimeInformation.ProcessArchitecture}");
    }

    public static void CatalogNativeDll(string? path, bool exists)
    {
        Write($"catalog: GlassSettings.Native.dll path={(path ?? "<null>")}, exists={exists}");
        if (exists && path is not null)
        {
            try
            {
                var info = new FileInfo(path);
                Write($"catalog: GlassSettings.Native.dll size={info.Length}, mtime={info.LastWriteTime:O}");
            }
            catch (Exception ex)
            {
                Write($"catalog: GlassSettings.Native.dll metadata error: {ex.Message}");
            }
        }
    }

    public static void CatalogOpenGlass(string? dllPath, string? installRoot)
    {
        Write($"catalog: OpenGlass.dll={(dllPath ?? "<null>")}");
        Write($"catalog: installRoot={(installRoot ?? "<null>")}");
    }

    public static void CatalogSearchRoot(int index, string root) =>
        Write($"catalog: searchRoot[{index}]={root}");

    public static void CatalogProbe(string fullPath, bool exists, int fileCount)
    {
        Write($"catalog: probe dir={fullPath}, exists={exists}, images={fileCount}");
    }

    public static void CatalogResolved(string fullPath, int count)
    {
        Write($"catalog: resolved dir={fullPath}, count={count}");
        if (count == 0)
            Write("catalog: no atlas images found");
    }

    public static void CatalogItem(int index, string title, string filePath, bool hasLayout)
    {
        Write($"catalog: item[{index}] title={title}");
        Write($"catalog: item[{index}] file={filePath}");
        Write($"catalog: item[{index}] layout={(hasLayout ? "yes" : "NO")}");
    }

    public static void PageLoad(int itemCount) =>
        Write($"page: LoadAtlasImages items={itemCount}");

    public static void PagePopulate(int modelCount) =>
        Write($"page: PopulateAtlasCollection models={modelCount}");

    public static void GenerationStart(string title, string atlasPath) =>
        Write($"generate: start title={title}, atlas={atlasPath}");

    public static void GenerationStep(string step) =>
        Write($"generate: {step}");

    public static void GenerationHResult(int hr, string context)
    {
        Write($"generate: HRESULT context={context}, hr=0x{hr:X8}");
        if (hr != 0)
            Write($"generate: HRESULT message={DescribeHResult(hr)}");
    }

    public static void GenerationBytes(string title, int byteCount, string? tempPath = null)
    {
        Write($"generate: bytes title={title}, count={byteCount}, temp={(tempPath ?? "<none>")}");
    }

    public static void GenerationFailed(string title, string reason) =>
        Write($"generate: FAILED title={title}, reason={reason}");

    public static void DecodeStart(string title, int byteCount) =>
        Write($"decode: start title={title}, bytes={byteCount}");

    public static void DecodeComplete(string title, bool success) =>
        Write($"decode: complete title={title}, success={success}");

    public static void UiUpdate(string title, string state, string? detail = null)
    {
        var msg = $"ui: title={title}, state={state}";
        if (!string.IsNullOrWhiteSpace(detail))
            msg += $", detail={detail}";
        Write(msg);
    }

    public static void Exception(string context, Exception ex)
    {
        Write($"error: context={context}, type={ex.GetType().Name}, message={ex.Message}");
        Write($"error: stack={ex.StackTrace}");
    }

    private static string DescribeHResult(int hr)
    {
        try
        {
            var ex = Marshal.GetExceptionForHR(hr);
            return ex?.Message ?? "unknown";
        }
        catch
        {
            return "unknown";
        }
    }

    private static void Write(string message) =>
        Debug.WriteLine($"{Prefix} {message}");
}

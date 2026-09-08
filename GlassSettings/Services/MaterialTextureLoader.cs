using System.Runtime.InteropServices;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;

namespace GlassSettings.Services;

/// <summary>
/// Загрузка шумовой текстуры Acrylic (NoiseAsset) из системного
/// Windows.UI.Xaml.Controls.dll (ресурс ID 2000, RT_RCDATA).
/// Аналогичный механизм используется в OpenGlass (MaterialRealizer::LoadEffect).
/// </summary>
internal static class MaterialTextureLoader
{
    private const uint LOAD_LIBRARY_AS_IMAGE_RESOURCE = 0x00000020;
    private const uint RT_RCDATA = 10;
    private const int NOISE_RESOURCE_ID = 2000;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern nint LoadLibraryExW(string lpFileName, nint hFile, uint dwFlags);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern nint FindResourceW(nint hModule, nint lpName, nint lpType);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern nint LoadResource(nint hModule, nint hResource);

    [DllImport("kernel32.dll")]
    private static extern nint LockResource(nint hGlobal);

    [DllImport("kernel32.dll")]
    private static extern uint SizeofResource(nint hModule, nint hResource);

    private static ImageSource? s_cached;

    public static async System.Threading.Tasks.Task<ImageSource?> GetNoiseTextureAsync()
    {
        if (s_cached is not null)
            return s_cached;

        try
        {
            var hModule = LoadLibraryExW(
                "Windows.UI.Xaml.Controls.dll",
                nint.Zero,
                LOAD_LIBRARY_AS_IMAGE_RESOURCE);

            if (hModule == nint.Zero)
                return null;

            var hResource = FindResourceW(hModule, NOISE_RESOURCE_ID, (nint)RT_RCDATA);
            if (hResource == nint.Zero)
                return null;

            var hGlobal = LoadResource(hModule, hResource);
            if (hGlobal == nint.Zero)
                return null;

            var pData = LockResource(hGlobal);
            var size = SizeofResource(hModule, hResource);
            if (pData == nint.Zero || size == 0)
                return null;

            var pngBytes = new byte[size];
            Marshal.Copy(pData, pngBytes, 0, (int)size);

            using var stream = new InMemoryRandomAccessStream();
            using (var writer = new DataWriter(stream.GetOutputStreamAt(0)))
            {
                writer.WriteBytes(pngBytes);
                await writer.StoreAsync();
                await writer.FlushAsync();
            }

            stream.Seek(0);
            var decoder = await BitmapDecoder.CreateAsync(stream);
            var softwareBitmap = await decoder.GetSoftwareBitmapAsync(
                BitmapPixelFormat.Bgra8,
                BitmapAlphaMode.Premultiplied);

            var source = new SoftwareBitmapSource();
            await source.SetBitmapAsync(softwareBitmap);
            s_cached = source;
            return source;
        }
        catch
        {
            return null;
        }
    }
}

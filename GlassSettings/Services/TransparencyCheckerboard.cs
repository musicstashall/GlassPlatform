using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>Шахматная сетка для анализа прозрачности в preview-карточках.</summary>
public static class TransparencyCheckerboard
{
    private const int TileSize = 8;

    public static BitmapSource CreateImage(int width, int height)
    {
        var bitmap = new WriteableBitmap(width, height);
        using var stream = bitmap.PixelBuffer.AsStream();

        for (var row = 0; row < height; row++)
        {
            for (var col = 0; col < width; col++)
            {
                var isLight = ((col / TileSize) + (row / TileSize)) % 2 == 0;
                var color = isLight ? Colors.White : Color.FromArgb(255, 224, 224, 224);
                stream.WriteByte(color.B);
                stream.WriteByte(color.G);
                stream.WriteByte(color.R);
                stream.WriteByte(color.A);
            }
        }

        return bitmap;
    }
}

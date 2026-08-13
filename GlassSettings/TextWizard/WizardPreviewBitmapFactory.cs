using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using GlassSettings.Core.Layout;
using GlassSettings.Core.Text;
using Microsoft.UI.Xaml.Media.Imaging;

namespace GlassSettings.TextWizard;

/// <summary>Фоновая отрисовка preview-буферов для мастера (без UI).</summary>
internal static class WizardPreviewBitmapFactory
{
    public static int PreviewWidth => HeroPreviewLayoutMetrics.TextOverlayPaintWidth;

    public static int PreviewHeight => HeroPreviewLayoutMetrics.TextOverlayPaintHeight;

    public static byte[]? TryRenderBlockBuffer(
        WizardPreviewBlock block,
        string sampleText,
        bool isDarkTheme)
    {
        var buffer = new byte[PreviewWidth * PreviewHeight * 4];
        var paintHr = TextPreviewNative.PaintProfileToBuffer(
            buffer,
            PreviewWidth,
            PreviewHeight,
            sampleText,
            isDarkTheme,
            interactiveDuringGesture: false,
            block.RenderMode,
            block.Profile);
        if (paintHr < 0)
            return null;

        return buffer;
    }

    public static WriteableBitmap CreateBitmapFromBuffer(byte[] buffer)
    {
        var bitmap = new WriteableBitmap(PreviewWidth, PreviewHeight);
        using (var stream = bitmap.PixelBuffer.AsStream())
        {
            stream.Seek(0, SeekOrigin.Begin);
            stream.Write(buffer, 0, buffer.Length);
        }

        bitmap.Invalidate();
        return bitmap;
    }
}

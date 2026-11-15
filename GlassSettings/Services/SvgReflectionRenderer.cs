namespace GlassSettings.Services;

using System.Drawing.Imaging;
using System.Xml;
using System.Xml.Linq;
using Microsoft.UI.Xaml.Media.Imaging;

/// <summary>Restricted self-contained SVG, rasterized by SVG.NET for both preview and OG.</summary>
public static class SvgReflectionRenderer
{
    public const string CacheVersion = "svg-net-3.4.7-v1:";
    private const int MaximumSourceBytes = 262144;
    private static readonly HashSet<string> Elements = new(StringComparer.Ordinal)
    {
        "svg", "g", "defs", "path", "rect", "circle", "ellipse", "line", "polyline", "polygon",
        "linearGradient", "radialGradient", "stop", "clipPath", "title", "desc"
    };
    private static readonly HashSet<string> Attributes = new(StringComparer.Ordinal)
    {
        "id", "version", "viewBox", "width", "height", "preserveAspectRatio", "transform",
        "x", "y", "x1", "y1", "x2", "y2", "cx", "cy", "r", "rx", "ry", "fx", "fy",
        "d", "points", "fill", "fill-opacity", "fill-rule", "opacity", "stroke", "stroke-width",
        "stroke-opacity", "stroke-linecap", "stroke-linejoin", "stroke-dasharray", "clip-path",
        "clip-rule", "clipPathUnits", "gradientUnits", "gradientTransform", "spreadMethod",
        "offset", "stop-color", "stop-opacity"
    };

    public static bool IsPattern(string path) =>
        string.Equals(Path.GetExtension(path), ".svg", StringComparison.OrdinalIgnoreCase) &&
        string.Equals(Path.GetFileName(Path.GetDirectoryName(path)), "Pattern", StringComparison.OrdinalIgnoreCase);

    public static byte[] RenderPng(byte[] source, int width, int height)
    {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || (long)width * height > 33554432)
            throw new InvalidDataException("Unsupported SVG canvas size.");
        if (source.Length == 0 || source.Length > MaximumSourceBytes)
            throw new InvalidDataException("SVG source exceeds 256 KiB.");
        using var input = new MemoryStream(source, writable: false);
        using var reader = XmlReader.Create(input, new XmlReaderSettings
        {
            DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null,
            MaxCharactersInDocument = MaximumSourceBytes
        });
        var xml = XDocument.Load(reader);
        if (xml.DescendantNodes().OfType<XProcessingInstruction>().Any())
            throw new InvalidDataException("SVG processing instructions are not allowed.");
        XNamespace ns = "http://www.w3.org/2000/svg";
        if (xml.Root?.Name != ns + "svg" || xml.Root.Attribute("viewBox") is null)
            throw new InvalidDataException("SVG must have an SVG root and viewBox.");
        var nodes = xml.Root.DescendantsAndSelf().ToArray();
        if (nodes.Length > 512 || nodes.Any(node => node.Ancestors().Count() > 32))
            throw new InvalidDataException("SVG is too complex.");
        foreach (var node in nodes)
        {
            if (node.Name.Namespace != ns || !Elements.Contains(node.Name.LocalName))
                throw new InvalidDataException("Unsupported SVG element: " + node.Name);
            foreach (var attribute in node.Attributes())
            {
                if (attribute.IsNamespaceDeclaration) continue;
                if (attribute.Name.Namespace != XNamespace.None || !Attributes.Contains(attribute.Name.LocalName))
                    throw new InvalidDataException("Unsupported SVG attribute: " + attribute.Name);
                var value = attribute.Value.Trim();
                if (value.Contains("url", StringComparison.OrdinalIgnoreCase) &&
                    !(value.StartsWith("url(#", StringComparison.Ordinal) && value.EndsWith(')') &&
                      value.Count(c => c == '(') == 1 && !value.Contains(':')))
                    throw new InvalidDataException("Only local SVG paint references are allowed.");
            }
        }
        // Reparse only validated XML: no scripts, CSS, images, use/href, fonts, DTD or external resources.
        using var validated = new MemoryStream(System.Text.Encoding.UTF8.GetBytes(xml.ToString()));
        var document = Svg.SvgDocument.Open<Svg.SvgDocument>(validated);
        document.Width = new Svg.SvgUnit(width);
        document.Height = new Svg.SvgUnit(height);
        using var bitmap = document.Draw(width, height)
            ?? throw new InvalidDataException("SVG produced no bitmap.");
        if (bitmap.Width != width || bitmap.Height != height)
            throw new InvalidDataException("SVG renderer returned an unexpected canvas size.");
        using var output = new MemoryStream();
        bitmap.Save(output, ImageFormat.Png);
        return output.ToArray();
    }

    public static async Task<BitmapImage> CreatePreviewAsync(string path)
    {
        if (new FileInfo(path).Length > MaximumSourceBytes)
            throw new InvalidDataException("SVG source exceeds 256 KiB.");
        var source = await File.ReadAllBytesAsync(path);
        var png = await Task.Run(() => RenderPng(source, 148, 92));
        using var stream = new MemoryStream(png);
        using var random = stream.AsRandomAccessStream();
        var image = new BitmapImage();
        await image.SetSourceAsync(random);
        return image;
    }
}

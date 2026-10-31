using Microsoft.UI;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>
/// Единая политика цветов для индикаторов статуса процессов и подсистем.
/// Все цвета зафиксированы здесь — не дублировать в UI-коде.
/// </summary>
public static class ProcessStatusColors
{
    // ── Beacon (точки в списке процессов) ──

    /// <summary>Активный процесс (не отвергнут, модули загружены).</summary>
    public static Brush Active { get; } =
        new SolidColorBrush(Color.FromArgb(0xFF, 0x10, 0xC6, 0x0A)); // #10C60A

    /// <summary>Отвергнут (BlockList / Forbidden).</summary>
    public static Brush Rejected { get; } =
        new SolidColorBrush(Color.FromArgb(0xFF, 0xC4, 0x91, 0x00)); // #C49100

    /// <summary>Residual — модули в памяти после выгрузки (только при активной выгрузке).</summary>
    public static Brush Residual { get; } =
        new SolidColorBrush(Color.FromArgb(0xFF, 0xC4, 0x2B, 0x1C)); // #C42B1C

    // ── Subsystem dialog (диалог подсистем) ──

    /// <summary>Подсистема работает.</summary>
    public static Brush Running { get; } =
        new SolidColorBrush(Color.FromArgb(0xFF, 0x10, 0xC6, 0x0A)); // #10C60A

    /// <summary>Подсистема запускается / остановлена (нейтральное состояние).</summary>
    public static Brush Starting { get; } =
        new SolidColorBrush(Color.FromArgb(0xFF, 0xE6, 0xBE, 0x00)); // #E6BE00

    /// <summary>Подсистема останавливается.</summary>
    public static Brush Stopping { get; } =
        new SolidColorBrush(Color.FromArgb(0xFF, 0xA0, 0xA0, 0xA0)); // #A0A0A0

    // ── Нейтральный / неизвестный ──

    /// <summary>Серый — нет данных / не определено.</summary>
    public static Brush Neutral { get; } =
        new SolidColorBrush(Colors.Gray);
}

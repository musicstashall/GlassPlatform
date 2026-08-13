using System.Runtime.InteropServices;
using Microsoft.Win32;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>
/// Персонализация Windows: режим Light/Dark, прозрачность, акцент.
/// Запись в те же ключи, что Параметры Windows, + ImmersiveColorSet / UpdateCustomTheme.
/// </summary>
public static class WindowsPersonalizationService
{
    private const string PersonalizeKeyPath =
        @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize";

    private const string DesktopKeyPath = @"Control Panel\Desktop";
    private const string ExplorerAccentKeyPath =
        @"Software\Microsoft\Windows\CurrentVersion\Explorer\Accent";
    private const string DwmKeyPath = @"Software\Microsoft\Windows\DWM";

    public enum AppModeChoice
    {
        Light = 0,
        Dark = 1,
        Custom = 2
    }

    public enum AccentModeChoice
    {
        Automatic = 0,
        Manual = 1
    }

    public static AppModeChoice ReadAppModeChoice()
    {
        var appsLight = ReadDword(PersonalizeKeyPath, "AppsUseLightTheme", defaultValue: 1) != 0;
        var systemLight = ReadDword(PersonalizeKeyPath, "SystemUsesLightTheme", defaultValue: 1) != 0;
        if (appsLight == systemLight)
            return appsLight ? AppModeChoice.Light : AppModeChoice.Dark;

        return AppModeChoice.Custom;
    }

    public static bool ReadAppsUseLightTheme() =>
        ReadDword(PersonalizeKeyPath, "AppsUseLightTheme", defaultValue: 1) != 0;

    public static bool ReadSystemUsesLightTheme() =>
        ReadDword(PersonalizeKeyPath, "SystemUsesLightTheme", defaultValue: 1) != 0;

    public static void SetLightDarkMode(bool light)
    {
        WriteDword(PersonalizeKeyPath, "AppsUseLightTheme", light ? 1u : 0u);
        WriteDword(PersonalizeKeyPath, "SystemUsesLightTheme", light ? 1u : 0u);
        BroadcastImmersiveColorSet();
        _ = ThemeManager2.TryUpdateCustomTheme(out _);
    }

    public static void SetCustomModes(bool systemLight, bool appsLight)
    {
        WriteDword(PersonalizeKeyPath, "SystemUsesLightTheme", systemLight ? 1u : 0u);
        WriteDword(PersonalizeKeyPath, "AppsUseLightTheme", appsLight ? 1u : 0u);
        BroadcastImmersiveColorSet();
        _ = ThemeManager2.TryUpdateCustomTheme(out _);
    }

    public static bool ReadTransparencyEffects() =>
        ReadDword(PersonalizeKeyPath, "EnableTransparency", defaultValue: 1) != 0;

    public static void SetTransparencyEffects(bool enabled)
    {
        WriteDword(PersonalizeKeyPath, "EnableTransparency", enabled ? 1u : 0u);
        BroadcastImmersiveColorSet();
        _ = ThemeManager2.TryUpdateCustomTheme(out _);
    }

    public static AccentModeChoice ReadAccentMode()
    {
        var auto = ReadDword(DesktopKeyPath, "AutoColorization", defaultValue: 0) != 0;
        return auto ? AccentModeChoice.Automatic : AccentModeChoice.Manual;
    }

    public static void SetAccentMode(AccentModeChoice mode)
    {
        WriteDword(DesktopKeyPath, "AutoColorization", mode == AccentModeChoice.Automatic ? 1u : 0u);
        BroadcastImmersiveColorSet();
        _ = ThemeManager2.TryUpdateCustomTheme(out _);
    }

    /// <summary>Акцент на Пуск / панели задач — Personalize\ColorPrevalence.</summary>
    public static bool ReadStartTaskbarColorPrevalence() =>
        ReadDword(PersonalizeKeyPath, "ColorPrevalence", defaultValue: 0) != 0;

    public static void SetStartTaskbarColorPrevalence(bool enabled)
    {
        WriteDword(PersonalizeKeyPath, "ColorPrevalence", enabled ? 1u : 0u);
        BroadcastImmersiveColorSet();
        _ = ThemeManager2.TryUpdateCustomTheme(out _);
    }

    /// <summary>Акцент на заголовках и границах — DWM\ColorPrevalence.</summary>
    public static bool ReadTitleBarsColorPrevalence() =>
        ReadDword(DwmKeyPath, "ColorPrevalence", defaultValue: 0) != 0;

    public static void SetTitleBarsColorPrevalence(bool enabled)
    {
        WriteDword(DwmKeyPath, "ColorPrevalence", enabled ? 1u : 0u);
        BroadcastImmersiveColorSet();
        _ = ThemeManager2.TryUpdateCustomTheme(out _);
    }

    public static Color ReadAccentColor()
    {
        // Accent*=COLORREF; Colorization*=AARRGGBB (паритет HKCU / DwmGetColorizationColor).
        // После IThemeManager2 AccentColor иногда содержит тот же AARRGGBB dword —
        // сверяем с Colorization*, чтобы не перепутать R/B.
        var hasAccent = TryReadDword(DwmKeyPath, "AccentColor", out var accent);
        var hasColorization = TryReadDword(DwmKeyPath, "ColorizationColor", out var colorization);

        if (hasAccent)
        {
            var asColorRef = ThemeAccentColorHelper.ToOpaqueForeground(
                ThemeAccentColorHelper.FromColorRefDword(accent));
            if (hasColorization)
            {
                var colorizRgb = ThemeAccentColorHelper.ToOpaqueForeground(
                    ThemeAccentColorHelper.FromArgbDword(colorization));
                if (ThemeAccentColorHelper.RgbEquals(asColorRef, colorizRgb))
                    return asColorRef;

                var asArgb = ThemeAccentColorHelper.ToOpaqueForeground(
                    ThemeAccentColorHelper.FromArgbDword(accent));
                if (ThemeAccentColorHelper.RgbEquals(asArgb, colorizRgb))
                    return asArgb;
            }

            return asColorRef;
        }

        if (hasColorization)
        {
            return ThemeAccentColorHelper.ToOpaqueForeground(
                ThemeAccentColorHelper.FromArgbDword(colorization));
        }

        return Color.FromArgb(255, 0, 120, 215);
    }

    /// <summary>
    /// Акцент, выбранный в Параметрах: Explorer\Accent\AccentPalette[3] (RGBA),
    /// не DWM\AccentColor (там часто производный оттенок для chrome).
    /// </summary>
    public static Color ReadUserAccentColor()
    {
        if (TryReadAccentPalettePrimary(out var fromPalette))
            return fromPalette;

        if (TryReadDword(ColorHistoryKeyPath, "ColorHistory0", out var history0))
        {
            return ThemeAccentColorHelper.ToOpaqueForeground(
                ThemeAccentColorHelper.FromColorRefDword(history0));
        }

        return ReadAccentColor();
    }

    /// <summary>Select в палитре — по user accent (AccentPalette[3]), как в Параметрах.</summary>
    public static Color? ResolveRecentPaletteSelection(IReadOnlyList<Color> recent)
    {
        if (recent is null || recent.Count == 0)
            return null;

        var userAccent = ReadUserAccentColor();
        foreach (var color in recent)
        {
            if (ThemeAccentColorHelper.RgbEquals(color, userAccent))
                return color;
        }

        return null;
    }

    private const string ColorHistoryKeyPath =
        @"Software\Microsoft\Windows\CurrentVersion\Themes\History\Colors";

    /// <summary>В реестре до 10 слотов; Параметры показывают 5.</summary>
    private const int MaxRecentAccentColors = 10;

    private const int RecentAccentColorsUiCount = 5;

    /// <summary>
    /// Недавние акценты из Themes\History\Colors\ColorHistory0..4 (COLORREF), как в Параметрах.
    /// </summary>
    public static IReadOnlyList<Color> ReadRecentAccentColors()
    {
        var list = new List<Color>(RecentAccentColorsUiCount);
        for (var i = 0; i < MaxRecentAccentColors; i++)
        {
            if (!TryReadDword(ColorHistoryKeyPath, $"ColorHistory{i}", out var dword))
                continue;

            if (list.Count >= RecentAccentColorsUiCount)
                break;

            var color = ThemeAccentColorHelper.ToOpaqueForeground(
                ThemeAccentColorHelper.FromColorRefDword(dword));
            if (list.Exists(c => ThemeAccentColorHelper.RgbEquals(c, color)))
                continue;

            list.Add(color);
        }

        return list;
    }

    /// <summary>AccentPalette: 8×RGBA; индекс 3 — выбранный акцент (как в Параметрах).</summary>
    private static bool TryReadAccentPalettePrimary(out Color color)
    {
        color = default;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(ExplorerAccentKeyPath);
            if (key?.GetValue("AccentPalette") is not byte[] palette || palette.Length < 16)
                return false;

            // bytes 12..14 = RGB of slot 3
            color = Color.FromArgb(255, palette[12], palette[13], palette[14]);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Вставить цвет в ColorHistory0 и сдвинуть 0→1→…→9 (COLORREF).</summary>
    public static void PushRecentAccentColor(Color color)
    {
        var opaque = ThemeAccentColorHelper.ToOpaqueForeground(color);
        var packed = ThemeAccentColorHelper.ToColorRefDword(opaque, alpha: 0);

        var existing = new List<uint>(MaxRecentAccentColors);
        for (var i = 0; i < MaxRecentAccentColors; i++)
        {
            if (!TryReadDword(ColorHistoryKeyPath, $"ColorHistory{i}", out var dword))
                continue;

            var entry = ThemeAccentColorHelper.ToOpaqueForeground(
                ThemeAccentColorHelper.FromColorRefDword(dword));
            if (ThemeAccentColorHelper.RgbEquals(entry, opaque))
                continue;

            existing.Add(ThemeAccentColorHelper.ToColorRefDword(entry, alpha: 0));
        }

        WriteDword(ColorHistoryKeyPath, "ColorHistory0", packed);
        for (var i = 0; i < MaxRecentAccentColors - 1; i++)
        {
            if (i < existing.Count)
                WriteDword(ColorHistoryKeyPath, $"ColorHistory{i + 1}", existing[i]);
            else
                DeleteDword(ColorHistoryKeyPath, $"ColorHistory{i + 1}");
        }
    }

    /// <summary>
    /// Применить акцент: DWM + Explorer\Accent + ImmersiveColorSet + UpdateCustomTheme
    /// (паритет записи системных Параметров; themeui дописывает связанные ключи).
    /// Тяжёлый путь — на STA-фоне через <see cref="ApplyAccentColorAsync"/>.
    /// </summary>
    public static void SetAccentColor(Color color) => ApplyAccentColorCore(color);

    /// <summary>Полный apply акцента на STA-потоке (COM themeui + broadcast).</summary>
    public static Task ApplyAccentColorAsync(Color color)
    {
        return Task.Run(() =>
        {
            Exception? error = null;
            var thread = new Thread(() =>
            {
                try
                {
                    ApplyAccentColorCore(color);
                }
                catch (Exception ex)
                {
                    error = ex;
                }
            });
            thread.SetApartmentState(ApartmentState.STA);
            thread.IsBackground = true;
            thread.Start();
            thread.Join();

            if (error is not null)
                System.Diagnostics.Debug.WriteLine($"GlassSettings ApplyAccentColorAsync: {error}");
        });
    }

    private static void ApplyAccentColorCore(Color color)
    {
        // Manual без отдельного broadcast — один wave в конце apply.
        WriteDword(DesktopKeyPath, "AutoColorization", 0);

        var r = color.R;
        var g = color.G;
        var b = color.B;
        WriteAccentRegistryValues(color);

        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(ExplorerAccentKeyPath);
            key?.SetValue("AccentPalette", BuildAccentPalette(r, g, b), RegistryValueKind.Binary);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings SetAccentColor AccentPalette: {ex}");
        }

        BroadcastImmersiveColorSet();
        _ = ThemeManager2.TryUpdateCustomTheme(out _);
        _ = ThemeManager2.TryUpdateColorSettingsForLogonUi(out _);
        // COM мог сдвинуть ключи — восстанавливаем Accent*=COLORREF / Colorization*=AARRGGBB.
        WriteAccentRegistryValues(color);
        PushRecentAccentColor(color);
        Core.Platform.LiveReloadScheduler.NotifyDwmColorizationNow();
    }

    private static void WriteAccentRegistryValues(Color color)
    {
        // Эталон HKCU: Accent*/Menu = COLORREF, Colorization* = AARRGGBB (как .theme).
        var accentColorRef = ThemeAccentColorHelper.ToColorRefDword(color);
        var colorizationArgb = ThemeAccentColorHelper.ToArgbDword(color, alpha: 0xC4);

        WriteDword(DwmKeyPath, "AccentColor", accentColorRef);
        WriteDword(DwmKeyPath, "ColorizationColor", colorizationArgb);
        WriteDword(DwmKeyPath, "ColorizationAfterglow", colorizationArgb);
        WriteDword(DwmKeyPath, "ColorizationColorBalance", 89);
        WriteDword(DwmKeyPath, "ColorizationAfterglowBalance", 10);
        WriteDword(DwmKeyPath, "ColorizationBlurBalance", 1);
        // ColorPrevalence (заголовки) — отдельный тумблер; apply цвета не трогает.

        WriteDword(ExplorerAccentKeyPath, "AccentColorMenu", accentColorRef);
        WriteDword(ExplorerAccentKeyPath, "StartColorMenu", accentColorRef);
    }

    private static byte[] BuildAccentPalette(byte r, byte g, byte b)
    {
        // 8×RGBA (A=0); индекс 3 — выбранный акцент (паритет живого AccentPalette).
        static byte[] Rgba(byte rr, byte gg, byte bb, byte aa = 0x00) => [rr, gg, bb, aa];

        byte Darken(byte c, double f) => (byte)Math.Clamp((int)(c * f), 0, 255);
        byte Lighten(byte c, double f) => (byte)Math.Clamp((int)(c + (255 - c) * f), 0, 255);

        var parts = new List<byte>(32);
        void Add(byte[] rgba) => parts.AddRange(rgba);

        Add(Rgba(Lighten(r, 0.55), Lighten(g, 0.55), Lighten(b, 0.55)));
        Add(Rgba(Lighten(r, 0.35), Lighten(g, 0.35), Lighten(b, 0.35)));
        Add(Rgba(Lighten(r, 0.15), Lighten(g, 0.15), Lighten(b, 0.15)));
        Add(Rgba(r, g, b)); // [3] — user accent
        Add(Rgba(Darken(r, 0.8), Darken(g, 0.8), Darken(b, 0.8)));
        Add(Rgba(Darken(r, 0.55), Darken(g, 0.55), Darken(b, 0.55)));
        Add(Rgba(Darken(r, 0.3), Darken(g, 0.3), Darken(b, 0.3)));
        Add(Rgba(0x00, 0x00, 0x00, 0x00));
        return parts.ToArray();
    }

    private static void BroadcastImmersiveColorSet()
    {
        const int hwndBroadcast = 0xFFFF;
        const uint wmSettingChange = 0x001A;
        const uint smtoAbortIfHung = 0x0002;
        _ = SendMessageTimeoutW(
            new IntPtr(hwndBroadcast),
            wmSettingChange,
            IntPtr.Zero,
            "ImmersiveColorSet",
            smtoAbortIfHung,
            1000,
            out _);
    }

    private static uint ReadDword(string subKey, string name, uint defaultValue) =>
        TryReadDword(subKey, name, out var value) ? value : defaultValue;

    private static bool TryReadDword(string subKey, string name, out uint value)
    {
        value = 0;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(subKey);
            var raw = key?.GetValue(name);
            if (raw is int i)
            {
                value = unchecked((uint)i);
                return true;
            }

            if (raw is long l)
            {
                value = unchecked((uint)l);
                return true;
            }
        }
        catch
        {
            /* ignore */
        }

        return false;
    }

    private static void WriteDword(string subKey, string name, uint value)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(subKey);
            key?.SetValue(name, unchecked((int)value), RegistryValueKind.DWord);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings WriteDword {subKey}\\{name}: {ex}");
        }
    }

    private static void DeleteDword(string subKey, string name)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(subKey, writable: true);
            key?.DeleteValue(name, throwOnMissingValue: false);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings DeleteDword {subKey}\\{name}: {ex}");
        }
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern IntPtr SendMessageTimeoutW(
        IntPtr hWnd,
        uint msg,
        IntPtr wParam,
        string lParam,
        uint flags,
        uint timeoutMs,
        out IntPtr result);
}

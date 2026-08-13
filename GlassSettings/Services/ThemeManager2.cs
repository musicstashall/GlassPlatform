using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

namespace GlassSettings.Services;

/// <summary>
/// Неофициальный themeui IThemeManager2 — применение .theme pack (обои, звуки, иконки, курсоры).
/// См. namazso / SecureUxTheme ThemeLib.
/// </summary>
internal static class ThemeManager2
{
    private static readonly Guid ClsidThemeManager2 = new("9324da94-50ec-4a14-a770-e90ca03e7c8f");
    private static readonly Guid IidThemeManager2 = typeof(IThemeManager2).GUID;

    private const uint ClsctxInprocServer = 0x1;
    private const uint ClsctxLocalServer = 0x4;
    private const uint ClsctxRemoteServer = 0x10;
    private const uint ClsctxAll = ClsctxInprocServer | ClsctxLocalServer | ClsctxRemoteServer;

    [Flags]
    internal enum InitializationFlags
    {
        ThemeInitNoFlags = 0,
        ThemeInitCurrentThemeOnly = 1 << 0,
    }

    [Flags]
    internal enum ThemeApplyFlags
    {
        None = 0,
        IgnoreBackground = 1 << 0,
        IgnoreCursor = 1 << 1,
        IgnoreDesktopIcons = 1 << 2,
        IgnoreColor = 1 << 3,
        IgnoreSound = 1 << 4,
        IgnoreScreensaver = 1 << 5,
        UnknownMetrics = 1 << 6,
        Unknown2 = 1 << 7,
        NoHourglass = 1 << 8,
    }

    [Flags]
    internal enum ThemePackFlags
    {
        None = 0,
        Unknown1 = 1 << 0,
        Unknown2 = 1 << 1,
        Silent = 1 << 2,
        Roamed = 1 << 3,
    }

    [Guid("c1e8c83e-845d-4d95-81db-e283fdffc000")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [ComImport]
    private interface IThemeManager2
    {
        void Init(InitializationFlags initFlags);
        void InitAsync(IntPtr hwnd, int unk1);
        void Refresh();
        void RefreshAsync(IntPtr hwnd, int unk1);
        void RefreshComplete();
        void GetThemeCount(out int count);
        void GetTheme(int index, out ITheme theme);
        void IsThemeDisabled(int index, out int disabled);
        void GetCurrentTheme(out int index);

        void SetCurrentTheme(
            IntPtr parent,
            int themeIndex,
            int applyNow,
            ThemeApplyFlags applyFlags,
            ThemePackFlags packFlags);

        void GetCustomTheme(out int index);
        void GetDefaultTheme(out int index);
        void CreateThemePack(IntPtr hwnd, string unk1, ThemePackFlags packFlags);

        /// <summary>LPWSTR* path — CoTaskMemAlloc.</summary>
        void CloneAndSetCurrentTheme(IntPtr hwnd, [MarshalAs(UnmanagedType.LPWStr)] string name, out IntPtr path);

        void InstallThemePack(
            IntPtr hwnd,
            string unk1,
            int unk2,
            ThemePackFlags packFlags,
            out IntPtr unk3,
            out ITheme unk4);

        void DeleteTheme(string unk1);
        void OpenTheme(IntPtr hwnd, string path, ThemePackFlags packFlags);

        void AddAndSelectTheme(
            IntPtr hwnd,
            string path,
            ThemeApplyFlags applyFlags,
            ThemePackFlags packFlags);

        void SQMCurrentTheme();
        void ExportRoamingThemeToStream(IStream stream, int unk1);
        void ImportRoamingThemeFromStream(IStream stream, int unk1);
        void UpdateColorSettingsForLogonUI();
        void GetDefaultThemeId(out Guid guid);
        void UpdateCustomTheme();
    }

    [Guid("26e4185f-0528-475f-acaf-abe89ba6017d")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [ComImport]
    private interface ITheme
    {
        string DisplayName { get; set; }
        string VisualStyle1 { get; set; }
        string VisualStyle2 { get; set; }
    }

    /// <summary>
    /// Применяет .theme через ThemeManager. applyFlags=0 — полный pack; Silent — без диалогов.
    /// </summary>
    public static bool TryAddAndSelectTheme(string themeFilePath, out int hresult)
    {
        return WithManager(out hresult, manager =>
        {
            manager.AddAndSelectTheme(
                IntPtr.Zero,
                themeFilePath,
                ThemeApplyFlags.IgnoreScreensaver,
                ThemePackFlags.Silent);
        }, requireThemePath: themeFilePath);
    }

    /// <summary>Синхронизирует Custom.theme с текущими настройками персонализации.</summary>
    public static bool TryUpdateCustomTheme(out int hresult) =>
        WithManager(out hresult, manager => manager.UpdateCustomTheme());

    public static bool TryUpdateColorSettingsForLogonUi(out int hresult) =>
        WithManager(out hresult, manager => manager.UpdateColorSettingsForLogonUI());

    /// <summary>
    /// Сохраняет текущую (Custom) тему под именем — паритет «Сохранить тему» в Персонализации.
    /// </summary>
    public static bool TryCloneAndSetCurrentTheme(
        IntPtr ownerHwnd,
        string themeDisplayName,
        out string? themeFilePath,
        out int hresult)
    {
        themeFilePath = null;
        hresult = unchecked((int)0x80004005);
        if (string.IsNullOrWhiteSpace(themeDisplayName))
        {
            hresult = unchecked((int)0x80070057);
            return false;
        }

        object? comObject = null;
        var pathPtr = IntPtr.Zero;
        try
        {
            var hr = CoCreateInstance(
                ClsidThemeManager2,
                IntPtr.Zero,
                ClsctxAll,
                IidThemeManager2,
                out comObject);
            hresult = hr;
            if (hr < 0 || comObject is null)
                return false;

            var manager = (IThemeManager2)comObject;
            manager.Init(InitializationFlags.ThemeInitNoFlags);
            manager.CloneAndSetCurrentTheme(ownerHwnd, themeDisplayName.Trim(), out pathPtr);
            if (pathPtr != IntPtr.Zero)
            {
                themeFilePath = Marshal.PtrToStringUni(pathPtr);
                Marshal.FreeCoTaskMem(pathPtr);
                pathPtr = IntPtr.Zero;
            }

            hresult = 0;
            return !string.IsNullOrWhiteSpace(themeFilePath);
        }
        catch (COMException ex)
        {
            hresult = ex.HResult;
            return false;
        }
        catch (Exception)
        {
            return false;
        }
        finally
        {
            if (pathPtr != IntPtr.Zero)
                Marshal.FreeCoTaskMem(pathPtr);

            if (comObject is not null && Marshal.IsComObject(comObject))
                Marshal.FinalReleaseComObject(comObject);
        }
    }

    private static bool WithManager(out int hresult, Action<IThemeManager2> action, string? requireThemePath = null)
    {
        hresult = unchecked((int)0x80004005);
        if (requireThemePath is not null
            && (string.IsNullOrWhiteSpace(requireThemePath) || !File.Exists(requireThemePath)))
        {
            hresult = unchecked((int)0x80070002);
            return false;
        }

        object? comObject = null;
        try
        {
            var hr = CoCreateInstance(
                ClsidThemeManager2,
                IntPtr.Zero,
                ClsctxAll,
                IidThemeManager2,
                out comObject);
            hresult = hr;
            if (hr < 0 || comObject is null)
                return false;

            var manager = (IThemeManager2)comObject;
            manager.Init(InitializationFlags.ThemeInitNoFlags);
            action(manager);
            hresult = 0;
            return true;
        }
        catch (COMException ex)
        {
            hresult = ex.HResult;
            return false;
        }
        catch (Exception)
        {
            return false;
        }
        finally
        {
            if (comObject is not null && Marshal.IsComObject(comObject))
                Marshal.FinalReleaseComObject(comObject);
        }
    }

    [DllImport("ole32.dll", ExactSpelling = true)]
    private static extern int CoCreateInstance(
        [In, MarshalAs(UnmanagedType.LPStruct)] Guid rclsid,
        IntPtr pUnkOuter,
        uint dwClsContext,
        [In, MarshalAs(UnmanagedType.LPStruct)] Guid riid,
        [MarshalAs(UnmanagedType.IUnknown)] out object? ppv);
}

using System.Runtime.InteropServices;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    public const int ImageFileMachineI386 = 0x014C;
    public const int ImageFileMachineAmd64 = 0x8664;
    public const int ImageFileMachineArm64 = 0xAA64;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SecureUxThemeNativeQuery
    {
        public int Wow64Process;
        public int NativeMachine;
        public int ProcessMachine;
        public int LocalDllPresent;
        public int LocalDllMachine;
        public int LocalDllArchMatchesOs;
        public int SystemDll4Present;
        public int SystemLegacyDllPresent;
        public int ThemeUiProxyPresent;
        public int LoadedEventPresent;
        public uint WinlogonGlobalFlag;
        public uint ExplorerGlobalFlag;
        public uint SettingsGlobalFlag;
        public uint LogonUiGlobalFlag;
        public uint DwmGlobalFlag;
        public int WinlogonHasOurToken;
        public int WinlogonHasLegacyExact;
        public int ExplorerHasLegacyExact;
        public int SettingsHasLegacyExact;
        public int LogonUiHasLegacyExact;
        public int DwmHasLegacyExact;
        public int OfficialMsiPresent;
        public int OfficialNameMatches;
        public int OfficialPublisherMatches;
        public int ComProxyHijacked;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string LocalDllPath;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string SystemDll4Path;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string SystemLegacyDllPath;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string OfficialVersion;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string OfficialProductName;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string OfficialPublisher;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)]
        public string WinlogonVerifierDlls;
    }

    public static int SecureUxThemeQuery(out SecureUxThemeNativeQuery query)
    {
        query = default;
        return Native_SecureUxThemeQuery(out query);
    }

    public static int SecureUxThemeActivate(string payloadFolder, bool reactivate = false) =>
        Native_SecureUxThemeActivate(payloadFolder, reactivate ? 1 : 0);

    public static int SecureUxThemeDeactivate() =>
        Native_SecureUxThemeDeactivate();

    public static bool SecureUxThemeEnsureLoaded() =>
        Native_SecureUxThemeEnsureLoaded();

    public static bool SecureUxThemeCheckCleanupPending() =>
        Native_SecureUxThemeCheckCleanupPending();

    public static bool SecureUxThemeTryDeleteSystemFiles() =>
        Native_SecureUxThemeTryDeleteSystemFiles();

    [DllImport(Dll, EntryPoint = "GlassSettings_SecureUxThemeQuery", CharSet = CharSet.Unicode)]
    private static extern int Native_SecureUxThemeQuery(out SecureUxThemeNativeQuery query);

    [DllImport(Dll, EntryPoint = "GlassSettings_SecureUxThemeActivate", CharSet = CharSet.Unicode)]
    private static extern int Native_SecureUxThemeActivate(string payloadFolder, int reactivate);

    [DllImport(Dll, EntryPoint = "GlassSettings_SecureUxThemeDeactivate")]
    private static extern int Native_SecureUxThemeDeactivate();

    [DllImport(Dll, EntryPoint = "GlassSettings_SecureUxThemeEnsureLoaded")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_SecureUxThemeEnsureLoaded();

    [DllImport(Dll, EntryPoint = "GlassSettings_SecureUxThemeCheckCleanupPending")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_SecureUxThemeCheckCleanupPending();

    [DllImport(Dll, EntryPoint = "GlassSettings_SecureUxThemeTryDeleteSystemFiles")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_SecureUxThemeTryDeleteSystemFiles();
}

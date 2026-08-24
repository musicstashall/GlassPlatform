using System.Runtime.InteropServices;
using System.Text;
using GlassSettings.Core.Platform;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    public const int InjectMapSlotCount = 512;
    public const int InjectExeNameChars = 64;
    /// <summary>Р”РѕР»Р¶РµРЅ СЃРѕРІРїР°РґР°С‚СЊ СЃ sizeof(Shared::InjectProcessMap::Slot) v2 = 164.</summary>
    public const int InjectMapSlotStride = 164;
    /// <summary>Р”РѕР»Р¶РµРЅ СЃРѕРІРїР°РґР°С‚СЊ СЃ sizeof(Shared::InjectModuleScan::Result) = 136.</summary>
    public const int InjectScanResultStride = 136;

    public enum InjectKind : byte
    {
        None = 0,
        ActiveGui = 1,
        RejectedBlockList = 2,
        RejectedForbidden = 3,
    }

    public enum InjectState : byte
    {
        Empty = 0,
        Alive = 1,
        Unloading = 2,
        Unloaded = 3,
    }

    public enum InjectArch : byte
    {
        X64 = 0,
        X86 = 1,
    }

    [Flags]
    public enum InjectModuleBits : uint
    {
        Compat = 1u << 0,
        Core = 1u << 1,
        Wnd = 1u << 2,
        Explorer = 1u << 3,
        Taskbar = 1u << 4,
        UICompat = 1u << 5,
    }

    [Flags]
    public enum InjectDiagFlags : uint
    {
        Hooks = 1u << 0,
        PaintDispatcher = 1u << 1,
        ModuleHandler = 1u << 2,
        Explorer = 1u << 3,
        Taskbar = 1u << 4,
        WinEventProc = 1u << 5,
        CrashGuard = 1u << 6,
        UICompat = 1u << 7,
    }

    public struct InjectMapSlot
    {
        public int Seq;
        public uint Pid;
        public InjectKind Kind;
        public byte Path;
        public InjectState State;
        public uint ModuleBits;
        public uint UnloadResult;
        public uint DiagFlags;
        public uint SubclassWindowTotal;
        public ulong SubclassKindMask;
        public string ExeName;
        public InjectArch Arch;
    }

    public struct InjectScanResult
    {
        public uint Pid;
        public uint ModuleBits;
        public string ExeName;
    }

    public static bool IsInjectMapAvailable() => Native_IsInjectMapAvailable();

    public static bool RequestProcessUnload(uint pid) => Native_RequestProcessUnload(pid);

    public static bool ForceUnloadProcess(uint pid) => Native_ForceUnloadProcess(pid);

    public static bool IsX86ForceUnloadAgentAvailable() => Native_IsX86ForceUnloadAgentAvailable();

    public static string? TryGetAutorunGlassCompatInstallRoot(GlassPlatformArch arch)
    {
        var sb = new StringBuilder(1024);
        if (!Native_TryGetAutorunGlassCompatInstallRootForArch((int)arch, sb, (uint)sb.Capacity))
            return null;
        var path = sb.ToString().Trim().TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        return string.IsNullOrEmpty(path) ? null : path;
    }

    public static bool HasShellInjectResiduals() =>
        HasShellInjectResiduals(GlassPlatformNames.NativeArch);

    public static bool HasShellInjectResiduals(GlassPlatformArch arch) =>
        Native_HasShellInjectResidualsForArch((int)arch);

    public enum InjectLifecycleUi : int
    {
        Idle = 0,
        Running = 1,
        Stopping = 2,
        Residual = 3,
    }

    public static InjectLifecycleUi QueryInjectLifecycleUi(GlassPlatformArch arch) =>
        (InjectLifecycleUi)Native_QueryInjectLifecycleUiForArch((int)arch);

    /// <summary>Скан known hosts → sticky; true если residual bits.</summary>
    public static bool RefreshInjectResidualSticky(GlassPlatformArch arch) =>
        Native_RefreshInjectResidualStickyForArch((int)arch);

    /// <summary>Убрать слот из карты указанной разрядности (мёртвый PID после exit).</summary>
    public static bool ClearInjectMapPidForArch(InjectArch arch, uint pid) =>
        pid != 0 && Native_ClearInjectMapPidForArch((uint)arch, pid);

    public static bool TryReadInjectMapSlots(out InjectMapSlot[] slots)
    {
        var list = new List<InjectMapSlot>(64);
        var ok64 = TryReadArch(InjectArch.X64, list);
        var ok86 = TryReadArch(InjectArch.X86, list);
        if (ok64 | ok86)
        {
            slots = list.ToArray();
            return true;
        }

        slots = Array.Empty<InjectMapSlot>();
        return false;
    }

    private static bool TryReadArch(InjectArch arch, List<InjectMapSlot> into)
    {
        var bytes = new byte[InjectMapSlotCount * InjectMapSlotStride];
        var pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try
        {
            uint count = 0;
            if (!Native_TryReadInjectMapSlotsForArch(
                    (uint)arch,
                    pin.AddrOfPinnedObject(),
                    InjectMapSlotCount,
                    ref count))
            {
                return false;
            }

            var n = (int)Math.Min(count, InjectMapSlotCount);
            for (var i = 0; i < n; i++)
            {
                var slot = ParseMapSlot(bytes, i * InjectMapSlotStride);
                slot.Arch = arch;
                into.Add(slot);
            }

            return true;
        }
        finally
        {
            pin.Free();
        }
    }

    public static bool ScanGlassModules(uint[]? pids, out InjectScanResult[] results)
    {
        results = Array.Empty<InjectScanResult>();
        var bytes = new byte[InjectMapSlotCount * InjectScanResultStride];
        var pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try
        {
            uint count = 0;
            bool ok;
            if (pids is { Length: > 0 })
            {
                ok = Native_ScanGlassModules(
                    pids,
                    (uint)pids.Length,
                    pin.AddrOfPinnedObject(),
                    InjectMapSlotCount,
                    ref count);
            }
            else
            {
                ok = Native_ScanGlassModules(
                    null,
                    0,
                    pin.AddrOfPinnedObject(),
                    InjectMapSlotCount,
                    ref count);
            }

            if (!ok)
                return false;

            if (count == 0)
                return true;

            var n = (int)Math.Min(count, InjectMapSlotCount);
            results = new InjectScanResult[n];
            for (var i = 0; i < n; i++)
                results[i] = ParseScanResult(bytes, i * InjectScanResultStride);
            return true;
        }
        finally
        {
            pin.Free();
        }
    }

    private static InjectMapSlot ParseMapSlot(byte[] bytes, int offset)
    {
        // Layout: seq(4) pid(4) kind/path/state/res(4) moduleBits(4) unload(4)
        //         diag(4) subclassTotal(4) kindMask(8) exeName(128)
        return new InjectMapSlot
        {
            Seq = BitConverter.ToInt32(bytes, offset + 0),
            Pid = BitConverter.ToUInt32(bytes, offset + 4),
            Kind = (InjectKind)bytes[offset + 8],
            Path = bytes[offset + 9],
            State = (InjectState)bytes[offset + 10],
            ModuleBits = BitConverter.ToUInt32(bytes, offset + 12),
            UnloadResult = BitConverter.ToUInt32(bytes, offset + 16),
            DiagFlags = BitConverter.ToUInt32(bytes, offset + 20),
            SubclassWindowTotal = BitConverter.ToUInt32(bytes, offset + 24),
            SubclassKindMask = BitConverter.ToUInt64(bytes, offset + 28),
            ExeName = ReadFixedUnicode(bytes, offset + 36, InjectExeNameChars),
        };
    }

    private static InjectScanResult ParseScanResult(byte[] bytes, int offset)
    {
        return new InjectScanResult
        {
            Pid = BitConverter.ToUInt32(bytes, offset + 0),
            ModuleBits = BitConverter.ToUInt32(bytes, offset + 4),
            ExeName = ReadFixedUnicode(bytes, offset + 8, InjectExeNameChars),
        };
    }

    private static string ReadFixedUnicode(byte[] bytes, int offset, int charCount)
    {
        var byteLen = charCount * 2;
        if (offset + byteLen > bytes.Length)
            byteLen = Math.Max(0, bytes.Length - offset);
        var s = Encoding.Unicode.GetString(bytes, offset, byteLen);
        var z = s.IndexOf('\0');
        return z >= 0 ? s[..z] : s;
    }

    [DllImport(Dll, EntryPoint = "GlassSettings_IsInjectMapAvailable")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_IsInjectMapAvailable();

    [DllImport(Dll, EntryPoint = "GlassSettings_TryReadInjectMapSlotsForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_TryReadInjectMapSlotsForArch(
        uint arch,
        IntPtr outSlots,
        uint maxSlots,
        ref uint outCount);

    [DllImport(Dll, EntryPoint = "GlassSettings_RequestProcessUnload")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_RequestProcessUnload(uint pid);

    [DllImport(Dll, EntryPoint = "GlassSettings_ForceUnloadProcess")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_ForceUnloadProcess(uint pid);

    [DllImport(Dll, EntryPoint = "GlassSettings_IsX86ForceUnloadAgentAvailable")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_IsX86ForceUnloadAgentAvailable();

    [DllImport(Dll, EntryPoint = "GlassSettings_TryGetAutorunGlassCompatInstallRootForArch", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_TryGetAutorunGlassCompatInstallRootForArch(
        int arch,
        StringBuilder buf,
        uint cch);

    [DllImport(Dll, EntryPoint = "GlassSettings_HasShellInjectResidualsForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_HasShellInjectResidualsForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_QueryInjectLifecycleUiForArch")]
    private static extern int Native_QueryInjectLifecycleUiForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_RefreshInjectResidualStickyForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_RefreshInjectResidualStickyForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_ClearInjectMapPidForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_ClearInjectMapPidForArch(uint arch, uint pid);

    [DllImport(Dll, EntryPoint = "GlassSettings_ScanGlassModules")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_ScanGlassModules(
        [In] uint[]? pids,
        uint pidsCount,
        IntPtr outResults,
        uint maxResults,
        ref uint outCount);
}

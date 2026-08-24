using System.Runtime.InteropServices;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    public static int GetBuiltInBlockListCount() =>
        unchecked((int)Native_GetBuiltInBlockListCount());

    public static bool TryCopyBuiltInBlockListAt(int index, char[] buffer)
    {
        if (buffer.Length == 0 || index < 0)
            return false;
        return Native_CopyBuiltInBlockListAt((uint)index, buffer, (uint)buffer.Length);
    }

    public static bool IsBuiltInBlockList(string exeName) =>
        Native_IsBuiltInBlockList(exeName);

    [DllImport(Dll, EntryPoint = "GlassSettings_GetBuiltInBlockListCount")]
    private static extern uint Native_GetBuiltInBlockListCount();

    [DllImport(Dll, EntryPoint = "GlassSettings_CopyBuiltInBlockListAt", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_CopyBuiltInBlockListAt(
        uint index,
        [Out] char[] buffer,
        uint cch);

    [DllImport(Dll, EntryPoint = "GlassSettings_IsBuiltInBlockList", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_IsBuiltInBlockList(string exeName);
}

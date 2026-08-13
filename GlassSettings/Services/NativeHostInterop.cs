using System.Runtime.InteropServices;
using GlassSettings.Controls;
using GlassSettings.Core.Text;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace GlassSettings.Services;

/// <summary>P/Invoke Рє GlassSettings.Native.dll вЂ” GlassCore runtime (РїСЂРµРІСЊСЋ С‚РµРєСЃС‚Р°, live reload).</summary>
public static partial class NativeHostInterop
{
    private const string Dll = "GlassSettings.Native.dll";

    public static bool EnsureGlassRuntime() => Native_EnsureGlassCoreRuntime();

    public static bool IsGlassRuntimeLoaded() => Native_IsGlassRuntimeLoaded();

    public static void ShutdownGlassRuntime() => Native_ShutdownGlassRuntime();

    public static void ReapplyOpenRootBackdropsIfEngineLoaded() =>
        Native_ReapplyOpenRootBackdropsIfEngineLoaded();

    public static void ResetReapplyFingerprintIfEngineLoaded() =>
        Native_ResetReapplyFingerprintIfEngineLoaded();

    public static void PostLiveRootBackdropReapplyAllIfEngineLoaded() =>
        Native_PostLiveRootBackdropReapplyAllIfEngineLoaded();

    public static void PostLiveRootBackdropReapplyAppliedIfEngineLoaded() =>
        Native_PostLiveRootBackdropReapplyAppliedIfEngineLoaded();

    public static void PostLiveRootColorizationToAppliedRootsIfEngineLoaded(int liveRootScope = 0) =>
        Native_PostLiveRootColorizationToAppliedRootsIfEngineLoaded(liveRootScope);

    public static void InvalidateThemedChromeOnTrackedRootsIfEngineLoaded() =>
        Native_InvalidateThemedChromeOnTrackedRootsIfEngineLoaded();

    public static void InvalidateTextGlowClientsOnTrackedRootsIfEngineLoaded() =>
        Native_InvalidateTextGlowClientsOnTrackedRootsIfEngineLoaded();

    public static void ReloadRenderTextInkFromRegistryIfEngineLoaded() =>
        Native_ReloadRenderTextInkFromRegistryIfEngineLoaded();

    public static void SetRenderTextPreviewGlobalGlowIfEngineLoaded(byte[] bytes)
    {
        if (bytes.Length < GlobalTextGlowModel.BinarySize)
            return;

        Native_SetRenderTextPreviewGlobalGlow(bytes, bytes.Length);
    }

    public static bool TryLoadGlassCore() => Native_LoadGlassCoreDll();

    public static bool CanShowTextVisualTuningDialog() => Native_CanShowTextVisualTuningDialog();

    public static bool IsShellExtensionRegistered() => Native_IsShellExtensionRegistered();

    internal static int PaintTextPreviewToBuffer(
        byte[] buffer,
        int width,
        int height,
        int stride,
        ref TextPreviewNative.NativeRequest request,
        bool interactiveDuringGesture) =>
        Native_PaintTextPreviewToBuffer(
            buffer,
            width,
            height,
            stride,
            ref request,
            interactiveDuringGesture);

    internal static void FillMessageLogFont(out TextPreviewNative.LogFontW font) =>
        Native_FillMessageLogFont(out font);

    internal static void FillPreviewLogFont(out TextPreviewNative.LogFontW font, int pointSize) =>
        Native_FillPreviewLogFont(out font, pointSize);

    public static bool TryPickExecutableFile(nint owner, char[] pathBuffer)
    {
        if (pathBuffer.Length < 260)
            return false;

        return Native_PickExecutableFile(owner, pathBuffer, pathBuffer.Length);
    }

    public static bool TryPickExecutableFile(Window window, out string fullPath)
    {
        fullPath = string.Empty;
        if (window is null)
            return false;

        var owner = WindowNative.GetWindowHandle(window);
        var buffer = new char[260];
        if (!Native_PickExecutableFile(owner, buffer, buffer.Length))
            return false;

        fullPath = new string(buffer).TrimEnd('\0');
        return !string.IsNullOrWhiteSpace(fullPath);
    }

    public static bool TryPickImageFile(nint owner, char[] pathBuffer)
    {
        if (pathBuffer.Length < 260)
            return false;

        return Native_PickImageFile(owner, pathBuffer, pathBuffer.Length);
    }

    public static bool TryPickImageFile(Window window, out string fullPath)
    {
        fullPath = string.Empty;
        if (window is null)
            return false;

        var owner = WindowNative.GetWindowHandle(window);
        var buffer = new char[260];
        if (!Native_PickImageFile(owner, buffer, buffer.Length))
            return false;

        fullPath = new string(buffer).TrimEnd('\0');
        return !string.IsNullOrWhiteSpace(fullPath);
    }

    public static bool TryPickFolder(nint owner, char[] pathBuffer)
    {
        if (pathBuffer.Length < 260)
            return false;

        return Native_PickFolder(owner, pathBuffer, pathBuffer.Length);
    }

    public static bool TryPickFolder(Window window, out string fullPath)
    {
        fullPath = string.Empty;
        if (window is null)
            return false;

        var owner = WindowNative.GetWindowHandle(window);
        var buffer = new char[260];
        if (!Native_PickFolder(owner, buffer, buffer.Length))
            return false;

        fullPath = new string(buffer).TrimEnd('\0');
        return !string.IsNullOrWhiteSpace(fullPath);
    }

    [DllImport(Dll, EntryPoint = "GlassSettings_IsShellExtensionRegistered")]
    private static extern bool Native_IsShellExtensionRegistered();

    [DllImport(Dll, EntryPoint = "GlassSettings_LoadGlassCoreDll")]
    private static extern bool Native_LoadGlassCoreDll();

    [DllImport(Dll, EntryPoint = "GlassSettings_EnsureGlassCoreRuntime")]
    private static extern bool Native_EnsureGlassCoreRuntime();

    [DllImport(Dll, EntryPoint = "GlassSettings_IsGlassRuntimeLoaded")]
    private static extern bool Native_IsGlassRuntimeLoaded();

    [DllImport(Dll, EntryPoint = "GlassSettings_ShutdownGlassRuntime")]
    private static extern void Native_ShutdownGlassRuntime();

    [DllImport(Dll, EntryPoint = "GlassSettings_ReapplyOpenRootBackdropsIfEngineLoaded")]
    private static extern void Native_ReapplyOpenRootBackdropsIfEngineLoaded();

    [DllImport(Dll, EntryPoint = "GlassSettings_ResetReapplyFingerprintIfEngineLoaded")]
    private static extern void Native_ResetReapplyFingerprintIfEngineLoaded();

    [DllImport(Dll, EntryPoint = "GlassSettings_PostLiveRootBackdropReapplyAllIfEngineLoaded")]
    private static extern void Native_PostLiveRootBackdropReapplyAllIfEngineLoaded();

    [DllImport(Dll, EntryPoint = "GlassSettings_PostLiveRootBackdropReapplyAppliedIfEngineLoaded")]
    private static extern void Native_PostLiveRootBackdropReapplyAppliedIfEngineLoaded();

    [DllImport(Dll, EntryPoint = "GlassSettings_PostLiveRootColorizationToAppliedRootsIfEngineLoaded")]
    private static extern void Native_PostLiveRootColorizationToAppliedRootsIfEngineLoaded(int liveRootScope);

    [DllImport(Dll, EntryPoint = "GlassSettings_InvalidateThemedChromeOnTrackedRootsIfEngineLoaded")]
    private static extern void Native_InvalidateThemedChromeOnTrackedRootsIfEngineLoaded();

    [DllImport(Dll, EntryPoint = "GlassSettings_InvalidateTextGlowClientsOnTrackedRootsIfEngineLoaded")]
    private static extern void Native_InvalidateTextGlowClientsOnTrackedRootsIfEngineLoaded();

    [DllImport(Dll, EntryPoint = "GlassSettings_ReloadRenderTextInkFromRegistryIfEngineLoaded")]
    private static extern void Native_ReloadRenderTextInkFromRegistryIfEngineLoaded();

    [DllImport(Dll, EntryPoint = "GlassSettings_SetRenderTextPreviewGlobalGlow")]
    private static extern void Native_SetRenderTextPreviewGlobalGlow(byte[] data, nint byteCount);

    [DllImport(Dll, EntryPoint = "GlassSettings_PaintTextPreviewToBuffer")]
    private static extern int Native_PaintTextPreviewToBuffer(
        byte[] buffer,
        int width,
        int height,
        int stride,
        ref TextPreviewNative.NativeRequest request,
        [MarshalAs(UnmanagedType.Bool)] bool interactiveDuringGesture);

    [DllImport(Dll, EntryPoint = "GlassSettings_FillMessageLogFont")]
    private static extern void Native_FillMessageLogFont(out TextPreviewNative.LogFontW font);

    [DllImport(Dll, EntryPoint = "GlassSettings_FillPreviewLogFont")]
    private static extern void Native_FillPreviewLogFont(out TextPreviewNative.LogFontW font, int pointSize);

    [DllImport(Dll, EntryPoint = "GlassSettings_PickExecutableFile")]
    private static extern bool Native_PickExecutableFile(
        nint owner,
        [Out] char[] pathBuffer,
        int cchPath);

    [DllImport(Dll, EntryPoint = "GlassSettings_PickImageFile")]
    private static extern bool Native_PickImageFile(
        nint owner,
        [Out] char[] pathBuffer,
        int cchPath);

    [DllImport(Dll, EntryPoint = "GlassSettings_PickFolder")]
    private static extern bool Native_PickFolder(
        nint owner,
        [Out] char[] pathBuffer,
        int cchPath);

    [DllImport(Dll, EntryPoint = "GlassSettings_CanShowTextVisualTuningDialog")]
    private static extern bool Native_CanShowTextVisualTuningDialog();
}

namespace GlassSettings.Core.Platform;



public static class ReloadRenderFlags

{

    public const uint GlassCore = 0x00000001;

    public const uint TaskbarSwca = 0x00000002;

    public const uint TaskbarXaml = 0x00000004;

    public const uint RootBackdropReapply = 0x00000008;

    public const uint TaskbarBoth = TaskbarSwca | TaskbarXaml;

    public const uint All = GlassCore | TaskbarSwca | TaskbarXaml | RootBackdropReapply;

    public const uint GlassSettingsCommit = GlassCore | TaskbarSwca | TaskbarXaml;

    /// <summary>Перечитать AttachList в инжектированных процессах.</summary>
    public const uint AttachList = 0x00000010;

    /// <summary>Live Root backdrop по g_rootWindows.</summary>
    public const uint RootLiveBackdropAll = 0x00000020;

    /// <summary>Live Root backdrop только по SnapshotGlassAppliedRootHwnds.</summary>
    public const uint RootLiveBackdropApplied = 0x00000080;

    /// <summary>Live DrawTheme alpha: soft-invalidate chrome по g_rootWindows.</summary>
    public const uint RootLiveThemePaint = 0x00000040;

    /// <summary>Live Root composition: PostMessage WM_DWMCOLORIZATIONCHANGED (wParam=0, lParam=scope)</summary>
    public const uint RootLiveColorizationApplied = 0x00000100;

    /// <summary>Перечитать GlobalTextGlow / ink globals в инжектированных процессах.</summary>
    public const uint RenderText = 0x00000200;
}


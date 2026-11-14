namespace GlassSettings.Search;

internal static class SettingsSearchResourceKeys
{
    // Осмысленные подписи и descriptions, сгруппированные по destination.
    private static readonly IReadOnlyDictionary<string, string[]> KeysByDestination =
        new Dictionary<string, string[]>(StringComparer.Ordinal)
        {
            ["glass"] =
            [
                "Glass_GeneralDesc", "Glass_GeneralTitle", "Glass_HubGeneral", "Glass_HubGeneralDesc",
            ],
            ["root-windows"] =
            [
                "Flyout_Animation", "Flyout_AnimationDesc", "Flyout_AnimationSubtitle", "Flyout_BorderColor",
                "Flyout_BorderColorDesc", "Flyout_BorderColorSubtitle", "Flyout_BorderSource", "Flyout_BorderSourceDesc",
                "Flyout_CornerType", "Flyout_CornerTypeDesc", "Flyout_Corners", "Flyout_CornersShadows",
                "Flyout_CornersShadowsSubtitle", "Flyout_DrawThemeAlpha", "Flyout_DrawThemeAlphaSubtitle", "Flyout_EffectParams",
                "Flyout_EffectScopeDesc", "Flyout_EffectScopeRootTargets", "Flyout_EffectScopeTargets", "Flyout_EffectScopeTaskbarTarget",
                "Flyout_EffectType", "Flyout_EffectWithIndex", "Flyout_GradientColor", "Flyout_GradientColorDesc",
                "Flyout_GradientColorSubtitle", "Flyout_GradientSystemBackdropLimit", "Flyout_GradientSystemBackdropTip", "Flyout_LabelAlpha",
                "Flyout_NoBorder", "Flyout_NoBorderCheck", "Flyout_PageSubtitle", "Flyout_RenderMode",
                "Flyout_RenderModeDesc", "Flyout_RootEffectDesc", "Flyout_Scope", "Flyout_ScopeGeneral",
                "Flyout_ScopeIndividual", "Flyout_SeparateZones", "Flyout_SeparateZonesDesc", "Flyout_Shadow",
                "Flyout_TaskbarEffectDesc", "Flyout_Type", "Flyout_ZonesCornersPolicyLimit", "Flyout_ZonesNeedCornersTip",
                "Glass_ControlIcon_Button", "Glass_ControlIcon_Button_Desc", "Glass_ControlIcon_Button_Subtitle", "Glass_ControlIcon_ListView",
                "Glass_ControlIcon_ListView_Desc", "Glass_ControlIcon_ListView_Subtitle", "Glass_ControlIcon_Ribbon", "Glass_ControlIcon_Ribbon_Desc",
                "Glass_ControlIcon_Ribbon_Subtitle", "Glass_ControlIcon_Static", "Glass_ControlIcon_Static_Desc", "Glass_ControlIcon_Static_Subtitle",
                "Glass_ControlIcon_Subtitle", "Glass_ControlIcon_Threshold_Label", "Glass_ControlIcon_Title", "Glass_ControlIcon_Toolbar",
                "Glass_ControlIcon_Toolbar_Desc", "Glass_ControlIcon_Toolbar_Subtitle", "Glass_ControlIcon_TreeView", "Glass_ControlIcon_TreeView_Desc",
                "Glass_ControlIcon_TreeView_Subtitle", "Glass_HubRoot", "Glass_HubRootDesc", "Glass_LabelCornerRadius",
                "Glass_LabelEffectType", "Glass_LabelEnableThemeColorization", "Glass_LabelMenuItemWidth", "Glass_LabelThemeColorizationType",
                "ParamLabel_CornerType", "ParamLabel_EnableDropShadow", "ParamLabel_FadeInTime", "ParamLabel_FadeOutTime",
                "ParamLabel_GlassDrawThemeConstantAlpha", "ParamLabel_ImmediateInterrupting", "ParamLabel_PopInStyle", "ParamLabel_PopInTime",
                "ParamLabel_StartRatio",
            ],
            ["popup"] =
            [
                "Flyout_Animation", "Flyout_AnimationDesc", "Flyout_AnimationSubtitle", "Flyout_BorderColor",
                "Flyout_BorderColorDesc", "Flyout_BorderColorSubtitle", "Flyout_BorderSource", "Flyout_BorderSourceDesc",
                "Flyout_CornerType", "Flyout_CornerTypeDesc", "Flyout_Corners", "Flyout_CornersShadows",
                "Flyout_CornersShadowsSubtitle", "Flyout_DrawThemeAlpha", "Flyout_DrawThemeAlphaSubtitle", "Flyout_EffectParams",
                "Flyout_EffectScopeDesc", "Flyout_EffectScopeRootTargets", "Flyout_EffectScopeTargets", "Flyout_EffectScopeTaskbarTarget",
                "Flyout_EffectType", "Flyout_EffectWithIndex", "Flyout_GradientColor", "Flyout_GradientColorDesc",
                "Flyout_GradientColorSubtitle", "Flyout_GradientSystemBackdropLimit", "Flyout_GradientSystemBackdropTip", "Flyout_LabelAlpha",
                "Flyout_NoBorder", "Flyout_NoBorderCheck", "Flyout_PageSubtitle", "Flyout_RenderMode",
                "Flyout_RenderModeDesc", "Flyout_RootEffectDesc", "Flyout_Scope", "Flyout_ScopeGeneral",
                "Flyout_ScopeIndividual", "Flyout_SeparateZones", "Flyout_SeparateZonesDesc", "Flyout_Shadow",
                "Flyout_TaskbarEffectDesc", "Flyout_Type", "Flyout_ZonesCornersPolicyLimit", "Flyout_ZonesNeedCornersTip",
                "Glass_HubPopup", "Glass_HubPopupDesc", "Glass_MenuGeneralSettings_Desc", "Glass_MenuGeneralSettings_Subtitle",
                "Glass_MenuGeneralSettings_Title", "Glass_MenuGeneral_EnableImmersiveStyle_Title", "Glass_MenuGeneral_NoModernAppBackground_Title", "Glass_MenuGeneral_NoSystemDropShadow_Title",
                "Glass_MenuGeneral_Title", "Glass_MenuIcon_Action", "Glass_MenuIcon_Threshold_Label", "Glass_MenuIcon_Title",
                "Glass_MenuItem_DisabledHot", "Glass_MenuItem_DisabledHotAction", "Glass_MenuItem_DisabledHotDesc", "Glass_MenuItem_Focusing",
                "Glass_MenuItem_FocusingAction", "Glass_MenuItem_FocusingDesc", "Glass_MenuItem_Hot", "Glass_MenuItem_HotAction",
                "Glass_MenuItem_HotDesc", "Glass_PopupDropDown", "Glass_PopupDropDownDesc", "Glass_PopupMenu",
                "Glass_PopupMenuDesc", "ParamLabel_CornerType", "ParamLabel_EnableDropShadow", "ParamLabel_FadeInTime",
                "ParamLabel_FadeOutTime", "ParamLabel_GlassDrawThemeConstantAlpha", "ParamLabel_ImmediateInterrupting", "ParamLabel_PopInStyle",
                "ParamLabel_PopInTime", "ParamLabel_StartRatio",
            ],
            ["taskbar"] =
            [
                "Flyout_Animation", "Flyout_AnimationDesc", "Flyout_AnimationSubtitle", "Flyout_BorderColor",
                "Flyout_BorderColorDesc", "Flyout_BorderColorSubtitle", "Flyout_BorderSource", "Flyout_BorderSourceDesc",
                "Flyout_CornerType", "Flyout_CornerTypeDesc", "Flyout_Corners", "Flyout_CornersShadows",
                "Flyout_CornersShadowsSubtitle", "Flyout_DrawThemeAlpha", "Flyout_DrawThemeAlphaSubtitle", "Flyout_EffectParams",
                "Flyout_EffectScopeDesc", "Flyout_EffectScopeRootTargets", "Flyout_EffectScopeTargets", "Flyout_EffectScopeTaskbarTarget",
                "Flyout_EffectType", "Flyout_EffectWithIndex", "Flyout_GradientColor", "Flyout_GradientColorDesc",
                "Flyout_GradientColorSubtitle", "Flyout_GradientSystemBackdropLimit", "Flyout_GradientSystemBackdropTip", "Flyout_LabelAlpha",
                "Flyout_NoBorder", "Flyout_NoBorderCheck", "Flyout_PageSubtitle", "Flyout_RenderMode",
                "Flyout_RenderModeDesc", "Flyout_RootEffectDesc", "Flyout_Scope", "Flyout_ScopeGeneral",
                "Flyout_ScopeIndividual", "Flyout_SeparateZones", "Flyout_SeparateZonesDesc", "Flyout_Shadow",
                "Flyout_TaskbarEffectDesc", "Flyout_Type", "Flyout_ZonesCornersPolicyLimit", "Flyout_ZonesNeedCornersTip",
                "Glass_HubTaskbar", "Glass_HubTaskbarDesc", "ParamLabel_CornerType", "ParamLabel_EnableDropShadow",
                "ParamLabel_FadeInTime", "ParamLabel_FadeOutTime", "ParamLabel_GlassDrawThemeConstantAlpha", "ParamLabel_ImmediateInterrupting",
                "ParamLabel_PopInStyle", "ParamLabel_PopInTime", "ParamLabel_StartRatio",
            ],
            ["adjacent"] =
            [
                "Glass_AdjacentWindows", "Glass_AdjacentWindowsDesc", "Glass_Adjacent_EffectDesc", "Glass_Adjacent_TabMenu",
                "Glass_Adjacent_TabPopup",
            ],
            ["dwm"] =
            [
                "Dwm_HubColorization", "Dwm_HubColorizationDesc", "Dwm_HubGlass", "Dwm_HubGlassDesc",
                "Dwm_HubTheme", "Dwm_HubThemeDesc", "Dwm_System_BatteryShort", "Dwm_System_GeneralSubtitle",
                "Dwm_System_GeneralTitle", "Dwm_System_Hook_AccentOverrider", "Dwm_System_Hook_CaptionMetrics", "Dwm_System_Hook_CaptionText",
                "Dwm_System_Hook_GlassFrame", "Dwm_System_Hook_GlassReflection", "Dwm_System_HooksCta", "Dwm_System_HooksDesc",
                "Dwm_System_SafetyZoneShort", "Dwm_System_SymbolsCachePath", "Dwm_System_SymbolsCancel", "Dwm_System_SymbolsDownload",
                "Dwm_System_SymbolsDownloading", "Dwm_System_SymbolsTitle",
            ],
            ["dwm-colorization"] =
            [
                "Dwm_ActiveGlassColor", "Dwm_ActiveGlassColorBodyDescVista", "Dwm_ActiveGlassColorBodyDescWin7", "Dwm_ActiveGlassColorDesc",
                "Dwm_ActiveGlassColorSubtitle", "Dwm_AfterglowColor", "Dwm_AfterglowColorBodyDesc", "Dwm_AfterglowColorDesc",
                "Dwm_AfterglowSubtitle", "Dwm_BaseColorSpoilerTitle", "Dwm_BaseMaximized", "Dwm_BaseMaximizedBodyDesc",
                "Dwm_BaseMaximizedDesc", "Dwm_BaseOpacityActive", "Dwm_BaseOpacityActiveDesc", "Dwm_BaseOpacityExpanderDesc",
                "Dwm_BaseOpacityInactive", "Dwm_BaseOpacityInactiveDesc", "Dwm_BaseOpacityInactiveMaximized", "Dwm_BaseOpacityInactiveMaximizedDesc",
                "Dwm_BaseOpacityMaximized", "Dwm_BaseOpacityMaximizedDesc", "Dwm_BaseOpacitySpoilerTitle", "Dwm_BaseOpacitySpoilerTitleActive",
                "Dwm_BaseOpacitySpoilerTitleInactive", "Dwm_BaseOpaque", "Dwm_BaseOpaqueBodyDesc", "Dwm_BaseOpaqueDesc",
                "Dwm_BaseTransparent", "Dwm_BaseTransparentBodyDesc", "Dwm_BaseTransparentDesc", "Dwm_BlurLevelTitle",
                "Dwm_BlurRadiusTitle", "Dwm_CustomInactiveColor", "Dwm_CustomInactiveColorBodyDesc", "Dwm_CustomInactiveColorDesc",
                "Dwm_CustomInactiveColorSubtitle", "Dwm_GlassColor", "Dwm_GlassColorDesc", "Dwm_GlassColorSubtitle",
                "Dwm_GlassColorWin7", "Dwm_GlassColorWin7Subtitle", "Dwm_GlassTypeDesc", "Dwm_GlassTypeShort",
                "Dwm_GlassTypeTitle", "Dwm_GlassTypeVista", "Dwm_GlassTypeWin7", "Dwm_HubColorization",
                "Dwm_HubColorizationDesc", "Dwm_OpacityColorization", "Dwm_OpacityGlass", "Dwm_OpacityMaximized",
                "Dwm_OpacityModeDesc", "Dwm_PivotBaseColorMaximized", "Dwm_PivotBaseColorOpaque", "Dwm_PivotBaseColorTransparent",
                "Dwm_PivotBaseOpacityActive", "Dwm_PivotBaseOpacityInactive", "Dwm_PivotBaseOpacityInactiveMax", "Dwm_PivotBaseOpacityMaximized",
                "ParamLabel_GlassDrawThemeConstantAlpha",
            ],
            ["dwm-glass"] =
            [
                "Dwm_AppearanceCaption", "Dwm_AppearanceCaptionBodyDesc", "Dwm_AppearanceCaptionSubtitle", "Dwm_AppearanceGeneral",
                "Dwm_AppearanceGeneralSubtitle", "Dwm_AppearanceWindow", "Dwm_AppearanceWindowBodyDesc", "Dwm_AppearanceWindowSubtitle",
                "Dwm_BlurLevelTitle", "Dwm_BlurOptimization", "Dwm_BlurRadiusTitle", "Dwm_CaptionButtons",
                "Dwm_CaptionButtonsDesc", "Dwm_CaptionButtonsStyle", "Dwm_CaptionColor", "Dwm_CaptionColorActiveSubtitle",
                "Dwm_CaptionColorActiveTitle", "Dwm_CaptionColorInactiveSubtitle", "Dwm_CaptionColorInactiveTitle", "Dwm_CaptionColorMax",
                "Dwm_CaptionColorOverride", "Dwm_CaptionDefault", "Dwm_CaptionHide", "Dwm_DisableModernBorders",
                "Dwm_GlassGeometryRadius", "Dwm_RoundRectProfileLabel", "Dwm_System_Hook_CaptionMetrics", "Dwm_System_Hook_CaptionText",
                "Dwm_System_Hook_GlassFrame", "Dwm_TextCentering", "Dwm_TextGlowMode", "Dwm_TextGlowSize",
                "Dwm_UseDirect3DRenderer",
            ],
            ["dwm-theme"] =
            [
                "Dwm_AtlasCollection", "Dwm_AtlasImage", "Dwm_AtlasImageDesc", "Dwm_DefaultMaterialTile",
                "Dwm_DefaultReflectionTile", "Dwm_ExportAtlasButton", "Dwm_HubTheme", "Dwm_HubThemeDesc",
                "Dwm_Intensity", "Dwm_MaterialCollection", "Dwm_MaterialCollectionDesc", "Dwm_MaterialOpacity",
                "Dwm_MaterialOpacityDesc", "Dwm_ModeFromTheme", "Dwm_ReflectionCollection", "Dwm_ReflectionControlsTitle",
                "Dwm_ReflectionDefaultTitle", "Dwm_ReflectionImage", "Dwm_ReflectionImageDesc", "Dwm_ReflectionIntensity",
                "Dwm_ReflectionIntensityDesc", "Dwm_ReflectionOpacity", "Dwm_ReflectionOpacityDesc", "Dwm_ReflectionOpacityMax",
                "Dwm_ReflectionParallax", "Dwm_ReflectionParallaxDesc", "Dwm_ReflectionParametersDesc", "Dwm_ReflectionParametersTitle",
                "Dwm_ReflectionPolicyAeroPeek", "Dwm_ReflectionPolicyAeroSnap", "Dwm_ReflectionPolicyDesc", "Dwm_ReflectionPolicyLabel",
                "Dwm_ReflectionPolicyTitlebar", "Dwm_System_Hook_GlassReflection",
            ],
            ["themes"] =
            [
                "Themes_AllSection", "Themes_ApplyFonts", "Themes_ApplySystemFonts", "Themes_ApplySystemFontsDesc",
                "Themes_BackgroundCardTitle", "Themes_BackgroundSolid", "Themes_BackgroundSpotlight", "Themes_BrowseThemes",
                "Themes_ColorCardTitle", "Themes_CurrentSection", "Themes_Default", "Themes_DeleteActiveLibraryBody",
                "Themes_DeleteMenu", "Themes_FindOnline", "Themes_OtherLabel", "Themes_PageSubtitle",
                "Themes_RestartNotRequired", "Themes_RestartRequired", "Themes_RowGlassSmoothing", "Themes_RowGlassSmoothingDesc",
                "Themes_RowSystemFontsDesc", "Themes_SaveTheme", "Themes_SaveThemeCancel", "Themes_SectionDesc",
                "Themes_SecureUxThemeOnlineLink", "Themes_SignatureApplyBlockedPending", "Themes_SignatureCardDesc", "Themes_SignatureCardTitle",
                "Themes_SignatureRebootButton", "Themes_SystemFontsDesc", "Themes_SystemFontsTitle", "Themes_TextAndFontsDesc",
                "Themes_ThemeFonts", "Themes_ThemeFontsDesc", "Themes_UsePersonalizedTheme", "Themes_UsePersonalizedThemeMissing",
            ],
            ["themes-background"] =
            [
                "Themes_BackgroundCardTitle", "Themes_BackgroundSolid", "Themes_BackgroundSpotlight", "Themes_Bg_AlbumDeselectAll",
                "Themes_Bg_AlbumInvertSelection", "Themes_Bg_AlbumSelectAll", "Themes_Bg_Browse", "Themes_Bg_BrowseFolder",
                "Themes_Bg_BrowsePhotos", "Themes_Bg_ChooseFit", "Themes_Bg_ChoosePhoto", "Themes_Bg_FitItems",
                "Themes_Bg_ModeItems", "Themes_Bg_PersonalizeDesc", "Themes_Bg_PersonalizeTitle", "Themes_Bg_RecentImages",
                "Themes_Bg_SlideshowAlbum", "Themes_Bg_SlideshowChooseFit", "Themes_Bg_SlideshowInterval", "Themes_Bg_SlideshowIntervalItems",
                "Themes_Bg_SlideshowOnBattery", "Themes_Bg_SlideshowShuffle", "Themes_Bg_WindowsDesktopBackgrounds",
            ],
            ["themes-colors"] =
            [
                "Themes_ColorCardTitle", "Themes_Colors_AccentBodyDesc", "Themes_Colors_AccentDesc", "Themes_Colors_AccentModeItems",
                "Themes_Colors_AccentStartTaskbar", "Themes_Colors_AccentTitle", "Themes_Colors_AccentTitleBars", "Themes_Colors_AppsMode",
                "Themes_Colors_LightDarkItems", "Themes_Colors_ModeBodyDesc", "Themes_Colors_ModeChoiceItems", "Themes_Colors_ModeDesc",
                "Themes_Colors_ModeTitle", "Themes_Colors_RecentColors", "Themes_Colors_TransparencyDesc", "Themes_Colors_TransparencyTitle",
                "Themes_Colors_WindowsMode",
            ],
            ["text"] =
            [
                "Text_EffectsDesc", "Text_EffectsTitle", "Text_GlowColorDescDark", "Text_GlowColorDescLight",
                "Text_GlowEnableToggle", "Text_GlowPanelIntro", "Text_GlowRadiusLabel", "Text_GlowSpreadLabel",
                "Text_InkBlendLabel", "Text_InkGammaBlendDesc", "Text_InkGammaBlendIntro", "Text_InkGammaBlendTitle",
                "Text_InkGammaLabel", "Text_InkProfileModeLabel", "Text_InkPseudoDesc", "Text_InkPseudoIntensityLabel",
                "Text_InkPseudoIntro", "Text_InkPseudoRadiusLabel", "Text_InkPseudoTitle", "Text_InkVisualTuneButton",
                "Text_RenderControlCardTitle", "Text_SystemFontsChoose", "Text_SystemFontsDesc", "Text_SystemFontsPivotTabs",
                "Text_SystemFontsScopeGlobal", "Text_SystemFontsScopeSelective", "Text_SystemFontsTitle", "Themes_ApplySystemFonts",
                "Themes_ApplySystemFontsDesc", "Themes_SystemFontsDesc", "Themes_SystemFontsTitle", "Themes_ThemeFonts",
                "Themes_ThemeFontsDesc",
            ],
            ["system"] =
            [
                "Home_RowShellExtension", "Home_RowShellExtensionSub", "System_RowGlassPlatform", "System_RowGlassPlatformDesc",
                "System_RowGlassPlatformX64", "System_RowGlassPlatformX86", "System_RowOpenGlass", "System_RowOpenGlassDesc",
                "System_RowShellExtension", "System_RowShellExtensionDesc", "System_UpdateDaily", "System_UpdateIntervalDesc",
                "System_UpdateIntervalTitle", "System_UpdateMonthly", "System_UpdateNever", "System_UpdateWeekly",
            ],
            ["attach"] =
            [
                "Attach_ActionSuccess", "Attach_AddChooseFile", "Attach_AddEnterString", "Attach_AddFromRunningList",
                "Attach_AddPickWindow", "Attach_AddProcess", "Attach_BlockedProcess", "Attach_DetailForbidden",
                "Attach_DetailFull", "Attach_DetailPopup", "Attach_DuplicateProcess", "Attach_FilterAll",
                "Attach_FilterByLabel", "Attach_FilterForbidden", "Attach_FilterSystem", "Attach_ListSection",
                "Attach_ListSectionDesc", "Attach_MenuDelete", "Attach_MenuDeleteAndBlock", "Attach_MenuDisable",
                "Attach_ModeForbidden", "Attach_ModeFull", "Attach_ModePopup", "Attach_ProcessNameExtensionRequiredBody",
                "Attach_ProcessNameExtensionRequiredTitle", "Attach_ProcessNameInputTitle", "Attach_Process_Few", "Attach_Process_Many",
                "Attach_Process_One", "Attach_SortByLabel", "Attach_SortMode", "Attach_SortNameAsc",
                "Attach_SortNameDesc", "Attach_ViewCompact", "Attach_ViewDetails", "Attach_ViewLargeIcons",
                "Attach_ViewList", "Attach_ViewTiles", "Attach_WindowPickDuplicateBody", "Attach_WindowPickDuplicateTitle",
                "Home_RowProcessList", "Home_RowViewAllProcesses", "Runtime_ColProcess", "Runtime_CopyName",
                "Runtime_EndTask", "Runtime_FlyoutSubclass", "Runtime_FlyoutSubsystems", "Runtime_Kill",
                "Runtime_Section", "Runtime_SectionDesc", "Runtime_SubclassKinds", "Runtime_SubclassWindows",
                "Runtime_SubsystemState", "Runtime_Unload", "Runtime_UnloadAll", "Runtime_UnloadAndExclude",
                "Runtime_UnloadAndForbid", "Runtime_UnloadNeedsX86Package",
            ],
        };

    public static IReadOnlyList<string> ForDestination(string destination) =>
        KeysByDestination.TryGetValue(destination, out var keys) ? keys : Array.Empty<string>();
}

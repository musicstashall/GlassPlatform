#pragma once

#include <guiddef.h>

// IID интерфейсов Microsoft.UI.Composition (WinUI 3 / Windows App Runtime).
// Извлечены из метаданных:
//   C:\Windows\SystemApps\Microsoft.WindowsAppRuntime.vNext.CBS_8wekyb3d8bbwe\Microsoft.UI.winmd
// Утилита: tools\WinmdGuidDump (см. docs\frozen\explorer-xaml\ExplorerXamlPopupPatchingAttempts.md)
// Раскладка слотов vtable ABI: dotnet run -- --vtable <Microsoft.UI.winmd> <полное имя интерфейса>

// Microsoft.UI.Composition.ICompositionObject
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositionObject =
    { 0x0E583D49, 0xFB5E, 0x5481, { 0xA4, 0x26, 0xD3, 0xC4, 0x1E, 0x05, 0x9A, 0x5A } };

// Microsoft.UI.Composition.ICompositionBrush
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositionBrush =
    { 0x483924E7, 0x99A5, 0x5377, { 0x96, 0x8B, 0xDE, 0xC6, 0xD4, 0x0B, 0xBC, 0xCD } };

// Microsoft.UI.Composition.ICompositor
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositor =
    { 0x95213C13, 0xC4CB, 0x57DE, { 0xB2, 0x67, 0xD2, 0x1A, 0xB9, 0x01, 0xAE, 0x38 } };

// Microsoft.UI.Composition.ICompositor2 (CreateBackdropBrush — размытие под слоем в WinUI 3)
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositor2 =
    { 0xA9FFEDAD, 0x3982, 0x576D, { 0xA3, 0x8A, 0xC8, 0x88, 0xFF, 0x60, 0x58, 0x19 } };

// Microsoft.UI.Composition.ICompositionColorBrush
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositionColorBrush =
    { 0x3F8FFB69, 0x3E71, 0x55A7, { 0x8E, 0x79, 0xF2, 0x7A, 0x21, 0x4C, 0x56, 0xAE } };

// Microsoft.UI.Composition.ICompositionEffectBrush
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositionEffectBrush =
    { 0x62E0BBAB, 0x1F45, 0x5A44, { 0x9D, 0xDF, 0xF0, 0xC3, 0x8A, 0x02, 0xED, 0x85 } };

// Microsoft.UI.Composition.ICompositionEffectFactory
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositionEffectFactory =
    { 0xC50F407A, 0x0231, 0x5ED2, { 0xB7, 0xA7, 0xCA, 0x66, 0xD3, 0xE1, 0x4B, 0x3B } };

// Microsoft.UI.Composition.ICompositionEffectSourceParameter
// E13.3 — узел Backdrop в графе CompositionEffectBrush. Microsoft.UI.Composition.Compositor
// отвергает Windows.UI вариант (Composite::GetSource idx=0 во втором проходе валидации даёт
// E_INVALIDARG в CreateEffectFactory). Активация MS.UI варианта снимает ABI mismatch.
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositionEffectSourceParameter =
    { 0xBECE3367, 0x5704, 0x59F7, { 0xBE, 0x8C, 0xD6, 0x29, 0x3A, 0xF9, 0xC9, 0x5F } };

// Microsoft.UI.Composition.ICompositionEffectSourceParameterFactory
// vtable layout (winmd): слот 6 = Create(HSTRING name, ICompositionEffectSourceParameter** out)
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositionEffectSourceParameterFactory =
    { 0x26185954, 0x4489, 0x5D0E, { 0xAE, 0x4D, 0x7B, 0xC4, 0xBB, 0xBC, 0x61, 0x61 } };

// Microsoft.UI.Composition.ICompositionMaskBrush (CreateMaskBrush + put_Source/put_Mask — тинт поверх backdrop)
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositionMaskBrush =
    { 0xC9502786, 0x65AA, 0x5BE5, { 0xA6, 0x79, 0xC3, 0xB5, 0xDB, 0xFB, 0x0D, 0xC6 } };

// Microsoft.UI.Composition.ICompositionSupportsSystemBackdrop (get/put_SystemBackdrop на элементе WinUI)
inline constexpr GUID IID_Microsoft_UI_Composition_ICompositionSupportsSystemBackdrop =
    { 0x397DAFE4, 0xB6C2, 0x5BB9, { 0x95, 0x1D, 0xF5, 0x70, 0x7D, 0xE8, 0xB7, 0xBC } };

// Microsoft.UI.Composition.SystemBackdrops — единственный в этом SDK источник реального
// акрила: Compositor даёт только CreateBackdropBrush (внутриприложенческая подложка),
// в windowed popup ей нечего сэмплировать. DesktopAcrylicController — механизм системы.
inline constexpr GUID IID_Microsoft_UI_Composition_IDesktopAcrylicController =
    { 0x7C20A6AF, 0x8EB3, 0x5F08, { 0xBD, 0xFC, 0x6D, 0x35, 0xE3, 0x5D, 0xFE, 0x45 } };

// IDesktopAcrylicController3 — Kind (DesktopAcrylicKind: Default 0, Base 1, Thin 2)
inline constexpr GUID IID_Microsoft_UI_Composition_IDesktopAcrylicController3 =
    { 0x30D917E6, 0x02D3, 0x59CA, { 0xB4, 0x40, 0xBF, 0x9D, 0x2E, 0x7C, 0xC1, 0x40 } };

inline constexpr GUID IID_Microsoft_UI_Composition_ISystemBackdropControllerWithTargets =
    { 0x9C56FE7C, 0x98EB, 0x5F89, { 0xAD, 0x97, 0xDA, 0xD5, 0x7F, 0xC3, 0x0C, 0x8C } };

inline constexpr GUID IID_Microsoft_UI_Composition_ISystemBackdropConfiguration =
    { 0xEBCCE1B9, 0x0E0C, 0x5431, { 0xAB, 0x0E, 0x00, 0xF3, 0xF0, 0x66, 0x99, 0x62 } };

// Microsoft.UI.Xaml.IXamlRoot (WinmdGuidDump: …\Microsoft.UI.Xaml.winmd; get_Content — слот 6 vtable)
inline constexpr GUID IID_Microsoft_UI_Xaml_IXamlRoot =
    { 0x60CB215A, 0xAD15, 0x520A, { 0x8B, 0x01, 0x44, 0x16, 0x82, 0x4F, 0x04, 0x41 } };

// Windows.UI.Xaml.IXamlRoot (cppwinrt guid_v; тот же layout vtable, что у Microsoft.UI.Xaml.IXamlRoot)
inline constexpr GUID IID_Windows_UI_Xaml_IXamlRoot =
    { 0x34B50756, 0x1696, 0x5B6D, { 0x8E, 0x9B, 0xC7, 0x14, 0x64, 0xCC, 0xAD, 0x5A } };

// Microsoft.UI.Xaml.IUIElement — корректный IID из Microsoft.UI.Xaml.winmd (WASR/NuGet).
// Старый 0x63379181 давал E_NOINTERFACE (ветка C14 frozen).
inline constexpr GUID IID_Microsoft_UI_Xaml_IUIElement =
    { 0xC3C01020, 0x320C, 0x5CF6, { 0x9D, 0x24, 0xD3, 0x96, 0xBB, 0xFA, 0x4D, 0x8B } };

// Windows.UI.Xaml.IUIElement10 (cppwinrt guid_v): ABI — get_XamlRoot третий метод после IInspectable (vtable[8]).
inline constexpr GUID IID_Windows_UI_Xaml_IUIElement10 =
    { 0xD531C629, 0xAD2C, 0x5F6B, { 0xAD, 0xCF, 0xFB, 0x87, 0x28, 0x7D, 0x18, 0xD7 } };

// Windows.UI.Xaml.Media.IVisualTreeHelperStatics (RoGetActivationFactory(VisualTreeHelper)).
inline constexpr GUID IID_Windows_UI_Xaml_Media_IVisualTreeHelperStatics =
    { 0xE75758C4, 0xD25D, 0x4B1D, { 0x97, 0x1F, 0x59, 0x6F, 0x17, 0xF1, 0x2B, 0xAA } };

// Слоты IVisualTreeHelperStatics после IInspectable (WinMD ABI): … GetChild = 10, GetChildrenCount = 11.
inline constexpr int kWindowsIVisualTreeHelperStaticsVtable_GetChild = 10;
inline constexpr int kWindowsIVisualTreeHelperStaticsVtable_GetChildrenCount = 11;

// Слоты Microsoft.UI.Composition (WinmdGuidDump --vtable по Microsoft.UI.winmd).
inline constexpr int kMuxICompositionObject_get_Compositor = 6;
// ICompositor: два overload CreateEffectFactory — 11 (effect) и 12 (effect + animatableProperties).
inline constexpr int kMuxICompositor_CreateEffectFactory = 11;
inline constexpr int kMuxICompositor_CreateEffectFactoryWithProperties = 12;
inline constexpr int kMuxICompositor2_CreateBackdropBrush = 8;
inline constexpr int kMuxICompositionEffectFactory_CreateBrush = 6;
inline constexpr int kMuxICompositionEffectBrush_SetSourceParameter = 7;
inline constexpr int kMuxICompositionEffectSourceParameterFactory_Create = 6;

// SystemBackdrops (WinmdGuidDump --vtable по Microsoft.UI.winmd).
inline constexpr int kMuxIDesktopAcrylicController_put_FallbackColor = 7;
inline constexpr int kMuxIDesktopAcrylicController_put_LuminosityOpacity = 9;
inline constexpr int kMuxIDesktopAcrylicController_put_TintColor = 11;
inline constexpr int kMuxIDesktopAcrylicController_put_TintOpacity = 13;
inline constexpr int kMuxIDesktopAcrylicController3_put_Kind = 7;
inline constexpr int kMuxISystemBackdropControllerWithTargets_AddSystemBackdropTarget = 7;
inline constexpr int kMuxISystemBackdropControllerWithTargets_RemoveAllTargets = 8;
inline constexpr int kMuxISystemBackdropControllerWithTargets_SetConfiguration = 10;
inline constexpr int kMuxISystemBackdropConfiguration_put_IsInputActive = 11;
inline constexpr int kMuxISystemBackdropConfiguration_put_Theme = 13;
inline constexpr int kMuxICompositionSupportsSystemBackdrop_put_SystemBackdrop = 7;

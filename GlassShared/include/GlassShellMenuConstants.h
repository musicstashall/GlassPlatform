#pragma once

namespace GlassShared::GlassShellMenu
{
    inline constexpr wchar_t kHandlerSubkeyName[] = L"GlassPlatformDesktopMenu";

    /// CLSID legacy IContextMenu («Показать дополнительные параметры» на Win11).
    inline constexpr wchar_t kClsidStringBraced[] = L"{A91C9F80-7E4B-4D8A-9C16-F304E8B2D601}";

    /// CLSID IExplorerCommand — переключение платформы (компактное меню Win11).
    inline constexpr wchar_t kToggleExplorerClsidBraced[] = L"{6D4E18F4-91B2-4C7D-A3E9-442B7C889D41}";

    /// CLSID IExplorerCommand — открыть GlassSettings (компактное меню Win11).
    inline constexpr wchar_t kOpenConfigExplorerClsidBraced[] = L"{8B219A33-0456-418E-B5D8-991C2E774E52}";

    /// Canonical name IExplorerCommand::GetCanonicalName (см. GUID_GlassShell*CommandCanonical в GlassShellExt).
    inline constexpr wchar_t kToggleExplorerCommandCanonicalBraced[] = L"{C1E4A5F6-2D8B-4E01-9F3A-6D4E18F40101}";
    inline constexpr wchar_t kOpenConfigExplorerCommandCanonicalBraced[] = L"{D2F5B607-3E9C-4F12-A04B-8B219A330202}";

    inline constexpr wchar_t kDesktopToggleVerbSubkey[] = L"GlassPlatform00Toggle";
    inline constexpr wchar_t kDesktopOpenConfigVerbSubkey[] = L"GlassPlatform10OpenConfig";

    /// Identity sparse-пакета (AppxManifest Identity@Name).
    inline constexpr wchar_t kSparsePackageName[] = L"GlassProject.GlassShellMenu";

    /// Package family name (Name + Publisher hash из установленного пакета).
    inline constexpr wchar_t kSparsePackageFamilyName[] =
        L"GlassProject.GlassShellMenu_t6mpd150h6a5c";

    /// Подкаталог в output: AppxManifest.xml + Assets.
    inline constexpr wchar_t kSparsePackageSubdir[] = L"GlassShellSparse\\";

    /// Пустой манифест для снятия verb из каталога Win11 перед Remove-AppxPackage.
    inline constexpr wchar_t kSparseUnregisterStubFileName[] = L"AppxManifest.UnregisterStub.xml";

    inline constexpr wchar_t kSparsePublisher[] = L"CN=GlassProject Dev";
    inline constexpr wchar_t kSparseApplicationId[] = L"GlassShellHost";
};

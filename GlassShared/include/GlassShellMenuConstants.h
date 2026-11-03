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

    /// Package family name = Name + «_» + 13 символов (первые 8 байт SHA-256 от Publisher в UTF-16LE,
    /// base32 Crockford). Проверка: GlassSetup\tests\sparse_identity_mock.py; на Windows —
    /// Get-AppxPackage GlassProject.GlassShellMenu.
    inline constexpr wchar_t kSparsePackageFamilyName[] =
        L"GlassProject.GlassShellMenu_83517z3ecdax2";

    /// Подкаталог платформы с компонентами меню — одновременно ExternalLocation sparse-пакета:
    /// GlassShellExt.dll, GlassShellHost.exe, GlassShellSparse.msix, AppxManifest*.xml, Assets\.
    /// Сборка кладёт их туда же (OutDir GlassShellExt/GlassShellHost = Build\<arch>\<cfg>\GlassShellSparse\).
    inline constexpr wchar_t kSparsePackageSubdir[] = L"GlassShellSparse\\";

    /// Имена файлов и пути относительно корня платформы.
    inline constexpr wchar_t kShellExtFileName[] = L"GlassShellExt.dll";
    inline constexpr wchar_t kShellHostFileName[] = L"GlassShellHost.exe";
    inline constexpr wchar_t kSparseMsixFileName[] = L"GlassShellSparse.msix";
    inline constexpr wchar_t kShellExtRelativePath[] = L"GlassShellSparse\\GlassShellExt.dll";
    inline constexpr wchar_t kShellHostRelativePath[] = L"GlassShellSparse\\GlassShellHost.exe";

    /// Пустой манифест для снятия verb из каталога Win11 перед Remove-AppxPackage.
    inline constexpr wchar_t kSparseUnregisterStubFileName[] = L"AppxManifest.UnregisterStub.xml";

    /// Publisher манифеста = Subject сертификата разработчика (Shared\certs\DeveloperCodeSigning.cer),
    /// которым подписан msix. Отдельного «dev»-сертификата нет: на чужом ПК без доверия этому
    /// сертификату sparse-пакет не ставится, и меню регистрируется классически (см. GlassShellExt\README.md).
    inline constexpr wchar_t kSparsePublisher[] = L"CN=Stanislav K.";
    inline constexpr wchar_t kSparseApplicationId[] = L"GlassShellHost";

    /// Публичный сертификат разработчика и инструкция, поставляемые в корне платформы.
    inline constexpr wchar_t kAuthorCertificateFileName[] = L"DeveloperCodeSigning.cer";
    inline constexpr wchar_t kAuthorCertificateReadmeFileName[] = L"DeveloperCodeSigning.txt";
};

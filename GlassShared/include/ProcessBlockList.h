#pragma once
#include <cstddef>
#include <string>

namespace ProcessBlockList
{
    /// Встроенный BlockList — единственный источник истины.
    /// ShellExperienceHost / ShellHost / StartMenuExperienceHost / SearchHost / SearchApp
    /// сюда не входят: UICompat CoreWindow hosts (см. docs/guides/UICompat-CoreWindow.md).
    inline constexpr wchar_t const* const kBuiltInNames[] =
    {
        // Системные процессы Windows
        L"ApplicationFrameHost.exe",
        L"backgroundTaskHost.exe",
        L"conhost.exe",
        L"CoreWidgetProvider.exe",
        L"dwm.exe",
        L"FileCoAuth.exe",
        L"GameBar.exe",
        L"GameBarFTServer.exe",
        L"mblctr.exe",
        L"Microsoft.SharePoint.exe",
        L"mobsync.exe",
        L"msedge.exe",
        L"Adobe CEF Helper.exe",
        L"msedgewebview2.exe",
        L"OneDriveStandaloneUpdater.exe",
        L"PerfWatson2.exe",
        L"RuntimeBroker.exe",
        L"SDXHelper.exe",
        L"ServiceHub.ThreadedWaitDialog.exe",
        L"ServiceHub.VSDetouredHost.exe",
        L"sihost.exe",
        L"smartscreen.exe",
        L"splwow64.exe",
        L"spoolsv.exe",
        L"svchost.exe",
        L"systemsettings.exe",
        L"taskhostw.exe",
        L"WerFault.exe",
        L"WidgetService.exe",
        L"Widgets.exe",
        L"WindowsPackageManagerServer.exe",
        L"WkStartCpl64.exe",
        L"WkStartCpl86.exe",
        L"WSHost.exe",

        // Антивирус и безопасность
        L"avpui.exe",

        // Среды разработки
        L"clangd.exe",
        L"devenv.exe",
        L"msvsmon.exe",
        L"vsdebugger.exe",
        L"ServiceHub.Host.dotnet.arm64.exe",
        L"ServiceHub.Host.dotnet.x64.exe",

        // Инструменты отладки и мониторинга
        L"apimonitor-x64.exe",
        L"apimonitor-x86.exe",
        L"dbgview.exe",
        L"dbgview64.exe",
        L"DbgX.Shell.exe",
        L"procexp.exe",
        L"procexp64.exe",
        L"procmon.exe",
        L"procmon64.exe",
        L"windbg.exe",
        L"x32dbg.exe",
        L"x64dbg.exe",

        // Собственные инструменты
        L"LogViewer.exe",
        L"GlassSettings.exe",
    };

    [[nodiscard]] constexpr std::size_t BuiltInCount() noexcept
    {
        return sizeof(kBuiltInNames) / sizeof(kBuiltInNames[0]);
    }

    [[nodiscard]] inline const wchar_t* BuiltInAt(std::size_t index) noexcept
    {
        return index < BuiltInCount() ? kBuiltInNames[index] : nullptr;
    }

    /// Статический список + записи из реестра.
    void Initialize();

    /// Заблокирован: полная инъекция (GlassCore/WndCompat/GUI) не выполняется.
    bool IsBlocked(const std::wstring& exeName);

    /// Имя есть во встроенном списке (без реестра). Не требует Initialize().
    [[nodiscard]] bool IsBuiltIn(const std::wstring& exeName) noexcept;

    /// GlassSettings — локальный GlassCore без глобального NoBackdrop (WinUI host).
    [[nodiscard]] bool IsCurrentProcessGlassToolHost() noexcept;

    /// LogViewer — изолированный tool-host: NoBackdrop=0, backdrop не следует за старт/стоп платформы.
    [[nodiscard]] bool IsCurrentProcessLogViewerHost() noexcept;

    std::wstring Normalize(const std::wstring& exeName);

    /// Добавить exeName в реестр BlockList и в оперативный g_blockList.
    void AddToRegistry(const std::wstring& exeName) noexcept;
}

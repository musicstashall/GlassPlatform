#pragma once
#include <windows.h>
#include <functional>

namespace GlassCore::ModuleHandler
{
    // Колбэк о загрузке XAML-хоста (Windows.UI.Xaml.dll / Microsoft.UI.Xaml.dll). Регистрация: SetXamlHostDllNotify.
    // NotifyExistingXamlHostDlls — экспорт для ExplorerCompat: тот же attach-путь для уже загруженных модулей
    // (событие Ldr LOAD не приходит); в логах помечается как attach [replay] против attach [load] при скане/Ldr.
    using XamlHostDllNotifyFn = void(WINAPI*)(HMODULE loadedModule, const wchar_t* baseName);

    void SetXamlHostDllNotify(XamlHostDllNotifyFn fn) noexcept;
    void NotifyExistingXamlHostDlls() noexcept;

    // Запуск модульного диспетчера хуков (один раз на процесс)
    void Startup();

    // Остановка и отписка от уведомлений
    void Shutdown();

    /// Псевдо attach/unload для module-lifetime IAT (без comctl32 / Taskbar / XAML).
    void ScanBackdropScopedModules(bool attach) noexcept;

    /// Replay Full-scoped модулей после GlassCore_SetInjectionMode(Full).
    void ScanFullScopedModules(bool attach) noexcept;

    // Прямой доступ к comctl32 — без геттеров
    extern HMODULE g_comctl32New;      // WinSxS v6+ (через RunInActCtx)
    extern HMODULE g_comctl32Legacy;   // legacy comctl32 (v5.82 и т.п.)

    // Главный исполняемый модуль процесса (EXE).
    // Инициализируется в Startup() через DetourGetContainingModule(DetourGetEntryPoint(nullptr)).
    // Некоторые приложения рисуют текст в Static напрямую из EXE, минуя comctl32.
    extern HMODULE g_entryPointModule;

    // Запустить callback в контексте активации WinSxS comctl32 v6 (shell32.dll manifest #124).
    // Публичная — используется ModuleHandler и другими подсистемами.
    void RunInActCtx(std::function<void()> callback);

    // Предпрогрев IAT-кешей при загрузке DLL (только чтение: emplace диспетчера +
    // CacheHookData, без записей в IAT — семантика сессий не меняется).
    // Подписка из *Hooks::Startup (каждый файл — только свои модули);
    // дёргается из HandleModuleAttach(attach) — и сканом, и Ldr-уведомлением.
    // Безопасно под loader-lock: только чтение уже смапленного PE.
    using ModulePrewarmFn = void(*)(HMODULE loadedModule, const wchar_t* baseName) noexcept;
    void RegisterModulePrewarmHandler(ModulePrewarmFn fn) noexcept;
}

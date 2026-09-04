#include "CorePch.h"
#include "Module\ModuleHandler.h"
#include "Core\Utils.h"
#include <Hooks/DirectUI.h>
#include <Hooks/ScrollBarTheme.h>
#include <Hooks/ExplorerFrame.h>
#include <Hooks/TaskbarHooks.h>
#include <Hooks/forms/FormsHooks.h>
#include <Subsystems/RootSwcaGradient.h>
#include <RootPolicyQuery.h>
#include <InjectProcessMap.h>

#include <psapi.h>

#include <atomic>
#include <cstring>

// Добавляем UNICODE_STRING, если winternl.h не подключён
#ifndef _WINTERNL_
typedef struct _UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, * PUNICODE_STRING;
typedef const UNICODE_STRING* PCUNICODE_STRING;
#endif

namespace GlassCore::ModuleHandler
{
    static std::atomic<XamlHostDllNotifyFn> g_xamlHostDllNotify{ nullptr };

    void SetXamlHostDllNotify(XamlHostDllNotifyFn fn) noexcept
    {
        g_xamlHostDllNotify.store(fn, std::memory_order_release);
    }

    // Два известных XAML-хоста: UWP (Windows.UI.Xaml) и WinUI (Microsoft.UI.Xaml).
    enum class XamlHostKind : uint8_t
    {
        None = 0,
        WindowsUxaml,   // Windows.UI.Xaml.dll
        MicrosoftUxaml // Microsoft.UI.Xaml.dll
    };

    [[nodiscard]] static XamlHostKind ClassifyXamlHostBaseName(const wchar_t* baseName) noexcept
    {
        if (!baseName)
            return XamlHostKind::None;
        if (_wcsicmp(baseName, L"Windows.UI.Xaml.dll") == 0)
            return XamlHostKind::WindowsUxaml;
        if (_wcsicmp(baseName, L"Microsoft.UI.Xaml.dll") == 0)
            return XamlHostKind::MicrosoftUxaml;
        return XamlHostKind::None;
    }

    [[nodiscard]] static const wchar_t* CanonicalBaseNameForXamlHostKind(XamlHostKind kind) noexcept
    {
        switch (kind)
        {
        case XamlHostKind::WindowsUxaml:
            return L"Windows.UI.Xaml.dll";
        case XamlHostKind::MicrosoftUxaml:
            return L"Microsoft.UI.Xaml.dll";
        default:
            return nullptr;
        }
    }

    // Откуда пришёл attach: скан при старте / Ldr LOAD, либо повторный обход после SetXamlHostDllNotify (ExplorerCompat).
    enum class XamlHostAttachSource : uint8_t
    {
        ModuleLoadOrScan,
        ExplorerReplay,
    };

    static void LogXamlHostDllAttach(HMODULE module, XamlHostKind kind, XamlHostAttachSource source) noexcept
    {
        const wchar_t* const canonicalBase = CanonicalBaseNameForXamlHostKind(kind);
        if (!module || !canonicalBase || kind == XamlHostKind::None)
            return;

        const wchar_t* const sourceTag =
            (source == XamlHostAttachSource::ExplorerReplay) ? L"replay" : L"load";

        wchar_t fullPath[MAX_PATH]{};
        if (GetModuleFileNameW(module, fullPath, MAX_PATH))
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][XamlHost] XAML host attach [%ls]: %s (0x%p) Path: %s",
                sourceTag,
                canonicalBase,
                module,
                fullPath);
        }
        else
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][XamlHost] XAML host attach [%ls]: %s (0x%p)",
                sourceTag,
                canonicalBase,
                module);
        }
    }

    static void InvokeXamlHostDllNotify(HMODULE module, const wchar_t* canonicalBaseName) noexcept
    {
        if (const auto fn = g_xamlHostDllNotify.load(std::memory_order_acquire))
            fn(module, canonicalBaseName);
    }

    // Лог + колбэк ExplorerCompat (IXDE kick при появлении XAML-host DLL).
    static void AttachXamlHostDll(HMODULE module, XamlHostKind kind, XamlHostAttachSource source) noexcept
    {
        const wchar_t* const canonicalBase = CanonicalBaseNameForXamlHostKind(kind);
        if (!module || !canonicalBase || kind == XamlHostKind::None)
            return;

        LogXamlHostDllAttach(module, kind, source);
        InvokeXamlHostDllNotify(module, canonicalBase);
    }

    void NotifyExistingXamlHostDlls() noexcept
    {
        if (!g_xamlHostDllNotify.load(std::memory_order_acquire))
            return;

        HMODULE hLast = nullptr;
        wchar_t modPath[MAX_PATH]{};

        do
        {
            hLast = DetourEnumerateModules(hLast);
            if (!hLast)
                break;

            if (!GetModuleFileNameW(hLast, modPath, MAX_PATH))
                continue;

            const wchar_t* base = wcsrchr(modPath, L'\\');
            base = base ? base + 1 : modPath;

            const XamlHostKind kind = ClassifyXamlHostBaseName(base);
            if (kind != XamlHostKind::None)
                AttachXamlHostDll(hLast, kind, XamlHostAttachSource::ExplorerReplay);
        } while (hLast);
    }

    // --- типы для LdrRegisterDllNotification ---
    typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA
    {
        ULONG Flags;
        PCUNICODE_STRING FullDllName;
        PCUNICODE_STRING BaseDllName;
        PVOID DllBase;
        ULONG SizeOfImage;
    } LDR_DLL_LOADED_NOTIFICATION_DATA, * PLDR_DLL_LOADED_NOTIFICATION_DATA;

    typedef struct _LDR_DLL_UNLOADED_NOTIFICATION_DATA
    {
        ULONG Flags;
        PCUNICODE_STRING FullDllName;
        PCUNICODE_STRING BaseDllName;
        PVOID DllBase;
        ULONG SizeOfImage;
    } LDR_DLL_UNLOADED_NOTIFICATION_DATA, * PLDR_DLL_UNLOADED_NOTIFICATION_DATA;

    typedef union _LDR_DLL_NOTIFICATION_DATA
    {
        LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
        LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
    } LDR_DLL_NOTIFICATION_DATA, * PLDR_DLL_NOTIFICATION_DATA;

    using PCLDR_DLL_NOTIFICATION_DATA = const LDR_DLL_NOTIFICATION_DATA*;

    using LdrCallbackFn = VOID(CALLBACK*)(
        ULONG NotificationReason,
        PCLDR_DLL_NOTIFICATION_DATA NotificationData,
        PVOID Context
        );

    static PVOID g_cookie = nullptr;

    // Главный исполняемый модуль процесса — инициализируется в Startup()
    HMODULE g_entryPointModule = nullptr;

    // ---------------------------------------------------------
    // ХРАНЕНИЕ ВЕРСИЙ COMCTL32.DLL
    // ---------------------------------------------------------

    // WinSxS v6 (определяется через RunInActCtx); legacy — любой другой comctl32
    HMODULE g_comctl32New    = nullptr;
    HMODULE g_comctl32Legacy = nullptr;

    // ---------------------------------------------------------
    // ACTIVATION CONTEXT (WinSxS comctl32 v6)
    // ---------------------------------------------------------

    static HANDLE GetOrCreateActCtx()
    {
        auto release = [](HANDLE h) { ReleaseActCtx(h); };
        static wil::unique_any_handle_invalid<decltype(release), release> s_act
        {
            []() -> HANDLE
            {
                wchar_t sysDir[MAX_PATH + 1]{};
                GetSystemDirectoryW(sysDir, _countof(sysDir));
                ACTCTXW ctx
                {
                    .cbSize              = sizeof(ACTCTXW),
                    .dwFlags             = ACTCTX_FLAG_RESOURCE_NAME_VALID |
                                          ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID,
                    .lpSource            = L"shell32.dll",
                    .lpAssemblyDirectory = sysDir,
                    .lpResourceName      = MAKEINTRESOURCEW(124),
                };
                return CreateActCtxW(&ctx);
            }()
        };
        return s_act.get();
    }

    void RunInActCtx(std::function<void()> callback)
    {
        HANDLE h = GetOrCreateActCtx();
        ULONG_PTR cookie{};
        if (h != INVALID_HANDLE_VALUE && ActivateActCtx(h, &cookie))
        {
            callback();
            DeactivateActCtx(0, cookie);
        }
    }

    // ---------------------------------------------------------
    // ДОБАВЛЕНИЕ/УДАЛЕНИЕ COMCTL32
    // ---------------------------------------------------------

    static void AddComctl32(HMODULE mod)
    {
        // Захватываем WinSxS v6-адрес через контекст активации
        if (!g_comctl32New)
            RunInActCtx([&] { g_comctl32New = GetModuleHandleW(L"comctl32.dll"); });

        if (mod == g_comctl32New)
            DebugLog(L"[GlassCore][ModuleHandler] COMCTL32 new (WinSxS): 0x%p", mod);
        else
        {
            g_comctl32Legacy = mod;
            DebugLog(L"[GlassCore][ModuleHandler] COMCTL32 legacy: 0x%p", mod);
        }
    }

    static void RemoveComctl32(HMODULE mod)
    {
        if (mod == g_comctl32New)    g_comctl32New    = nullptr;
        if (mod == g_comctl32Legacy) g_comctl32Legacy = nullptr;

        DebugLog(L"[GlassCore][ModuleHandler] COMCTL32 unloaded: %p", mod);
    }

    // ---------------------------------------------------------
    // ЗАГРУЗКА ФУНКЦИЙ LdrRegisterDllNotification
    // ---------------------------------------------------------

    static auto LoadLdrRegister()
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return reinterpret_cast<NTSTATUS(NTAPI*)(
            ULONG, LdrCallbackFn, PVOID, PVOID*
            )>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    }

    static auto LoadLdrUnregister()
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return reinterpret_cast<NTSTATUS(NTAPI*)(PVOID)>(
            GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
    }

    // ---------------------------------------------------------
    // ОБРАБОТКА ЗАГРУЗКИ/ВЫГРУЗКИ МОДУЛЕЙ
    // ---------------------------------------------------------

    [[nodiscard]] static bool IsBackdropScopedModuleBaseName(const wchar_t* baseName) noexcept
    {
        if (!baseName)
            return false;
        return _wcsicmp(baseName, L"DUser.dll") == 0
            || _wcsicmp(baseName, L"DUI70.dll") == 0
            || _wcsicmp(baseName, L"wdc.dll") == 0
            || _wcsicmp(baseName, L"explorerframe.dll") == 0
            || _wcsicmp(baseName, L"twinui.pcshell.dll") == 0
            || _wcsicmp(baseName, L"shell32.dll") == 0
            || _wcsicmp(baseName, L"shlwapi.dll") == 0;
    }

    [[nodiscard]] static bool IsFullScopedModuleBaseName(const wchar_t* baseName) noexcept
    {
        return IsBackdropScopedModuleBaseName(baseName)
            || (baseName && _wcsicmp(baseName, L"Taskbar.dll") == 0);
    }

    static void HandleModuleAttach(
        HMODULE module,
        const wchar_t* baseName,
        bool attach,
        PCUNICODE_STRING ntFullDllName,
        const wchar_t* fullPathPrefer);

    static void EnumerateLoadedModulesForFilter(
        bool attach,
        bool (*filter)(const wchar_t* baseName) noexcept)
    {
        if (!filter)
            return;

        HMODULE hLast = nullptr;
        wchar_t modPath[MAX_PATH]{};

        do
        {
            hLast = DetourEnumerateModules(hLast);
            if (!hLast)
                break;

            if (!GetModuleFileNameW(hLast, modPath, MAX_PATH))
                continue;

            const wchar_t* base = wcsrchr(modPath, L'\\');
            base = base ? base + 1 : modPath;

            if (!filter(base))
                continue;

            HandleModuleAttach(hLast, base, attach, nullptr, modPath);
        } while (hLast);
    }

    static void HandleModuleAttach(
        HMODULE module,
        const wchar_t* baseName,
        bool attach,
        PCUNICODE_STRING ntFullDllName,
        const wchar_t* fullPathPrefer)
    {
        // Полный путь: предпочтительно уже известный (ScanExistingModules), затем UNICODE_STRING.FullDllName
        // из Ldr-уведомления, затем резерв через GetModuleFileNameW на HMODULE (MAX_PATH-длина строки).
        wchar_t resolvedBuf[MAX_PATH]{};
        wchar_t const* pathForLog = nullptr;
        if (fullPathPrefer != nullptr && fullPathPrefer[0] != L'\0')
        {
            pathForLog = fullPathPrefer;
        }
        else if (ntFullDllName != nullptr && ntFullDllName->Buffer != nullptr
            && ntFullDllName->Length >= sizeof(wchar_t))
        {
            const size_t WCHARCount =
                static_cast<size_t>(ntFullDllName->Length) / sizeof(wchar_t);
            const size_t copyW =
                WCHARCount >= static_cast<size_t>(MAX_PATH) ? static_cast<size_t>(MAX_PATH - 1)
                                                          : WCHARCount;
            std::memcpy(resolvedBuf, ntFullDllName->Buffer, copyW * sizeof(wchar_t));
            resolvedBuf[copyW] = L'\0';
            pathForLog = resolvedBuf;
        }
        else if (module != nullptr
            && GetModuleFileNameW(module, resolvedBuf, static_cast<DWORD>(std::size(resolvedBuf))) != 0)
        {
            pathForLog = resolvedBuf;
        }

        if (pathForLog != nullptr && pathForLog[0] != L'\0')
        {
            DebugLog(L"[GlassCore][ModuleHandler] Module %s: %s (0x%p) Path=%s",
                attach ? L"loaded" : L"unloaded",
                baseName,
                module,
                pathForLog);
        }
        else
        {
            DebugLog(L"[GlassCore][ModuleHandler] Module %s: %s (0x%p)",
                attach ? L"loaded" : L"unloaded", baseName, module);
        }

        // --- Windows.UI.Xaml.dll (UWP) / Microsoft.UI.Xaml.dll (WinUI) ---
        if (const XamlHostKind xamlKind = ClassifyXamlHostBaseName(baseName); xamlKind != XamlHostKind::None)
        {
            if (attach)
                AttachXamlHostDll(module, xamlKind, XamlHostAttachSource::ModuleLoadOrScan);
            return;
        }

        // --- COMCTL32.dll --- (безусловно: PopupOnly + NONE backdrop)
        else if (_wcsicmp(baseName, L"comctl32.dll") == 0)
        {
            if (attach)
            {
                AddComctl32(module);
                // ScrollBar IAT для comctl32 — ленивая установка из PaintDispatcher::HookGetDCEx
            }
            else
            {
                GlassCore::ScrollBarTheme::OnComctl32ModuleUnload(module);
                RemoveComctl32(module);
            }
            return;
        }

        if (attach && !GlassCore::Root::IsFullInjectionMode() && IsFullScopedModuleBaseName(baseName))
            return;

        if (attach && !Shared::RootPolicyQuery::IsRootGlassRenderingEnabled()
            && IsBackdropScopedModuleBaseName(baseName))
            return;

        // --- DUser.dll — загружается перед темами, помогает захватить WinSxS comctl32 ---
        // Плюс IAT DirectUI (как DUI70) — иначе DirectUIHWND, живущий на DUser, не получает хуков.
        else if (_wcsicmp(baseName, L"DUser.dll") == 0)
        {
            if (attach)
            {
                if (!g_comctl32New)
                    RunInActCtx([&] { g_comctl32New = GetModuleHandleW(L"comctl32.dll"); });
                GlassCore::DirectUI::OnDuiModuleLoaded(module, baseName);
            }
            else
                GlassCore::DirectUI::OnDuiModuleUnloaded(module, baseName);

            return;
        }

        // --- DUI70.dll — основной стек DirectUI (Shell, файловые диалоги, проводник) ---
        else if (_wcsicmp(baseName, L"DUI70.dll") == 0)
        {
            if (attach)
            {
                GlassCore::DirectUI::OnDuiModuleLoaded(module, baseName);
                GlassCore::ExplorerFrameControl::OnExplorerModuleLoaded(module, baseName);
            }
            else
            {
                GlassCore::DirectUI::OnDuiModuleUnloaded(module, baseName);
                GlassCore::ExplorerFrameControl::OnExplorerModuleUnloaded(module, baseName);
            }
            
            return;
        }

        // --- wdc.dll — Windows Device Context / DirectUI Win10+ ---
        else if (_wcsicmp(baseName, L"wdc.dll") == 0)
        {
            if (attach)
                GlassCore::DirectUI::OnDuiModuleLoaded(module, baseName);
            else
                GlassCore::DirectUI::OnDuiModuleUnloaded(module, baseName);

            return;
        }

        // --- System.Windows.Forms.ni.dll — подавление фона WinForms (BWM FormsHooks::ColorBrushAttach) ---
        else if (_wcsicmp(baseName, L"System.Windows.Forms.ni.dll") == 0)
        {
            GlassCore::FormsHooks::ColorBrushAttach(attach);
            return;
        }

        // --- dmdskmgr.dll / dmview.ocx — Disk Management snap-in (BWM ModuleHandler:200-205) ---
        else if (_wcsicmp(baseName, L"dmdskmgr.dll") == 0 || _wcsicmp(baseName, L"dmview.ocx") == 0)
        {
            if (attach)
            {
                DebugLog(L"[GlassCore][ModuleHandler] Disk Management module loaded: %s (0x%p)", baseName, module);
                // GDI+ включается при первом paint через ComboLBoxHooks/RootWindowHooks.
                // IAT хуки (EXTTEXTOUT, FILLRECT) устанавливаются при ComboLBox paint sessions.
            }
            return;
        }

        // --- Taskbar.dll — панель задач: IAT после системного TRANSPARENT_GRADIENT на Shell_*TrayWnd ---
        else if (_wcsicmp(baseName, L"Taskbar.dll") == 0)
        {
            if (attach)
                GlassCore::Hooks::TaskbarHooks::OnTaskbarModuleLoaded(module, baseName);
            else
                GlassCore::Hooks::TaskbarHooks::OnTaskbarModuleUnloaded(module, baseName);
            return;
        }

        // --- explorerframe.dll — IAT-хуки для окон проводника ---
        else if (_wcsicmp(baseName, L"explorerframe.dll") == 0)
        {
            if (attach)
                GlassCore::ExplorerFrameControl::OnExplorerModuleLoaded(module, baseName);
            else
                GlassCore::ExplorerFrameControl::OnExplorerModuleUnloaded(module, baseName);
            return;
        }

        // --- twinui.pcshell.dll — DwmSetWindowAttribute для проводника Win11 ---
        else if (_wcsicmp(baseName, L"twinui.pcshell.dll") == 0)
        {
            if (attach)
                GlassCore::ExplorerFrameControl::OnExplorerModuleLoaded(module, baseName);
            else
                GlassCore::ExplorerFrameControl::OnExplorerModuleUnloaded(module, baseName);
            return;
        }

        // --- shell32.dll — CreateWindowExW + GetThemeColor для проводника ---
        else if (_wcsicmp(baseName, L"shell32.dll") == 0)
        {
            if (attach)
                GlassCore::ExplorerFrameControl::OnExplorerModuleLoaded(module, baseName);
            else
                GlassCore::ExplorerFrameControl::OnExplorerModuleUnloaded(module, baseName);
        }

        // --- shlwapi.dll — ExtTextOutW для проводника ---
        else if (_wcsicmp(baseName, L"shlwapi.dll") == 0)
        {
            if (attach)
                GlassCore::ExplorerFrameControl::OnExplorerModuleLoaded(module, baseName);
            else
                GlassCore::ExplorerFrameControl::OnExplorerModuleUnloaded(module, baseName);
        }
    }

    // ---------------------------------------------------------
    // CALLBACK LdrRegisterDllNotification
    // ---------------------------------------------------------

    static VOID CALLBACK LdrCallback(
        ULONG reason,
        PCLDR_DLL_NOTIFICATION_DATA data,
        PVOID /*ctx*/
    )
    {
        if (!data) return;

        if (reason == 1) // LOADED
        {
            if (data->Loaded.BaseDllName == nullptr || data->Loaded.BaseDllName->Buffer == nullptr)
                return;
            const wchar_t* const base = data->Loaded.BaseDllName->Buffer;
            auto const mod = reinterpret_cast<HMODULE>(data->Loaded.DllBase);
            PCUNICODE_STRING const fullNt = data->Loaded.FullDllName;
            HandleModuleAttach(mod, base, true, fullNt, nullptr);
        }
        else if (reason == 2) // UNLOADED
        {
            if (data->Unloaded.BaseDllName == nullptr || data->Unloaded.BaseDllName->Buffer == nullptr)
                return;
            const wchar_t* const base = data->Unloaded.BaseDllName->Buffer;
            auto const mod = reinterpret_cast<HMODULE>(data->Unloaded.DllBase);
            PCUNICODE_STRING const fullNt = data->Unloaded.FullDllName;
            HandleModuleAttach(mod, base, false, fullNt, nullptr);
        }
    }

    // ---------------------------------------------------------
    // ПЕРВИЧНЫЙ ПРОХОД ПО УЖЕ ЗАГРУЖЕННЫМ МОДУЛЯМ
    // ---------------------------------------------------------

    // Первичный проход через DetourEnumerateModules — по образцу BWM.
    // Обходит PE-заголовки в памяти напрямую, видит все загруженные модули
    // включая те, что не зарегистрированы в стандартном списке ОС.
    static void ScanExistingModules()
    {
        HMODULE hLast = nullptr;
        wchar_t modPath[MAX_PATH];

        do
        {
            hLast = DetourEnumerateModules(hLast);
            if (!hLast) break;

            if (!GetModuleFileNameW(hLast, modPath, MAX_PATH))
                continue;

            const wchar_t* base = wcsrchr(modPath, L'\\');
            base = base ? base + 1 : modPath;

            //DebugLog(L"[GlassCore][ModuleHandler] ScanExistingModules: %s (%p)", base, hLast);

            HandleModuleAttach(hLast, base, true, nullptr, modPath);
        } while (hLast);
    }

    // ---------------------------------------------------------
    // ПУБЛИЧНЫЕ ФУНКЦИИ
    // ---------------------------------------------------------

    void ScanBackdropScopedModules(bool attach) noexcept
    {
        EnumerateLoadedModulesForFilter(attach, IsBackdropScopedModuleBaseName);
    }

    void ScanFullScopedModules(bool attach) noexcept
    {
        EnumerateLoadedModulesForFilter(attach, IsFullScopedModuleBaseName);
    }

    void Startup()
    {
        DebugLog(L"[GlassCore][ModuleHandler] ModuleHandler::Startup");

        Shared::RootPolicyQuery::RefreshRootBackdropPolicyCache();

        g_entryPointModule = DetourGetContainingModule(DetourGetEntryPoint(nullptr));
        DebugLog(L"[GlassCore][ModuleHandler] ModuleHandler: entryPointModule=0x%p", g_entryPointModule);

        ScanExistingModules();

        auto LdrRegister = LoadLdrRegister();
        if (LdrRegister)
        {
            NTSTATUS st = LdrRegister(0, LdrCallback, nullptr, &g_cookie);
            DebugLog(L"[GlassCore][ModuleHandler] LdrRegisterDllNotification: 0x%08X", st);
        }

        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagModuleHandler,
            true);
    }

    void Shutdown()
    {
        DebugLog(L"[GlassCore][ModuleHandler] ModuleHandler::Shutdown");

        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagModuleHandler,
            false);

        auto LdrUnregister = LoadLdrUnregister();
        if (g_cookie && LdrUnregister)
        {
            LdrUnregister(g_cookie);
            g_cookie = nullptr;
        }

        g_comctl32New = g_comctl32Legacy = g_entryPointModule = nullptr;
    }
}

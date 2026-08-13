#include "CorePch.h"

#include <Hooks/TaskbarHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <RootBackdropPolicy.h>
#include <TrayBorderDrawMode.h>

#include <DebugLog.h>
#include <wil.h>

#include <unordered_map>

using namespace std::literals;

namespace GlassCore::Hooks::TaskbarHooks
{
    namespace
    {
        constexpr DWORD kWcaAccentPolicy             = 19;
        constexpr DWORD kAccentTransparentGradient = 2;

        struct TrayAccentPolicy
        {
            DWORD AccentState;
            DWORD AccentFlags;
            DWORD dwGradientColor;
            DWORD dwAnimationId;
        };

        struct TrayWinCompAttrData
        {
            DWORD attribute;
            PVOID pData;
            ULONG dataSize;
        };

        std::atomic<bool> g_shutdown{ false };

        BOOL WINAPI MySetWindowCompositionAttribute(HWND hwnd, TrayWinCompAttrData* pAttrData);

        enum HookIndex : size_t
        {
            HK_SWCA = 0,
            HK_COUNT
        };

        inline const HookDispatcherDependency<HK_COUNT, 3> g_hookDependency{
            {std::tuple{
                std::array<std::string_view, 3>{
                    "user32.dll"sv,
                    "ext-ms-win-ntuser-window-l1-1-0.dll"sv,
                    ""sv,
                },
                "SetWindowCompositionAttribute",
                reinterpret_cast<PVOID>(MySetWindowCompositionAttribute),
            }}
        };

        wil::srwlock g_dispatchLock;

        using DispatcherType = HookDispatcher<HK_COUNT, 3>;
        std::unordered_map<PVOID, DispatcherType> g_dispatcherByTaskbarBase;

        [[nodiscard]] bool IsCurrentProcessExplorerExe() noexcept
        {
            wchar_t path[MAX_PATH]{};
            const DWORD n =
                GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
            if (n == 0 || n >= std::size(path))
                return false;
            const wchar_t* slash = wcsrchr(path, L'\\');
            const wchar_t* base  = slash ? slash + 1 : path;
            return _wcsicmp(base, L"explorer.exe") == 0;
        }

        [[nodiscard]] bool IsShellTrayWindowClass(HWND hRoot) noexcept
        {
            wchar_t cls[256]{};
            if (GetClassNameW(hRoot, cls, static_cast<int>(std::size(cls))) <= 0)
                return false;
            return _wcsicmp(cls, L"Shell_TrayWnd") == 0
                || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
        }

        [[nodiscard]] bool IsXamlPaintPath() noexcept
        {
            return Shared::TrayBorderDrawMode::ReadFromRegistry()
                == Shared::TrayBorderDrawMode::Mode::Xaml;
        }

        /// WCA_ACCENT_POLICY: TRANSPARENT_GRADIENT + gradient=0 (как в трассах Taskbar.dll на трей).
        [[nodiscard]] bool IsTraySwallowAccentPolicy(const TrayWinCompAttrData* p) noexcept
        {
            if (!p || !p->pData)
                return false;
            if (p->attribute != kWcaAccentPolicy)
                return false;
            if (p->dataSize < sizeof(TrayAccentPolicy))
                return false;
            const auto* const pol =
                reinterpret_cast<const TrayAccentPolicy*>(p->pData);
            if (pol->AccentState != kAccentTransparentGradient)
                return false;
            return pol->dwGradientColor == 0;
        }

        [[nodiscard]] BOOL CallUser32Direct(HWND hwnd, TrayWinCompAttrData* pAttrData)
        {
            using FnDirect = BOOL(WINAPI*)(HWND, TrayWinCompAttrData*);
            static FnDirect s_fn = reinterpret_cast<FnDirect>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
            return s_fn ? s_fn(hwnd, pAttrData) : FALSE;
        }

        BOOL WINAPI MySetWindowCompositionAttribute(HWND hwnd, TrayWinCompAttrData* pAttrData)
        {
            PVOID callerBase = DetourGetContainingModule(_ReturnAddress());

            DispatcherType* dispatcher{};
            {
                auto         sl = g_dispatchLock.lock_shared();
                const auto it = g_dispatcherByTaskbarBase.find(callerBase);
                if (it != g_dispatcherByTaskbarBase.end())
                    dispatcher = &it->second;
            }

            using FnHooked = BOOL(WINAPI*)(HWND, TrayWinCompAttrData*);
            FnHooked orig{};
            if (dispatcher)
                orig = dispatcher->GetOrg<HK_SWCA, FnHooked>();
            else
                return CallUser32Direct(hwnd, pAttrData);
            if (!orig)
                return CallUser32Direct(hwnd, pAttrData);

            if (g_shutdown.load(std::memory_order_acquire))
                return orig(hwnd, pAttrData);

            if (!GlassCore::Root::IsFullInjectionMode())
                return orig(hwnd, pAttrData);

            if (!IsCurrentProcessExplorerExe())
                return orig(hwnd, pAttrData);

            if (!hwnd || !IsWindow(hwnd))
                return orig(hwnd, pAttrData);

            if (!IsShellTrayWindowClass(hwnd))
                return orig(hwnd, pAttrData);

            // XAML path: HWND должен быть полностью clear — иначе Taskbar.dll рисует сплошной тинт
            // под островными зонами (BgFill/TaskbarBackground уже Collapsed).
            if (IsXamlPaintPath())
            {
                if (pAttrData && pAttrData->pData && pAttrData->attribute == kWcaAccentPolicy
                    && pAttrData->dataSize >= sizeof(TrayAccentPolicy))
                {
                    auto* const pol = reinterpret_cast<TrayAccentPolicy*>(pAttrData->pData);
                    const DWORD prevState = pol->AccentState;
                    const DWORD prevGrad = pol->dwGradientColor;
                    if (prevState != kAccentTransparentGradient || prevGrad != 0)
                    {
                        pol->AccentState = kAccentTransparentGradient;
                        pol->dwGradientColor = 0;
                        DebugLog(
                            DBG_IMPORTANT,
                            L"[GlassCore][TaskbarHooks] XAML: force SWCA clear "
                            L"(was accent=%u grad=0x%08X) hwnd=0x%p",
                            prevState,
                            prevGrad,
                            hwnd);
                    }
                }
                return orig(hwnd, pAttrData);
            }

            // DWM path: глотаем «пустой» Accent от Taskbar и переутверждаем Glass SWCA.
            if (!IsTraySwallowAccentPolicy(pAttrData))
                return orig(hwnd, pAttrData);

            if (!Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hwnd))
                return orig(hwnd, pAttrData);

            thread_local bool s_inHookReapply{};
            if (s_inHookReapply)
                return orig(hwnd, pAttrData);

            s_inHookReapply = true;
            auto reapplyGuard = wil::scope_exit([&]() noexcept { s_inHookReapply = false; });

            GlassCore::Root::ReapplyTrayRootSwcaAfterTaskbarAccent(hwnd);

            return TRUE;
        }

        [[nodiscard]] bool EnableHooksInternal(PVOID taskbarBase, bool enable)
        {
            if (!taskbarBase)
                return false;

            auto ex = g_dispatchLock.lock_exclusive();

            auto it = g_dispatcherByTaskbarBase.find(taskbarBase);
            if (it == g_dispatcherByTaskbarBase.end())
            {
                auto [inserted, okEmplace] = g_dispatcherByTaskbarBase.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(taskbarBase),
                    std::forward_as_tuple(g_hookDependency, taskbarBase));
                (void)okEmplace;
                it = inserted;
                it->second.CacheHookData();
            }
            else
                it->second.moduleAddress = taskbarBase;

            if (enable)
                it->second.EnableHook(HK_SWCA, true);
            else
                it->second.DisableAllHooks();

            return true;
        }

    } // namespace

    void SyncHooksForTrayPaintPath() noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return;

        const HMODULE taskbar = GetModuleHandleW(L"Taskbar.dll");
        if (!taskbar)
            return;

        // IAT всегда ON: DWM — защита Glass SWCA; XAML — clear HWND от тинта Taskbar.
        (void)EnableHooksInternal(reinterpret_cast<PVOID>(taskbar), true); // NOLINT
        DebugLog(
            DBG_IMPORTANT,
            L"[GlassCore][TaskbarHooks] Sync paintPath: SWCA IAT ON (BorderDrawMode=%u)",
            IsXamlPaintPath() ? 1u : 0u);
    }

    void OnTaskbarModuleLoaded(HMODULE module, PCWSTR baseName) noexcept
    {
        (void)baseName;
        if (!module || g_shutdown.load(std::memory_order_acquire))
            return;

        (void)EnableHooksInternal(reinterpret_cast<PVOID>(module), true); // NOLINT
        DebugLog(
            DBG_IMPORTANT,
            L"[GlassCore][TaskbarHooks] IAT Taskbar.dll base=0x%p вкл. (paintPath mode=%u)",
            static_cast<void*>(module),
            IsXamlPaintPath() ? 1u : 0u);
    }

    void OnTaskbarModuleUnloaded(HMODULE module, PCWSTR baseName) noexcept
    {
        (void)baseName;
        const PVOID taskbarBase = reinterpret_cast<PVOID>(module); // NOLINT
        auto        ex           = g_dispatchLock.lock_exclusive();
        const auto  it           = g_dispatcherByTaskbarBase.find(taskbarBase);
        if (it == g_dispatcherByTaskbarBase.end())
            return;
        it->second.DisableAllHooks();
        g_dispatcherByTaskbarBase.erase(it);
    }

    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto ex = g_dispatchLock.lock_exclusive();
        for (auto& [base, dispatcher] : g_dispatcherByTaskbarBase)
        {
            (void)base;
            dispatcher.DisableAllHooks();
        }
        g_dispatcherByTaskbarBase.clear();
    }
}

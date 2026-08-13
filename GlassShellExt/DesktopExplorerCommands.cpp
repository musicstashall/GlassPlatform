#include "GlassShellPch.h"

#include "DesktopExplorerFactory.h"
#include "DesktopShellActions.h"
#include "GlassShellExtClsids.h"
#include "GlassShellModule.h"
#include "resource.h"

#include <GlassPlatformTask.h>

#include <new>
#include <string>
#include <vector>

#include <strsafe.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace
{
    constexpr int kShellExtRootIconId = IDI_GLASSSHELLEXT;
    constexpr int kShellExtToggleIconId = IDI_GLASSSHELLEXT_PT;
    constexpr int kShellExtSettingsIconId = IDI_GLASSSHELLEXT_ST;
    constexpr int kShellExtOpenGlassIconId = IDI_GLASSSHELLEXT_OG;

    template <typename InvokeFn>
    [[nodiscard]] HRESULT CreateExplorerCommand(REFIID riid, void** ppv) noexcept;

    [[nodiscard]] bool BuildShellExtIconResource(std::wstring& out, int iconId) noexcept
    {
        out.clear();

        if (!GlassShellExt::g_module)
            return false;

        wchar_t modulePath[MAX_PATH]{};
        if (::GetModuleFileNameW(GlassShellExt::g_module, modulePath, MAX_PATH) == 0)
            return false;

        wchar_t iconSpec[MAX_PATH + 32]{};
        if (FAILED(::StringCchPrintfW(iconSpec, _countof(iconSpec), L"%s,-%d", modulePath, iconId)))
            return false;

        out.assign(iconSpec);
        return true;
    }

    [[nodiscard]] HRESULT DupTitleToOutParam(std::wstring const& title, PWSTR* ppszName) noexcept
    {
        if (!ppszName)
            return E_POINTER;
        *ppszName = nullptr;
        return ::SHStrDupW(title.c_str(), ppszName);
    }

    [[nodiscard]] EXPCMDSTATE MenuStateToExplorerState(UINT menuState) noexcept
    {
        if ((menuState & MFS_DISABLED) != 0)
            return ECS_DISABLED;
        return ECS_ENABLED;
    }

    class ExplorerCommandEnum final : public IEnumExplorerCommand
    {
        LONG m_cRef = 1;
        ULONG m_index = 0;
        std::vector<IExplorerCommand*> m_items;

    public:
        explicit ExplorerCommandEnum(std::vector<IExplorerCommand*>&& items) noexcept : m_items(std::move(items))
        {
            for (IExplorerCommand* item : m_items)
            {
                if (item)
                    item->AddRef();
            }
            InterlockedIncrement(&g_liveExplorerCommandObjects);
        }

        ~ExplorerCommandEnum()
        {
            for (IExplorerCommand* item : m_items)
            {
                if (item)
                    item->Release();
            }
            InterlockedDecrement(&g_liveExplorerCommandObjects);
        }

        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override
        {
            if (!ppv)
                return E_POINTER;
            *ppv = nullptr;
            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumExplorerCommand))
                *ppv = static_cast<IEnumExplorerCommand*>(this);
            else
                return E_NOINTERFACE;
            AddRef();
            return S_OK;
        }

        IFACEMETHODIMP_(ULONG) AddRef() noexcept override { return static_cast<ULONG>(InterlockedIncrement(&m_cRef)); }

        IFACEMETHODIMP_(ULONG) Release() noexcept override
        {
            const LONG c = InterlockedDecrement(&m_cRef);
            if (c == 0)
                delete this;
            return static_cast<ULONG>(c);
        }

        IFACEMETHODIMP Next(ULONG celt, IExplorerCommand** pUICommand, ULONG* pceltFetched) noexcept override
        {
            if (!pUICommand)
                return E_POINTER;
            if (pceltFetched)
                *pceltFetched = 0;

            ULONG fetched = 0;
            while (fetched < celt && m_index < m_items.size())
            {
                IExplorerCommand* cmd = m_items[m_index++];
                pUICommand[fetched] = cmd;
                if (cmd)
                    cmd->AddRef();
                ++fetched;
            }

            if (pceltFetched)
                *pceltFetched = fetched;
            return fetched == celt ? S_OK : S_FALSE;
        }

        IFACEMETHODIMP Skip(ULONG celt) noexcept override
        {
            const ULONG remaining = static_cast<ULONG>(m_items.size() - m_index);
            const ULONG step = (celt < remaining) ? celt : remaining;
            m_index += step;
            return step == celt ? S_OK : S_FALSE;
        }

        IFACEMETHODIMP Reset() noexcept override
        {
            m_index = 0;
            return S_OK;
        }

        IFACEMETHODIMP Clone(IEnumExplorerCommand** ppenum) noexcept override
        {
            if (!ppenum)
                return E_POINTER;
            *ppenum = nullptr;

            auto* clone = new (std::nothrow) ExplorerCommandEnum(std::vector<IExplorerCommand*>{m_items.begin(), m_items.end()});
            if (!clone)
                return E_OUTOFMEMORY;
            clone->m_index = m_index;
            *ppenum = clone;
            return S_OK;
        }
    };

    template <typename InvokeFn>
    class GlassDesktopExplorerCommandBase final : public IExplorerCommand, public IObjectWithSite
    {
        LONG m_cRef = 1;
        IUnknown* m_punkSite = nullptr;
        InvokeFn m_invoke;

    public:
        explicit GlassDesktopExplorerCommandBase(InvokeFn fn) noexcept : m_invoke(fn)
        {
            InterlockedIncrement(&g_liveExplorerCommandObjects);
        }

        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override
        {
            if (!ppv)
                return E_POINTER;
            *ppv = nullptr;
            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IExplorerCommand))
                *ppv = static_cast<IExplorerCommand*>(this);
            else if (IsEqualIID(riid, IID_IObjectWithSite))
                *ppv = static_cast<IObjectWithSite*>(this);
            else
                return E_NOINTERFACE;
            AddRef();
            return S_OK;
        }

        IFACEMETHODIMP_(ULONG) AddRef() noexcept override { return static_cast<ULONG>(InterlockedIncrement(&m_cRef)); }

        IFACEMETHODIMP_(ULONG) Release() noexcept override
        {
            const LONG c = InterlockedDecrement(&m_cRef);
            if (c == 0)
            {
                if (m_punkSite)
                {
                    m_punkSite->Release();
                    m_punkSite = nullptr;
                }
                InterlockedDecrement(&g_liveExplorerCommandObjects);
                delete this;
            }
            return static_cast<ULONG>(c);
        }

        IFACEMETHODIMP GetTitle(IShellItemArray*, PWSTR* ppszName) noexcept override
        {
            return m_invoke.GetTitle(ppszName);
        }

        IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* ppszIcon) noexcept override
        {
            return m_invoke.GetIcon(ppszIcon);
        }

        IFACEMETHODIMP GetToolTip(IShellItemArray*, PWSTR* ppszInfotip) noexcept override
        {
            if (!ppszInfotip)
                return E_POINTER;
            *ppszInfotip = nullptr;
            return E_NOTIMPL;
        }

        IFACEMETHODIMP GetCanonicalName(GUID* pguidCommandName) noexcept override
        {
            return m_invoke.GetCanonicalName(pguidCommandName);
        }

        IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* pCmdState) noexcept override
        {
            if (!pCmdState)
                return E_POINTER;
            return m_invoke.GetState(pCmdState);
        }

        IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) noexcept override
        {
            HWND owner = nullptr;
            if (m_punkSite)
                (void)IUnknown_GetWindow(m_punkSite, &owner);
            m_invoke.Invoke(owner);
            return S_OK;
        }

        IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) noexcept override
        {
            if (!pFlags)
                return E_POINTER;
            return m_invoke.GetFlags(pFlags);
        }

        IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** ppEnum) noexcept override
        {
            return m_invoke.EnumSubCommands(ppEnum);
        }

        IFACEMETHODIMP SetSite(IUnknown* punkSite) noexcept override
        {
            if (m_punkSite)
                m_punkSite->Release();
            m_punkSite = punkSite;
            if (m_punkSite)
                m_punkSite->AddRef();
            return S_OK;
        }

        IFACEMETHODIMP GetSite(REFIID riid, void** ppv) noexcept override
        {
            if (!ppv)
                return E_POINTER;
            *ppv = nullptr;
            if (!m_punkSite)
                return E_FAIL;
            return m_punkSite->QueryInterface(riid, ppv);
        }
    };

    template <GlassShared::GlassPlatformTask::PlatformArch kArch, const GUID& kCanonical>
    struct ToggleArchLeafInvoke
    {
        static HRESULT GetIcon(PWSTR* ppszIcon) noexcept
        {
            if (!ppszIcon)
                return E_POINTER;
            *ppszIcon = nullptr;

            std::wstring iconSpec;
            if (!BuildShellExtIconResource(iconSpec, kShellExtToggleIconId))
                return E_FAIL;
            return ::SHStrDupW(iconSpec.c_str(), ppszIcon);
        }

        static HRESULT GetCanonicalName(GUID* pguidCommandName) noexcept
        {
            if (!pguidCommandName)
                return E_POINTER;
            *pguidCommandName = kCanonical;
            return S_OK;
        }

        static HRESULT GetTitle(PWSTR* ppszName) noexcept
        {
            return DupTitleToOutParam(GlassDesktopActions::GetPlatformToggleTitle(kArch), ppszName);
        }

        static HRESULT GetState(EXPCMDSTATE* pCmdState) noexcept
        {
            *pCmdState = MenuStateToExplorerState(GlassDesktopActions::GetPlatformToggleMenuState(kArch));
            return S_OK;
        }

        static HRESULT GetFlags(EXPCMDFLAGS* pFlags) noexcept
        {
            *pFlags = ECF_DEFAULT;
            return S_OK;
        }

        static HRESULT EnumSubCommands(IEnumExplorerCommand** ppEnum) noexcept
        {
            if (!ppEnum)
                return E_POINTER;
            *ppEnum = nullptr;
            return E_NOTIMPL;
        }

        static void Invoke(HWND owner) noexcept
        {
            GlassDesktopActions::ExecutePlatformToggle(owner, kArch);
        }
    };

    using ToggleLeafInvoke = ToggleArchLeafInvoke<
        GlassShared::GlassPlatformTask::NativePlatformArch(),
        GUID_GlassShellToggleCommandCanonical>;
    using ToggleX86LeafInvoke = ToggleArchLeafInvoke<
        GlassShared::GlassPlatformTask::PlatformArch::X86,
        GUID_GlassShellToggleX86CommandCanonical>;
    using ToggleX64LeafInvoke = ToggleArchLeafInvoke<
        GlassShared::GlassPlatformTask::PlatformArch::X64,
        GUID_GlassShellToggleX64CommandCanonical>;

    struct OpenGlassLeafInvoke
    {
        static HRESULT GetIcon(PWSTR* ppszIcon) noexcept
        {
            if (!ppszIcon)
                return E_POINTER;
            *ppszIcon = nullptr;

            std::wstring iconSpec;
            if (!BuildShellExtIconResource(iconSpec, kShellExtOpenGlassIconId))
                return E_FAIL;
            return ::SHStrDupW(iconSpec.c_str(), ppszIcon);
        }

        static HRESULT GetCanonicalName(GUID* pguidCommandName) noexcept
        {
            if (!pguidCommandName)
                return E_POINTER;
            *pguidCommandName = GUID_GlassShellOpenGlassToggleCommandCanonical;
            return S_OK;
        }

        static HRESULT GetTitle(PWSTR* ppszName) noexcept
        {
            return DupTitleToOutParam(GlassDesktopActions::GetOpenGlassToggleTitle(), ppszName);
        }

        static HRESULT GetState(EXPCMDSTATE* pCmdState) noexcept
        {
            *pCmdState = MenuStateToExplorerState(GlassDesktopActions::GetOpenGlassToggleMenuState());
            return S_OK;
        }

        static HRESULT GetFlags(EXPCMDFLAGS* pFlags) noexcept
        {
            // Сепаратор перед OpenGlass (после Platform toggle) — как в Win32 DesktopShellMenu.
            *pFlags = static_cast<EXPCMDFLAGS>(ECF_DEFAULT | ECF_SEPARATORBEFORE);
            return S_OK;
        }

        static HRESULT EnumSubCommands(IEnumExplorerCommand** ppEnum) noexcept
        {
            if (!ppEnum)
                return E_POINTER;
            *ppEnum = nullptr;
            return E_NOTIMPL;
        }

        static void Invoke(HWND) noexcept { GlassDesktopActions::ExecuteOpenGlassToggle(nullptr); }
    };

    struct OpenConfigInvoke
    {
        static HRESULT GetIcon(PWSTR* ppszIcon) noexcept
        {
            if (!ppszIcon)
                return E_POINTER;
            *ppszIcon = nullptr;

            std::wstring iconSpec;
            if (!BuildShellExtIconResource(iconSpec, kShellExtSettingsIconId))
                return E_FAIL;
            return ::SHStrDupW(iconSpec.c_str(), ppszIcon);
        }

        static HRESULT GetCanonicalName(GUID* pguidCommandName) noexcept
        {
            if (!pguidCommandName)
                return E_POINTER;
            *pguidCommandName = GUID_GlassShellOpenConfigCommandCanonical;
            return S_OK;
        }

        static HRESULT GetTitle(PWSTR* ppszName) noexcept
        {
            return DupTitleToOutParam(
                GlassDesktopActions::LoadMenuString(IDS_GLASSSHELL_OPEN_CONFIG, L"Settings"),
                ppszName);
        }

        static HRESULT GetState(EXPCMDSTATE* pCmdState) noexcept
        {
            *pCmdState = ECS_ENABLED;
            return S_OK;
        }

        static HRESULT GetFlags(EXPCMDFLAGS* pFlags) noexcept
        {
            // Сепаратор перед «Настройки» — всегда (и без OpenGlass в списке).
            *pFlags = static_cast<EXPCMDFLAGS>(ECF_DEFAULT | ECF_SEPARATORBEFORE);
            return S_OK;
        }

        static HRESULT EnumSubCommands(IEnumExplorerCommand** ppEnum) noexcept
        {
            if (!ppEnum)
                return E_POINTER;
            *ppEnum = nullptr;
            return E_NOTIMPL;
        }

        static void Invoke(HWND owner) noexcept { GlassDesktopActions::LaunchGlassSettings(owner); }
    };

    struct RootInvoke
    {
        static HRESULT GetIcon(PWSTR* ppszIcon) noexcept
        {
            if (!ppszIcon)
                return E_POINTER;
            *ppszIcon = nullptr;

            std::wstring iconSpec;
            if (!BuildShellExtIconResource(iconSpec, kShellExtRootIconId))
                return E_FAIL;

            return ::SHStrDupW(iconSpec.c_str(), ppszIcon);
        }

        static HRESULT GetCanonicalName(GUID* pguidCommandName) noexcept
        {
            if (!pguidCommandName)
                return E_POINTER;
            *pguidCommandName = GUID_GlassShellToggleCommandCanonical;
            return S_OK;
        }

        static HRESULT GetTitle(PWSTR* ppszName) noexcept
        {
            return DupTitleToOutParam(
                GlassDesktopActions::LoadMenuString(IDS_GLASSSHELL_ROOT_TITLE, L"Glass Platform"),
                ppszName);
        }

        static HRESULT GetState(EXPCMDSTATE* pCmdState) noexcept
        {
            *pCmdState = ECS_ENABLED;
            return S_OK;
        }

        static HRESULT GetFlags(EXPCMDFLAGS* pFlags) noexcept
        {
            *pFlags = ECF_HASSUBCOMMANDS;
            return S_OK;
        }

        static HRESULT EnumSubCommands(IEnumExplorerCommand** ppEnum) noexcept
        {
            if (!ppEnum)
                return E_POINTER;
            *ppEnum = nullptr;

            std::vector<IExplorerCommand*> items;
            auto releaseAll = [&]() noexcept
            {
                for (IExplorerCommand* c : items)
                {
                    if (c)
                        c->Release();
                }
                items.clear();
            };

            HRESULT hr = S_OK;
            if (GlassDesktopActions::ShowDualPlatformMenu())
            {
                IExplorerCommand* toggleX86 = nullptr;
                hr = CreateExplorerCommand<ToggleX86LeafInvoke>(
                    IID_IExplorerCommand, reinterpret_cast<void**>(&toggleX86));
                if (FAILED(hr))
                    return hr;
                items.push_back(toggleX86);

                IExplorerCommand* toggleX64 = nullptr;
                hr = CreateExplorerCommand<ToggleX64LeafInvoke>(
                    IID_IExplorerCommand, reinterpret_cast<void**>(&toggleX64));
                if (FAILED(hr))
                {
                    releaseAll();
                    return hr;
                }
                items.push_back(toggleX64);
            }
            else
            {
                IExplorerCommand* toggle = nullptr;
                hr = CreateExplorerCommand<ToggleLeafInvoke>(
                    IID_IExplorerCommand, reinterpret_cast<void**>(&toggle));
                if (FAILED(hr))
                    return hr;
                items.push_back(toggle);
            }

            IExplorerCommand* openGlass = nullptr;
            if (GlassDesktopActions::IsOpenGlassMenuVisible())
            {
                hr = CreateExplorerCommand<OpenGlassLeafInvoke>(
                    IID_IExplorerCommand, reinterpret_cast<void**>(&openGlass));
                if (FAILED(hr))
                {
                    releaseAll();
                    return hr;
                }
                items.push_back(openGlass);
            }

            IExplorerCommand* openConfig = nullptr;
            hr = CreateExplorerCommand<OpenConfigInvoke>(IID_IExplorerCommand, reinterpret_cast<void**>(&openConfig));
            if (FAILED(hr))
            {
                releaseAll();
                return hr;
            }
            items.push_back(openConfig);

            const std::vector<IExplorerCommand*> toRelease = items;
            auto* e = new (std::nothrow) ExplorerCommandEnum(std::move(items));
            for (IExplorerCommand* c : toRelease)
                c->Release();
            if (!e)
                return E_OUTOFMEMORY;

            *ppEnum = e;
            return S_OK;
        }

        static void Invoke(HWND) noexcept {}
    };

    template <typename InvokeFn>
    [[nodiscard]] HRESULT CreateExplorerCommand(REFIID riid, void** ppv) noexcept
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;

        auto* cmd = new (std::nothrow) GlassDesktopExplorerCommandBase<InvokeFn>(InvokeFn{});
        if (!cmd)
            return E_OUTOFMEMORY;

        HRESULT hr = cmd->QueryInterface(riid, ppv);
        cmd->Release();
        return hr;
    }
}

volatile LONG g_liveExplorerCommandObjects = 0;

HRESULT GlassShell_CreateToggleExplorerCommand(REFIID riid, void** ppv) noexcept
{
    return CreateExplorerCommand<RootInvoke>(riid, ppv);
}

HRESULT GlassShell_CreateOpenConfigExplorerCommand(REFIID riid, void** ppv) noexcept
{
    return CreateExplorerCommand<OpenConfigInvoke>(riid, ppv);
}

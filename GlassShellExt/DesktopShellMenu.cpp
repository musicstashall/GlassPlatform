#include "GlassShellPch.h"

#include "DesktopExplorerFactory.h"
#include "DesktopShellActions.h"
#include "GlassShellExtClsids.h"
#include "GlassShellModule.h"
#include "resource.h"

#include <GlassShellMenuConstants.h>
#include <GlassShellRegistration.h>

#include <new>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
    constexpr HRESULT kSelfRegEClass = static_cast<HRESULT>(0x80040201L);

    volatile LONG g_comServerLocks = 0;
    volatile LONG g_liveMenuObjects = 0;
    volatile LONG g_liveFactoryObjects = 0;

    void FlushShellCachesAfterExtensionRegistrationChange() noexcept
    {
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }

    enum class DesktopFactoryKind : int
    {
        LegacyContextMenu = 0,
        ToggleExplorerCommand = 1,
        OpenConfigExplorerCommand = 2,
    };
}

extern "C" void WINAPI GlassShell_RemoveLegacyContextMenuHandler() noexcept
{
    Shared::GlassShellRegistration::RemoveLegacyContextMenuHandler();
}

class GlassDesktopShellMenu final : public IShellExtInit, public IContextMenu
{
    LONG m_cRef = 1;
    std::vector<HBITMAP> m_bitmaps{};

    ~GlassDesktopShellMenu() noexcept
    {
        for (HBITMAP bmp : m_bitmaps)
            if (bmp) ::DeleteObject(bmp);
    }

    // Иконка пункта меню: иконка модуля → 16×16 32-битный HBITMAP для MIIM_BITMAP.
    HBITMAP MenuIcon(UINT iconId) noexcept
    {
        constexpr int size = 16;
        HICON icon = static_cast<HICON>(::LoadImageW(
            GlassShellExt::g_module, MAKEINTRESOURCEW(iconId), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (!icon) return nullptr;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp)
        {
            ::DestroyIcon(icon);
            return nullptr;
        }
        HDC dc = ::CreateCompatibleDC(nullptr);
        if (dc)
        {
            HGDIOBJ old = ::SelectObject(dc, bmp);
            ::DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
            if (old) ::SelectObject(dc, old);
            ::DeleteDC(dc);
        }
        ::DestroyIcon(icon);
        // Меню Windows использует альфа-канал HBITMAP: проставляем его по ненулевым пикселям.
        auto* px = static_cast<DWORD*>(bits);
        if (px)
        {
            for (int i = 0; i < size * size; ++i)
                px[i] = (px[i] & 0x00FFFFFFu) ? (px[i] | 0xFF000000u) : 0u;
        }
        m_bitmaps.push_back(bmp);
        return bmp;
    }

public:
    GlassDesktopShellMenu() noexcept { InterlockedIncrement(&g_liveMenuObjects); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;

        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IShellExtInit))
            *ppv = static_cast<IShellExtInit*>(this);
        else if (IsEqualIID(riid, IID_IContextMenu))
            *ppv = static_cast<IContextMenu*>(this);
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
            InterlockedDecrement(&g_liveMenuObjects);
            delete this;
        }
        return static_cast<ULONG>(c);
    }

    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject*, HKEY) noexcept override { return S_OK; }

    IFACEMETHODIMP QueryContextMenu(
        HMENU hmenu,
        UINT indexMenu,
        UINT idCmdFirst,
        UINT idCmdLast,
        UINT uFlags) noexcept override
    {
        if (uFlags & CMF_DEFAULTONLY)
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);

        const bool dual = GlassDesktopActions::ShowDualPlatformMenu();
        const bool openGlassVisible = GlassDesktopActions::IsOpenGlassMenuVisible();
        const UINT platformCount = dual ? 2u : 1u;
        const UINT commandCount = platformCount + (openGlassVisible ? 1u : 0u) + 1u;
        if (idCmdFirst + commandCount - 1 > idCmdLast)
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);

        // Классическое меню собирается единым вложенным меню «Glass Platform»
        // (пункт-родитель с иконкой) с иконками дочерних пунктов — структура
        // повторяет компактное меню Windows 11.
        HMENU sub = ::CreatePopupMenu();
        if (!sub)
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);

        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);

        UINT insertAt = 0;
        UINT cmdOffset = 0;

        auto insertPlatformToggle = [&](GlassShared::GlassPlatformTask::PlatformArch arch) -> bool
        {
            std::wstring label = GlassDesktopActions::GetPlatformToggleTitle(arch);
            mii.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_BITMAP;
            mii.fType = MFT_STRING;
            mii.wID = idCmdFirst + cmdOffset++;
            mii.fState = GlassDesktopActions::GetPlatformToggleMenuState(arch);
            mii.dwTypeData = label.data();
            mii.hbmpItem = MenuIcon(IDI_GLASSSHELLEXT_PT);
            return ::InsertMenuItemW(sub, insertAt++, TRUE, &mii) != FALSE;
        };

        auto insertSeparator = [&]() -> bool
        {
            mii.fMask = MIIM_FTYPE;
            mii.fType = MFT_SEPARATOR;
            mii.fState = 0;
            mii.wID = 0;
            mii.dwTypeData = nullptr;
            mii.hbmpItem = nullptr;
            return ::InsertMenuItemW(sub, insertAt++, TRUE, &mii) != FALSE;
        };

        if (dual)
        {
            if (!insertPlatformToggle(GlassShared::GlassPlatformTask::PlatformArch::X86))
            {
                ::DestroyMenu(sub);
                return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
            }
            if (!insertPlatformToggle(GlassShared::GlassPlatformTask::PlatformArch::X64))
            {
                ::DestroyMenu(sub);
                return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 1);
            }
        }
        else if (!insertPlatformToggle(GlassShared::GlassPlatformTask::NativePlatformArch()))
        {
            ::DestroyMenu(sub);
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        }

        // Порядок: Platform → | → OpenGlass → | → Settings
        if (openGlassVisible)
        {
            if (!insertSeparator())
            {
                ::DestroyMenu(sub);
                return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, cmdOffset);
            }

            std::wstring labelOg = GlassDesktopActions::GetOpenGlassToggleTitle();
            mii.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_BITMAP;
            mii.fType = MFT_STRING;
            mii.wID = idCmdFirst + cmdOffset++;
            mii.fState = GlassDesktopActions::GetOpenGlassToggleMenuState();
            mii.dwTypeData = labelOg.data();
            mii.hbmpItem = MenuIcon(IDI_GLASSSHELLEXT_OG);
            if (!::InsertMenuItemW(sub, insertAt++, TRUE, &mii))
            {
                ::DestroyMenu(sub);
                return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, cmdOffset - 1);
            }
        }

        if (!insertSeparator())
        {
            ::DestroyMenu(sub);
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, cmdOffset);
        }

        std::wstring labelCfg =
            GlassDesktopActions::LoadMenuString(IDS_GLASSSHELL_OPEN_CONFIG, L"Settings");
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_BITMAP;
        mii.fType = MFT_STRING;
        mii.wID = idCmdFirst + cmdOffset++;
        mii.fState = MFS_ENABLED;
        mii.dwTypeData = labelCfg.data();
        mii.hbmpItem = MenuIcon(IDI_GLASSSHELLEXT_ST);
        if (!::InsertMenuItemW(sub, insertAt, TRUE, &mii))
        {
            ::DestroyMenu(sub);
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, cmdOffset - 1);
        }

        // Родительский пункт поверх общего меню: всё содержимое — внутри sub.
        std::wstring labelRoot =
            GlassDesktopActions::LoadMenuString(IDS_GLASSSHELL_ROOT_TITLE, L"Glass Platform");
        MENUITEMINFOW root{};
        root.cbSize = sizeof(root);
        root.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_BITMAP | MIIM_STATE;
        root.fState = MFS_ENABLED;
        root.hSubMenu = sub;
        root.dwTypeData = labelRoot.data();
        root.hbmpItem = MenuIcon(IDI_GLASSSHELLEXT);
        if (!::InsertMenuItemW(hmenu, indexMenu, TRUE, &root))
        {
            ::DestroyMenu(sub);
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        }

        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, cmdOffset);
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* pici) noexcept override
    {
        if (!pici || pici->cbSize < sizeof(CMINVOKECOMMANDINFO))
            return E_INVALIDARG;

        UINT cmdOffset = static_cast<UINT>(-1);

        const UINT_PTR verbNum = reinterpret_cast<UINT_PTR>(pici->lpVerb);
        if (HIWORD(verbNum) == 0)
            cmdOffset = LOWORD(verbNum);
        else if (pici->cbSize >= sizeof(CMINVOKECOMMANDINFOEX))
        {
            const auto* ex = reinterpret_cast<const CMINVOKECOMMANDINFOEX*>(pici);
            if ((ex->fMask & CMIC_MASK_UNICODE) != 0 && ex->lpVerbW != nullptr)
            {
                if (ex->lpVerbW[0] >= L'0' && ex->lpVerbW[0] <= L'9' && ex->lpVerbW[1] == L'\0')
                    cmdOffset = static_cast<UINT>(ex->lpVerbW[0] - L'0');
            }
        }

        if (cmdOffset == static_cast<UINT>(-1) && pici->lpVerb != nullptr)
        {
            const char* const a = reinterpret_cast<const char*>(pici->lpVerb);
            if (a[0] >= '0' && a[0] <= '9' && a[1] == '\0')
                cmdOffset = static_cast<UINT>(a[0] - '0');
        }

        const bool dual = GlassDesktopActions::ShowDualPlatformMenu();
        const bool openGlassVisible = GlassDesktopActions::IsOpenGlassMenuVisible();
        const UINT platformCount = dual ? 2u : 1u;
        const UINT maxOffset = platformCount + (openGlassVisible ? 1u : 0u);
        if (cmdOffset > maxOffset)
            return E_INVALIDARG;

        if (dual)
        {
            if (cmdOffset == 0)
            {
                GlassDesktopActions::ExecutePlatformToggle(
                    pici->hwnd, GlassShared::GlassPlatformTask::PlatformArch::X86);
                return S_OK;
            }
            if (cmdOffset == 1)
            {
                GlassDesktopActions::ExecutePlatformToggle(
                    pici->hwnd, GlassShared::GlassPlatformTask::PlatformArch::X64);
                return S_OK;
            }
        }
        else if (cmdOffset == 0)
        {
            GlassDesktopActions::ExecutePlatformToggle(pici->hwnd);
            return S_OK;
        }

        if (openGlassVisible && cmdOffset == platformCount)
        {
            GlassDesktopActions::ExecuteOpenGlassToggle(pici->hwnd);
            return S_OK;
        }

        if (cmdOffset == maxOffset)
        {
            GlassDesktopActions::LaunchGlassSettings(pici->hwnd);
            return S_OK;
        }

        return E_INVALIDARG;
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) noexcept override { return E_NOTIMPL; }
};

class GlassShellClassFactory final : public IClassFactory
{
    LONG m_cRef = 1;
    const DesktopFactoryKind m_kind;

public:
    explicit GlassShellClassFactory(DesktopFactoryKind kind) noexcept : m_kind(kind)
    {
        InterlockedIncrement(&g_liveFactoryObjects);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() noexcept override { return static_cast<ULONG>(InterlockedIncrement(&m_cRef)); }

    IFACEMETHODIMP_(ULONG) Release() noexcept override
    {
        const LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0)
        {
            InterlockedDecrement(&g_liveFactoryObjects);
            delete this;
        }
        return static_cast<ULONG>(c);
    }

    IFACEMETHODIMP CreateInstance(IUnknown* punkOuter, REFIID riid, void** ppv) noexcept override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (punkOuter)
            return CLASS_E_NOAGGREGATION;

        switch (m_kind)
        {
        case DesktopFactoryKind::LegacyContextMenu:
        {
            GlassDesktopShellMenu* p = new (std::nothrow) GlassDesktopShellMenu();
            if (!p)
                return E_OUTOFMEMORY;
            HRESULT hr = p->QueryInterface(riid, ppv);
            p->Release();
            return hr;
        }
        case DesktopFactoryKind::ToggleExplorerCommand:
            return GlassShell_CreateToggleExplorerCommand(riid, ppv);
        case DesktopFactoryKind::OpenConfigExplorerCommand:
            return GlassShell_CreateOpenConfigExplorerCommand(riid, ppv);
        default:
            return E_FAIL;
        }
    }

    IFACEMETHODIMP LockServer(BOOL lock) noexcept override
    {
        if (lock)
            InterlockedIncrement(&g_comServerLocks);
        else
            InterlockedDecrement(&g_comServerLocks);
        return S_OK;
    }
};

// Классическое Win32-меню (ContextMenuHandlers + CLSID\InprocServer32 в HKCU) создаёт и снимает
// только сама DLL — здесь, через regsvr32 /s и /u /s. Никакой другой компонент (GlassSettings.Native,
// Setup, GlassUnins) эти ключи не пишет: Win10 — всегда так; Win11 — только как запасной вариант,
// когда sparse-пакет (компактное меню) зарегистрировать нельзя, и вызывающая сторона сама следит,
// чтобы оба варианта не были активны одновременно (иначе пункты дублируются в «Показать дополнительно»).
STDAPI DllRegisterServer()
{
    wchar_t module[MAX_PATH]{};
    if (::GetModuleFileNameW(GlassShellExt::g_module, module, MAX_PATH) == 0 &&
        ::GetModuleFileNameW(nullptr, module, MAX_PATH) == 0)
        return kSelfRegEClass;

    Shared::GlassShellRegistration::RemoveStaleExplorerCommandRegistration();
    Shared::GlassShellRegistration::RemoveLegacyContextMenuHandler();

    if (!Shared::GlassShellRegistration::RegisterLegacyContextMenuHandler(module))
        return kSelfRegEClass;

    // Компактное меню Win11 (IExplorerCommand) — только sparse AppxManifest; HKCU ExplorerCommandHandler
    // здесь не пишется, иначе пункты дублируются в классическом меню.
    FlushShellCachesAfterExtensionRegistrationChange();
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    Shared::GlassShellRegistration::RemoveStaleExplorerCommandRegistration();
    Shared::GlassShellRegistration::RemoveLegacyContextMenuHandler();

    // IExplorerCommand для Win11 — только sparse AppX; InprocServer32 toggle/config не трогаем.
    FlushShellCachesAfterExtensionRegistrationChange();
    return S_OK;
}

STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _COM_Outptr_ void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    DesktopFactoryKind kind = DesktopFactoryKind::LegacyContextMenu;
    if (IsEqualCLSID(rclsid, CLSID_GlassDesktopShellMenu))
        kind = DesktopFactoryKind::LegacyContextMenu;
    else if (IsEqualCLSID(rclsid, CLSID_GlassDesktopToggleExplorerCommand))
        kind = DesktopFactoryKind::ToggleExplorerCommand;
    else if (IsEqualCLSID(rclsid, CLSID_GlassDesktopOpenConfigExplorerCommand))
        kind = DesktopFactoryKind::OpenConfigExplorerCommand;
    else
        return CLASS_E_CLASSNOTAVAILABLE;

    GlassShellClassFactory* factory = new (std::nothrow) GlassShellClassFactory(kind);
    if (!factory)
        return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    if (::InterlockedCompareExchange(&g_comServerLocks, 0, 0) != 0)
        return S_FALSE;
    if (::InterlockedCompareExchange(&g_liveMenuObjects, 0, 0) != 0)
        return S_FALSE;
    if (::InterlockedCompareExchange(&g_liveFactoryObjects, 0, 0) != 0)
        return S_FALSE;
    if (::InterlockedCompareExchange(&g_liveExplorerCommandObjects, 0, 0) != 0)
        return S_FALSE;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        GlassDesktopActions::BindModule(hinst);
    return TRUE;
}
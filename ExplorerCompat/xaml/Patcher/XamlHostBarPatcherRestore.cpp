#include "ExplorerPch.h"
#include "xaml/Patcher/XamlHostBarPatcherInternals.h"

#include <OleAuto.h>
#include <windows.ui.xaml.media.h>
#include <windows.ui.xaml.controls.h>
#include <winstring.h>
#include <DebugLog.h>

namespace XamlHostBarPatcher::Detail
{
    HRESULT CreateSolidColorBrushArgb(
        BYTE a,
        BYTE r,
        BYTE g,
        BYTE b,
        ABI::Windows::UI::Xaml::Media::ISolidColorBrush** out) noexcept
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;

        HSTRING_HEADER hdr{};
        HSTRING className = nullptr;
        HRESULT hr = WindowsCreateStringReference(
            L"Windows.UI.Xaml.Media.SolidColorBrush",
            static_cast<UINT32>(wcslen(L"Windows.UI.Xaml.Media.SolidColorBrush")),
            &hdr,
            &className);
        if (FAILED(hr))
            return hr;

        IInspectable* insp = nullptr;
        hr = RoActivateInstance(className, &insp);
        if (FAILED(hr))
            return hr;

        ABI::Windows::UI::Xaml::Media::ISolidColorBrush* brush = nullptr;
        hr = insp->QueryInterface(
            __uuidof(ABI::Windows::UI::Xaml::Media::ISolidColorBrush),
            reinterpret_cast<void**>(&brush));
        insp->Release();
        if (FAILED(hr))
            return hr;

        ABI::Windows::UI::Color color{};
        color.A = a;
        color.R = r;
        color.G = g;
        color.B = b;
        hr = brush->put_Color(color);
        if (FAILED(hr))
        {
            brush->Release();
            return hr;
        }

        *out = brush;
        return S_OK;
    }

    HRESULT TrySetBackgroundViaComInterface(
        IInspectable* insp,
        BYTE a,
        BYTE r,
        BYTE g,
        BYTE b) noexcept
    {
        if (!insp)
            return E_INVALIDARG;

        ABI::Windows::UI::Xaml::Media::ISolidColorBrush* brush = nullptr;
        HRESULT hr = CreateSolidColorBrushArgb(a, r, g, b, &brush);
        if (FAILED(hr) || !brush)
        {
            DebugLog(DBG_WARNING, L"[ExplorerCompat][Popup] CreateSolidColorBrushArgb failed: hr=0x%08X", hr);
            return hr;
        }

        ABI::Windows::UI::Xaml::Media::IBrush* brushAsBrush = nullptr;
        hr = brush->QueryInterface(
            __uuidof(ABI::Windows::UI::Xaml::Media::IBrush),
            reinterpret_cast<void**>(&brushAsBrush));
        brush->Release();
        if (FAILED(hr) || !brushAsBrush)
        {
            DebugLog(DBG_WARNING, L"[ExplorerCompat][Popup] Brush->QI(IBrush) failed: hr=0x%08X", hr);
            return hr;
        }

        DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][Popup] Brush created successfully, trying IPanel/IControl/IBorder...");

        // Попробовать IPanel
        auto tryPanel = [&]() -> HRESULT {
            ABI::Windows::UI::Xaml::Controls::IPanel* panel = nullptr;
            HRESULT hrQI = insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::Controls::IPanel),
                reinterpret_cast<void**>(&panel));
            if (FAILED(hrQI))
            {
                DebugLog(DBG_NORMAL, L"[ExplorerCompat][Popup] TryPanel QI failed: hr=0x%08X", hrQI);
                return hrQI;
            }
            if (!panel)
                return E_NOINTERFACE;
            HRESULT hrL = panel->put_Background(brushAsBrush);
            panel->Release();
            return hrL;
        };

        // Попробовать IControl
        auto tryControl = [&]() -> HRESULT {
            ABI::Windows::UI::Xaml::Controls::IControl* ctl = nullptr;
            HRESULT hrQI = insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::Controls::IControl),
                reinterpret_cast<void**>(&ctl));
            if (FAILED(hrQI))
            {
                DebugLog(DBG_NORMAL, L"[ExplorerCompat][Popup] TryControl QI failed: hr=0x%08X", hrQI);
                return hrQI;
            }
            if (!ctl)
                return E_NOINTERFACE;
            HRESULT hrL = ctl->put_Background(brushAsBrush);
            ctl->Release();
            return hrL;
        };

        // Попробовать IBorder
        auto tryBorder = [&]() -> HRESULT {
            ABI::Windows::UI::Xaml::Controls::IBorder* border = nullptr;
            HRESULT hrQI = insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::Controls::IBorder),
                reinterpret_cast<void**>(&border));
            if (FAILED(hrQI))
            {
                DebugLog(DBG_NORMAL, L"[ExplorerCompat][Popup] TryBorder QI failed: hr=0x%08X", hrQI);
                return hrQI;
            }
            if (!border)
                return E_NOINTERFACE;
            HRESULT hrL = border->put_Background(brushAsBrush);
            border->Release();
            return hrL;
        };

        HRESULT result = E_NOINTERFACE;
        if (SUCCEEDED(tryPanel()))
            result = S_OK;
        else if (SUCCEEDED(tryControl()))
            result = S_OK;
        else if (SUCCEEDED(tryBorder()))
            result = S_OK;

        brushAsBrush->Release();
        return result;
    }

    HRESULT CreateAcrylicBrushArgb(
        BYTE a,
        BYTE r,
        BYTE g,
        BYTE b,
        float tintOpacity,
        ABI::Windows::UI::Xaml::Media::IAcrylicBrush** out) noexcept
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;

        HSTRING_HEADER hdr{};
        HSTRING className = nullptr;
        HRESULT hr = WindowsCreateStringReference(
            L"Microsoft.UI.Xaml.Media.AcrylicBrush",
            static_cast<UINT32>(wcslen(L"Microsoft.UI.Xaml.Media.AcrylicBrush")),
            &hdr,
            &className);
        if (FAILED(hr))
            return hr;

        IInspectable* insp = nullptr;
        hr = RoActivateInstance(className, &insp);
        if (FAILED(hr))
            return hr;

        ABI::Windows::UI::Xaml::Media::IAcrylicBrush* brush = nullptr;
        hr = insp->QueryInterface(
            __uuidof(ABI::Windows::UI::Xaml::Media::IAcrylicBrush),
            reinterpret_cast<void**>(&brush));
        insp->Release();
        if (FAILED(hr))
            return hr;

        // Установить TintColor
        ABI::Windows::UI::Color color{};
        color.A = a;
        color.R = r;
        color.G = g;
        color.B = b;
        hr = brush->put_TintColor(color);
        if (FAILED(hr))
        {
            brush->Release();
            return hr;
        }

        // Установить TintOpacity
        hr = brush->put_TintOpacity(static_cast<double>(tintOpacity));
        if (FAILED(hr))
        {
            brush->Release();
            return hr;
        }

        // Установить BackgroundSource = Backdrop
        hr = brush->put_BackgroundSource(ABI::Windows::UI::Xaml::Media::AcrylicBackgroundSource_Backdrop);
        if (FAILED(hr))
        {
            brush->Release();
            return hr;
        }

        *out = brush;
        return S_OK;
    }

    HRESULT TrySetBackgroundViaComInterfaceAcrylic(
        IInspectable* insp,
        BYTE a,
        BYTE r,
        BYTE g,
        BYTE b,
        float tintOpacity) noexcept
    {
        if (!insp)
            return E_INVALIDARG;

        ABI::Windows::UI::Xaml::Media::IAcrylicBrush* brush = nullptr;
        HRESULT hr = CreateAcrylicBrushArgb(a, r, g, b, tintOpacity, &brush);
        if (FAILED(hr) || !brush)
        {
            DebugLog(DBG_WARNING, L"[ExplorerCompat][Popup] CreateAcrylicBrushArgb failed: hr=0x%08X", hr);
            return hr;
        }

        ABI::Windows::UI::Xaml::Media::IBrush* brushAsBrush = nullptr;
        hr = brush->QueryInterface(
            __uuidof(ABI::Windows::UI::Xaml::Media::IBrush),
            reinterpret_cast<void**>(&brushAsBrush));
        brush->Release();
        if (FAILED(hr) || !brushAsBrush)
        {
            DebugLog(DBG_WARNING, L"[ExplorerCompat][Popup] AcrylicBrush->QI(IBrush) failed: hr=0x%08X", hr);
            return hr;
        }

        DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][Popup] AcrylicBrush created successfully, trying IPanel/IControl/IBorder...");

        // Попробовать IPanel
        auto tryPanel = [&]() -> HRESULT {
            ABI::Windows::UI::Xaml::Controls::IPanel* panel = nullptr;
            HRESULT hrQI = insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::Controls::IPanel),
                reinterpret_cast<void**>(&panel));
            if (FAILED(hrQI))
            {
                DebugLog(DBG_NORMAL, L"[ExplorerCompat][Popup] TryPanel QI failed: hr=0x%08X", hrQI);
                return hrQI;
            }
            if (!panel)
                return E_NOINTERFACE;
            HRESULT hrL = panel->put_Background(brushAsBrush);
            panel->Release();
            return hrL;
        };

        // Попробовать IControl
        auto tryControl = [&]() -> HRESULT {
            ABI::Windows::UI::Xaml::Controls::IControl* ctl = nullptr;
            HRESULT hrQI = insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::Controls::IControl),
                reinterpret_cast<void**>(&ctl));
            if (FAILED(hrQI))
            {
                DebugLog(DBG_NORMAL, L"[ExplorerCompat][Popup] TryControl QI failed: hr=0x%08X", hrQI);
                return hrQI;
            }
            if (!ctl)
                return E_NOINTERFACE;
            HRESULT hrL = ctl->put_Background(brushAsBrush);
            ctl->Release();
            return hrL;
        };

        // Попробовать IBorder
        auto tryBorder = [&]() -> HRESULT {
            ABI::Windows::UI::Xaml::Controls::IBorder* border = nullptr;
            HRESULT hrQI = insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::Controls::IBorder),
                reinterpret_cast<void**>(&border));
            if (FAILED(hrQI))
            {
                DebugLog(DBG_NORMAL, L"[ExplorerCompat][Popup] TryBorder QI failed: hr=0x%08X", hrQI);
                return hrQI;
            }
            if (!border)
                return E_NOINTERFACE;
            HRESULT hrL = border->put_Background(brushAsBrush);
            border->Release();
            return hrL;
        };

        HRESULT result = E_NOINTERFACE;
        if (SUCCEEDED(tryPanel()))
            result = S_OK;
        else if (SUCCEEDED(tryControl()))
            result = S_OK;
        else if (SUCCEEDED(tryBorder()))
            result = S_OK;

        brushAsBrush->Release();
        return result;
    }
} // namespace XamlHostBarPatcher::Detail

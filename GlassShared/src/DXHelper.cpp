#include "SharedPch.h"
#include <dx.h>
#include <DXHelper.h>

namespace Shared::DXHelper
{
    void ReleaseAllLazyGraphicsResources() noexcept
    {
        LazyDComposition::GetInstance().ReleaseResources();
        LazyD3D::GetInstance().ReleaseResources();
        LazyD2D::GetInstance().ReleaseResources();
    }

    D2D1::ColorF MakeColorF(DWORD argb)
    {
        float a = ((argb >> 24) & 0xFF) / 255.0f;
        float r = ((argb >> 16) & 0xFF) / 255.0f;
        float g = ((argb >> 8) & 0xFF) / 255.0f;
        float b = (argb & 0xFF) / 255.0f;
        return D2D1::ColorF(r, g, b, a);
    }

    void LazyDX::EnsureResources()
    {
        if (!m_resourcesCreated)
        {
            CreateDeviceIndependentResources();
            CreateDeviceResources();
            m_resourcesCreated = true;
        }
    }

    void LazyDX::ReleaseResources()
    {
        if (m_resourcesCreated)
        {
            DestroyDeviceResources();
            DestroyDeviceIndependentResources();
            m_resourcesCreated = false;
        }
    }

    // LazyD2D
    LazyD2D& LazyD2D::GetInstance()
    {
        static LazyD2D instance;
        return instance;
    }

    void LazyD2D::CreateDeviceIndependentResources()
    {
        D2D1CreateFactory<ID2D1Factory>(D2D1_FACTORY_TYPE_MULTI_THREADED, &m_factory);
    }

    void LazyD2D::CreateDeviceResources()
    {
        if (!m_factory) return;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        wil::com_ptr<ID2D1DCRenderTarget> rt;
        if (SUCCEEDED(m_factory->CreateDCRenderTarget(&props, &rt)))
        {
            m_dcRT = rt;
            m_dcRT->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
    }

    void LazyD2D::DestroyDeviceIndependentResources() noexcept
    {
        m_factory.reset();
    }

    void LazyD2D::DestroyDeviceResources() noexcept
    {
        m_dcRT.reset();
    }

    // LazyD3D
    LazyD3D& LazyD3D::GetInstance()
    {
        static LazyD3D instance;
        return instance;
    }

    void LazyD3D::CreateDeviceIndependentResources() {}
    void LazyD3D::CreateDeviceResources()
    {
        D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0,
            D3D11_SDK_VERSION,
            &m_d3dDevice,
            nullptr,
            nullptr
        );
        if (m_d3dDevice)
        {
            m_d3dDevice.query_to(&m_dxgiDevice);
        }
    }
    void LazyD3D::DestroyDeviceIndependentResources() noexcept {}
    void LazyD3D::DestroyDeviceResources() noexcept
    {
        m_d3dDevice.reset();
        m_dxgiDevice.reset();
    }

    // LazyDComposition
    LazyDComposition& LazyDComposition::GetInstance()
    {
        static LazyDComposition instance;
        return instance;
    }

    void LazyDComposition::CreateDeviceIndependentResources() {}
    void LazyDComposition::CreateDeviceResources()
    {
        auto& d3d = LazyD3D::GetInstance();
        d3d.EnsureResources();
        if (d3d.GetDxgiDevice())
        {
            DCompositionCreateDevice3(d3d.GetDxgiDevice().get(), IID_PPV_ARGS(&m_dcompDevice));
        }
    }
    void LazyDComposition::DestroyDeviceIndependentResources() noexcept {}
    void LazyDComposition::DestroyDeviceResources() noexcept
    {
        m_dcompDevice.reset();
    }
}
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wil/com.h>

namespace Shared::DXHelper
{
    D2D1::ColorF MakeColorF(DWORD argb);

    /// Сброс кэша DComp → D3D → D2D (порядок зависимостей). Вызывать перед выгрузкой модуля,
    /// где создавались устройства, иначе при статическом деструкторе wil::com_ptr возможен краш в Release().
    void ReleaseAllLazyGraphicsResources() noexcept;

    class LazyDX
    {
    public:
        LazyDX() = default;
        virtual ~LazyDX() = default;
        virtual void CreateDeviceIndependentResources() = 0;
        virtual void CreateDeviceResources() = 0;
        virtual void DestroyDeviceIndependentResources() noexcept = 0;
        virtual void DestroyDeviceResources() noexcept = 0;
        void EnsureResources();
        void ReleaseResources();
    protected:
        bool m_resourcesCreated = false;
    };

    class LazyD2D : public LazyDX
    {
    public:
        static LazyD2D& GetInstance();
        ~LazyD2D() override = default;
        wil::com_ptr<ID2D1DCRenderTarget> GetRenderTarget() const { return m_dcRT; }
        wil::com_ptr<ID2D1Factory> GetFactory() const { return m_factory; }
    protected:
        void CreateDeviceIndependentResources() override;
        void CreateDeviceResources() override;
        void DestroyDeviceIndependentResources() noexcept override;
        void DestroyDeviceResources() noexcept override;
    private:
        LazyD2D() = default;
        wil::com_ptr<ID2D1DCRenderTarget> m_dcRT;
        wil::com_ptr<ID2D1Factory> m_factory;
    };

    class LazyD3D : public LazyDX
    {
    public:
        static LazyD3D& GetInstance();
        ~LazyD3D() override = default;
        wil::com_ptr<IDXGIDevice3> GetDxgiDevice() const { return m_dxgiDevice; }
        wil::com_ptr<ID3D11Device> GetD3DDevice() const { return m_d3dDevice; }
    protected:
        void CreateDeviceIndependentResources() override;
        void CreateDeviceResources() override;
        void DestroyDeviceIndependentResources() noexcept override;
        void DestroyDeviceResources() noexcept override;
    private:
        LazyD3D() = default;
        wil::com_ptr<IDXGIDevice3> m_dxgiDevice;
        wil::com_ptr<ID3D11Device> m_d3dDevice;
    };

    class LazyDComposition : public LazyDX
    {
    public:
        static LazyDComposition& GetInstance();
        ~LazyDComposition() override = default;
        wil::com_ptr<IDCompositionDesktopDevice> GetDCompositionDevice() const { return m_dcompDevice; }
    protected:
        void CreateDeviceIndependentResources() override;
        void CreateDeviceResources() override;
        void DestroyDeviceIndependentResources() noexcept override;
        void DestroyDeviceResources() noexcept override;
    private:
        LazyDComposition() = default;
        wil::com_ptr<IDCompositionDesktopDevice> m_dcompDevice;
    };
}
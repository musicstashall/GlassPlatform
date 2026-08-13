#include "ExplorerPch.h"
#include "DCompUtils.hpp"

DCompHandle DCompUtils::CreateLayerForWindow(HWND hwnd)
{
    DCompHandle result;

    if (FAILED(DCompositionCreateDevice(
        nullptr,
        __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(&result.Device))))
    {
        return {};
    }

    if (FAILED(result.Device->CreateVisual(&result.Visual)))
    {
        DestroyLayer(result);
        return {};
    }

    if (FAILED(result.Device->CreateTargetForHwnd(hwnd, TRUE, &result.Target)))
    {
        DestroyLayer(result);
        return {};
    }

    result.Target->SetRoot(result.Visual);
    result.Device->Commit();

    return result;
}

void DCompUtils::DestroyLayer(DCompHandle& handle)
{
    if (handle.Visual) { handle.Visual->Release();  handle.Visual = nullptr; }
    if (handle.Target) { handle.Target->Release();  handle.Target = nullptr; }
    if (handle.Device) { handle.Device->Release();  handle.Device = nullptr; }
}

void DCompUtils::ApplyAppearance(
    const DCompHandle& handle,
    ExplorerAccent accent,
    const Util::Color& color,
    float blurRadius)
{
    if (!handle.IsValid())
        return;

    switch (accent)
    {
    case ExplorerAccent::None:
        ApplyNone(handle);
        break;

    case ExplorerAccent::Blur:
        ApplyBlur(handle, color, blurRadius);
        break;

    case ExplorerAccent::Acrylic:
        ApplyAcrylic(handle, color);
        break;

    case ExplorerAccent::MicaLike:
        ApplyMicaLike(handle, color);
        break;
    }

    handle.Device->Commit();
}

void DCompUtils::ApplyNone(const DCompHandle& handle)
{
    // Пока ничего не делаем — базовый каркас.
    // Позже сюда добавим логику отключения эффекта.
    (void)handle;
}

void DCompUtils::ApplyBlur(const DCompHandle& handle, const Util::Color& color, float radius)
{
    // Заглушка: здесь будет реальный blur через эффекты DComp.
    (void)handle;
    (void)color;
    (void)radius;
}

void DCompUtils::ApplyAcrylic(const DCompHandle& handle, const Util::Color& color)
{
    // Заглушка: здесь будет Acrylic-пайплайн.
    (void)handle;
    (void)color;
}

void DCompUtils::ApplyMicaLike(const DCompHandle& handle, const Util::Color& color)
{
    // Заглушка: здесь будет Mica-подобный эффект.
    (void)handle;
    (void)color;
}

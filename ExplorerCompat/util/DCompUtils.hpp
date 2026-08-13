#pragma once

#include "../appearance/ExplorerAppearanceConfig.hpp"
#include "Color.hpp"

// Forward declarations — чтобы не тянуть dcomp.h во все TU
struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;

struct DCompHandle
{
    IDCompositionDevice* Device = nullptr;
    IDCompositionTarget* Target = nullptr;
    IDCompositionVisual* Visual = nullptr;

    bool IsValid() const noexcept
    {
        return Device && Target && Visual;
    }
};

class DCompUtils
{
public:
    static DCompHandle CreateLayerForWindow(HWND hwnd);

    static void ApplyAppearance(
        const DCompHandle& handle,
        ExplorerAccent accent,
        const Util::Color& color,
        float blurRadius
    );

    static void DestroyLayer(DCompHandle& handle);

private:
    static void ApplyNone(const DCompHandle& handle);
    static void ApplyBlur(const DCompHandle& handle, const Util::Color& color, float radius);
    static void ApplyAcrylic(const DCompHandle& handle, const Util::Color& color);
    static void ApplyMicaLike(const DCompHandle& handle, const Util::Color& color);
};

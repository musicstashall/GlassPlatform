#pragma once
#include <guiddef.h>

// Уникальный CLSID TAP для GlassPlatform (WinUI). Не пересекается с ExplorerBlurMica
// CLSID_TAPSite. CLSID UWP TAP удалён вместе с UwpXamlTapSite — таск-бар переехал в
// отдельный модуль TaskbarCompat.dll, где используется собственный CLSID.
// {7A4E2C10-B5F3-4D89-9E1A-0F2B3C4D5E60}
inline constexpr CLSID CLSID_GlassXamlTapSite = {
    0x7a4e2c10, 0xb5f3, 0x4d89,
    { 0x9e, 0x1a, 0x0f, 0x2b, 0x3c, 0x4d, 0x5e, 0x60 }
};

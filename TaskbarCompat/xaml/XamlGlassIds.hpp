#pragma once
#include <guiddef.h>

// Уникальный CLSID UWP TAP для панели задач Win11 (Windows.UI.Xaml.dll).
// {E3A91D2F-8C74-4E1B-B4F2-7A8E9C0D1E3F}
//
// CLSID должен отличаться от ExplorerCompat и TranslucentTB: TaskbarCompat держит
// собственную UDK-сессию с собственным endpoint (см. XamlDiagInit.cpp).
inline constexpr CLSID CLSID_GlassTaskbarTapSite = {
    0xe3a91d2f, 0x8c74, 0x4e1b,
    { 0xb4, 0xf2, 0x7a, 0x8e, 0x9c, 0x0d, 0x1e, 0x3f }
};

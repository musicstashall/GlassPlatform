#pragma once
#include <guiddef.h>

// Уникальный CLSID UWP TAP для CoreWindow hosts (SEH / ShellHost / SMEH).
// {A1B2C3D4-E5F6-4789-A012-3456789ABCDE}
// Отличается от TaskbarCompat и ExplorerCompat.
inline constexpr CLSID CLSID_GlassUICompatTapSite = {
    0xa1b2c3d4, 0xe5f6, 0x4789,
    { 0xa0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde }
};

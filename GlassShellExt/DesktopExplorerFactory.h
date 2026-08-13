#pragma once

#include <guiddef.h>

extern volatile LONG g_liveExplorerCommandObjects;

[[nodiscard]] HRESULT GlassShell_CreateToggleExplorerCommand(REFIID riid, void** ppv) noexcept;

[[nodiscard]] HRESULT GlassShell_CreateOpenConfigExplorerCommand(REFIID riid, void** ppv) noexcept;

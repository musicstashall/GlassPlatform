#pragma once

namespace GlassCore::CtlColorProbe
{
    // BWM FormsHooks::SetBkColorAttach — ref-count Detour gdi32!SetBkColor на время WM_CTLCOLORSTATIC/LISTBOX.
    void SetBkColorProbeAttach(bool enable) noexcept;
    void Shutdown() noexcept;
}

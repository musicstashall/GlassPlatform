#pragma once

namespace GlassCore::Hooks::MenuHooks
{
    void Prepare() noexcept;
    void Startup() noexcept;
    void Shutdown() noexcept;

    /// <summary>Синхронизировать icon-hooks с Menu\NoModernAppBackgroundColor (default ON).</summary>
    void SyncIconHooksFromRegistry() noexcept;
}

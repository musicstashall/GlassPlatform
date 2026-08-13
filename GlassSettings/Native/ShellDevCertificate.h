#pragma once

#include <cstddef>

namespace ShellDevCertificate
{
    [[nodiscard]] bool EnsureGlassShellDevCertificateTrusted() noexcept;

    [[nodiscard]] bool GetGlassShellDevCertThumbprint(wchar_t* out, size_t cch) noexcept;
}

#pragma once

#include <Windows.h>

namespace GlassCompat
{
    namespace ShortLivedAgentDetail
    {
        [[nodiscard]] inline bool CmdLineHasMainSwitch(LPCWSTR cmd, LPCWSTR switchToken) noexcept
        {
            if (!cmd || !switchToken)
                return false;

            const wchar_t* mainTag = wcsstr(cmd, L",Main");
            if (!mainTag)
                return false;

            return wcsstr(mainTag, switchToken) != nullptr;
        }
    }

    [[nodiscard]] inline bool IsShortLivedGlassCompatProcess() noexcept
    {
        const wchar_t* cmd = ::GetCommandLineW();
        if (!cmd)
            return false;

        using ShortLivedAgentDetail::CmdLineHasMainSwitch;

        if (CmdLineHasMainSwitch(cmd, L"/stop") || CmdLineHasMainSwitch(cmd, L"-stop"))
            return true;

        static const wchar_t* const kReloadSwitches[] = {
            L"/reloadRenderTaskbarSwca",
            L"-reloadRenderTaskbarSwca",
            L"/reloadRenderTaskbarXaml",
            L"-reloadRenderTaskbarXaml",
            L"/reloadRenderTaskbarBoth",
            L"-reloadRenderTaskbarBoth",
            L"/reloadRenderCommit",
            L"-reloadRenderCommit",
            L"/reloadRender",
            L"-reloadRender",
        };

        for (const wchar_t* sw : kReloadSwitches)
        {
            if (CmdLineHasMainSwitch(cmd, sw))
                return true;
        }

        if (CmdLineHasMainSwitch(cmd, L"/shellStartPlatform") ||
            CmdLineHasMainSwitch(cmd, L"-shellStartPlatform"))
        {
            return true;
        }

        return false;
    }
}
